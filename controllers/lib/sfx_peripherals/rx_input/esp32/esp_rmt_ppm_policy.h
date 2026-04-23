/*
 * ESP32 RMT PPM Policy — Hardware PPM capture via RMT peripheral
 *
 * Uses ESP-IDF v5.x RMT RX driver for zero-CPU pulse timing.
 * The RMT peripheral captures pulse durations in hardware and delivers
 * them via a FreeRTOS queue — no interrupts in user code.
 *
 * Compatible with ESP-IDF 5.1+ (rmt_new_rx_channel / rmt_receive API).
 *
 * Do not include directly — included from ppm_input.h via platform guard.
 */

#ifndef SFX_ESP_RMT_PPM_POLICY_H
#define SFX_ESP_RMT_PPM_POLICY_H

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <driver/rmt_rx.h>
#include "../rx_common.h"

class EspRmtPpmPolicy {
public:
    EspRmtPpmPolicy() = default;
    ~EspRmtPpmPolicy() { end(); }

    EspRmtPpmPolicy(const EspRmtPpmPolicy&) = delete;
    EspRmtPpmPolicy& operator=(const EspRmtPpmPolicy&) = delete;

    /**
     * @brief Start RMT RX channel for PPM capture
     * @param pin         GPIO connected to PPM output of receiver
     * @param risingEdge  true = rising edge marks channel boundary
     * @return true if RMT channel created and receiving
     */
    bool begin(int pin, bool risingEdge);

    /**
     * @brief Stop RMT channel and release resources
     */
    void end();

    /**
     * @brief Non-blocking: check if a complete PPM frame is available
     *
     * Reads the RMT receive queue (timeout 0).  When raw symbols arrive,
     * scans for the sync gap (> PPM_SYNC_US) and extracts channel widths.
     *
     * @param pulses_us  Output array of channel pulse widths (µs)
     * @param maxCh      Size of pulses_us array
     * @param outCount   Number of channels decoded
     * @return true if a complete frame was decoded this call
     */
    bool readFrame(uint16_t* pulses_us, uint8_t maxCh, uint8_t& outCount);

private:
    // RMT handles
    rmt_channel_handle_t   _rxChannel = nullptr;
    rmt_receive_config_t   _rxConfig  = {};
    QueueHandle_t          _rxQueue   = nullptr;

    // Raw symbol buffer (RMT delivers rmt_rx_done_event_data_t via queue)
    static constexpr size_t RAW_SYMBOL_BUF_SIZE = 64;
    rmt_symbol_word_t      _rawSymbols[RAW_SYMBOL_BUF_SIZE] = {};

    bool _risingEdge = true;
    bool _receiving  = false;

    // Re-arm RMT receive after consuming symbols
    void rearm();

    // RMT done callback (static, bridges to FreeRTOS queue)
    static bool IRAM_ATTR rmtDoneCallback(
        rmt_channel_handle_t channel,
        const rmt_rx_done_event_data_t* edata,
        void* user_ctx);
};

#endif // SFX_PLATFORM_ESP32
#endif // SFX_ESP_RMT_PPM_POLICY_H
