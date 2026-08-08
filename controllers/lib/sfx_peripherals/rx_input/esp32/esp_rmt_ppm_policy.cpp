/*
 * ESP32 RMT PPM Policy — implementation
 *
 * RMT RX captures a stream of pulse durations in hardware.
 * When the receive completes (idle timeout = sync gap) the driver
 * delivers the raw symbol array through a FreeRTOS queue.
 *
 * readFrame() polls the queue (no block), scans for the sync gap,
 * and extracts channel pulse widths.
 */

// Pull in platform detection (defines SFX_PLATFORM_ESP32) before the guard.
// Without this, the macro is undefined → 0 → the whole body is skipped and
// every linkage symbol declared in the .h becomes undefined at link time.
#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32

#include "esp_rmt_ppm_policy.h"
#include <string.h>
#include <serial/diag_log.h>   // SFX_LOG_INFO — sync-skip pulse diagnostics

// ─── RMT done callback (ISR context) ───────────────────────────
bool IRAM_ATTR EspRmtPpmPolicy::rmtDoneCallback(
    rmt_channel_handle_t channel,
    const rmt_rx_done_event_data_t* edata,
    void* user_ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    auto* self = static_cast<EspRmtPpmPolicy*>(user_ctx);
    // Push the event data pointer into the queue
    xQueueSendFromISR(self->_rxQueue, edata, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}

// ─── begin ─────────────────────────────────────────────────────
bool EspRmtPpmPolicy::begin(int pin, bool risingEdge)
{
    if (_rxChannel) end();

    _risingEdge = risingEdge;

    // FreeRTOS queue for RMT done events (depth 1 — we process before next)
    _rxQueue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!_rxQueue) return false;

    // RMT RX channel config
    rmt_rx_channel_config_t rxCfg = {};
    rxCfg.gpio_num          = static_cast<gpio_num_t>(pin);
    rxCfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    rxCfg.resolution_hz     = 1'000'000;  // 1 µs resolution
    rxCfg.mem_block_symbols = RAW_SYMBOL_BUF_SIZE;

    esp_err_t err = rmt_new_rx_channel(&rxCfg, &_rxChannel);
    if (err != ESP_OK) {
        vQueueDelete(_rxQueue);
        _rxQueue = nullptr;
        return false;
    }

    // Register done callback
    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmtDoneCallback;
    err = rmt_rx_register_event_callbacks(_rxChannel, &cbs, this);
    if (err != ESP_OK) {
        rmt_del_channel(_rxChannel);
        _rxChannel = nullptr;
        vQueueDelete(_rxQueue);
        _rxQueue = nullptr;
        return false;
    }

    // Enable channel
    err = rmt_enable(_rxChannel);
    if (err != ESP_OK) {
        rmt_del_channel(_rxChannel);
        _rxChannel = nullptr;
        vQueueDelete(_rxQueue);
        _rxQueue = nullptr;
        return false;
    }

    // Receive config: min pulse 400µs, idle threshold = sync gap
    _rxConfig.signal_range_min_ns = 400'000;   // 400µs — reject glitches
    _rxConfig.signal_range_max_ns = 0;         // no max filtering

    // Start first receive
    rearm();
    _receiving = true;
    return true;
}

// ─── end ───────────────────────────────────────────────────────
void EspRmtPpmPolicy::end()
{
    if (_rxChannel) {
        rmt_disable(_rxChannel);
        rmt_del_channel(_rxChannel);
        _rxChannel = nullptr;
    }
    if (_rxQueue) {
        vQueueDelete(_rxQueue);
        _rxQueue = nullptr;
    }
    _receiving = false;
}

// ─── rearm ─────────────────────────────────────────────────────
void EspRmtPpmPolicy::rearm()
{
    if (!_rxChannel) return;
    rmt_receive(_rxChannel, _rawSymbols, sizeof(_rawSymbols), &_rxConfig);
}

// ─── readFrame ─────────────────────────────────────────────────
bool EspRmtPpmPolicy::readFrame(uint16_t* pulses_us, uint8_t maxCh, uint8_t& outCount)
{
    outCount = 0;
    if (!_rxChannel || !_rxQueue) return false;

    rmt_rx_done_event_data_t rxData;
    if (xQueueReceive(_rxQueue, &rxData, 0) != pdTRUE) {
        return false;  // No frame ready
    }

    // Parse RMT symbols into channel pulse widths.
    // Each rmt_symbol_word_t has two durations (level0+dur0, level1+dur1).
    // PPM: each channel = one pulse of the active level + gap to next.
    // The channel width = time from rising edge to next rising edge
    // (or falling-to-falling if inverted).
    //
    // Strategy: accumulate dur0+dur1 for each symbol as one PPM period.
    // If the accumulated period > SYNC_US, it's the sync gap — skip it.

    const size_t numSymbols = rxData.num_symbols;
    const rmt_symbol_word_t* symbols = rxData.received_symbols;

    uint8_t ch = 0;
    for (size_t i = 0; i < numSymbols && ch < maxCh; i++) {
        // Total width of this symbol pair (active pulse + gap)
        uint32_t width_us;

        if (_risingEdge) {
            // Rising edge mode: level0 should be HIGH (the pulse)
            // duration0 = active pulse, duration1 = gap to next rising edge
            // Total period from rising to next rising = dur0 + dur1
            width_us = symbols[i].duration0 + symbols[i].duration1;
        } else {
            width_us = symbols[i].duration0 + symbols[i].duration1;
        }

        if (width_us == 0) continue;

        // Sync gap detection
        if (width_us >= RxConfig::PPM_SYNC_US) {
            // Diagnostic (rate-limited ~1 Hz): a repeating sync-only signal is
            // NOT PPM — e.g. a plain servo pulse train (one 1–2 ms pulse per
            // 20 ms frame) looks like nothing but sync gaps here.  Log the raw
            // HIGH/LOW split so the bench can measure a looped-back servo
            // header with no scope (the 2026-08-08 SRV→INP self-scope).
            {
                static uint32_t s_lastPulseLogMs = 0;
                const uint32_t now = SFX_MILLIS();
                if (now - s_lastPulseLogMs >= 1000) {
                    s_lastPulseLogMs = now;
                    SFX_LOG_INFO("[ppm] sync-skip sym: high=%uus low=%uus (lvl %u/%u)",
                                 (unsigned)symbols[i].duration0,
                                 (unsigned)symbols[i].duration1,
                                 (unsigned)symbols[i].level0,
                                 (unsigned)symbols[i].level1);
                }
            }
            // Sync — channels collected so far form the frame
            if (ch > 0) break;
            continue;  // Sync at start, skip
        }

        // Valid channel pulse
        if (width_us >= RxConfig::MIN_PULSE_US && width_us <= RxConfig::MAX_PULSE_US) {
            pulses_us[ch++] = static_cast<uint16_t>(width_us);
        }
    }

    // Re-arm for next frame
    rearm();

    if (ch > 0) {
        outCount = ch;
        return true;
    }
    return false;
}

#endif // SFX_PLATFORM_ESP32
