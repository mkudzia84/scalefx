/*
 * PPM Input — Templatized PPM decoder with platform-specific policy
 *
 * Decodes a standard PPM sum signal (all channels multiplexed on one pin).
 * Platform-specific capture is delegated to a TPolicy class:
 *
 *   - EspRmtPpmPolicy   — ESP32 RMT hardware (zero-CPU capture)
 *   - PicoIsrPpmPolicy  — RP2040/RP2350 GPIO interrupt (edge-timing)
 *
 * Policy concept (duck-typed):
 *   bool begin(int pin, bool risingEdge)  — start hardware capture
 *   void end()                            — stop hardware capture
 *   bool readFrame(uint16_t* pulses_us, uint8_t maxCh, uint8_t& outCount)
 *        — non-blocking: returns true when a complete frame is available,
 *          writes channel pulse widths into pulses_us[0..outCount-1].
 *
 * Usage:
 *   PpmInput<EspRmtPpmPolicy> ppm;
 *   ppm.begin(4);                    // GPIO 4
 *   ppm.update();                    // call in loop
 *   uint16_t ch1 = ppm.channel_us(1);
 *
 * Auto-selected alias at bottom of file:
 *   using PpmDecoder = PpmInput<platform-specific policy>;
 */

#ifndef SFX_PPM_INPUT_H
#define SFX_PPM_INPUT_H

#include "rx_common.h"
#include "platform/sfx_platform.h"

template <typename TPolicy>
class PpmInput {
public:
    PpmInput() = default;
    ~PpmInput() { end(); }

    // Non-copyable (hardware resource ownership)
    PpmInput(const PpmInput&) = delete;
    PpmInput& operator=(const PpmInput&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Start PPM decoding on the given GPIO pin
     * @param pin         GPIO pin connected to PPM signal
     * @param risingEdge  true = rising edge starts channel pulse (most TX)
     * @return true if hardware capture started
     */
    bool begin(int pin, bool risingEdge = RxConfig::PPM_RISING_EDGE) {
        if (pin < 0) return false;
        _pin = pin;
        _initialized = _policy.begin(pin, risingEdge);
        _lastFrameMs = 0;
        _channelCount = 0;
        memset(_channels_us, 0, sizeof(_channels_us));
        return _initialized;
    }

    /**
     * @brief Stop PPM decoding and release hardware resources
     */
    void end() {
        if (_initialized) {
            _policy.end();
            _initialized = false;
        }
        _channelCount = 0;
        _pin = -1;
    }

    // ========================================================================
    // Update (call in loop)
    // ========================================================================

    /**
     * @brief Poll the policy for a new complete PPM frame
     *
     * Non-blocking. When a frame is ready, updates channel values
     * and resets the signal-loss timer.
     */
    void update() {
        if (!_initialized) return;

        uint16_t temp[RxConfig::MAX_CHANNELS];
        uint8_t count = 0;

        if (_policy.readFrame(temp, RxConfig::MAX_CHANNELS, count)) {
            // Validate and store
            _channelCount = count;
            for (uint8_t i = 0; i < count; i++) {
                _channels_us[i] = temp[i];
            }
            _lastFrameMs = millis();
        }
    }

    // ========================================================================
    // ChannelSource interface
    // ========================================================================

    /**
     * @brief Get channel value in µs
     * @param ch 1-based channel index
     * @return Pulse width in µs, or RxConfig::CENTER_US if invalid/unavailable
     */
    uint16_t channel_us(uint8_t ch) const {
        if (ch < 1 || ch > _channelCount) return RxConfig::CENTER_US;
        return _channels_us[ch - 1];
    }

    /** @brief Number of channels decoded in the last valid frame */
    uint8_t channelCount() const { return _channelCount; }

    /** @brief true if a valid frame was received within SIGNAL_TIMEOUT_MS */
    bool isValid() const {
        if (_lastFrameMs == 0 || _channelCount == 0) return false;
        return (millis() - _lastFrameMs) < RxConfig::SIGNAL_TIMEOUT_MS;
    }

    /** @brief true if begin() succeeded */
    bool isInitialized() const { return _initialized; }

    /** @brief GPIO pin in use (-1 if not started) */
    int pin() const { return _pin; }

    /** @brief Direct policy access for advanced / diagnostic use */
    TPolicy&       policy()       { return _policy; }
    const TPolicy& policy() const { return _policy; }

private:
    TPolicy  _policy;
    int      _pin = -1;
    bool     _initialized = false;
    uint8_t  _channelCount = 0;
    uint16_t _channels_us[RxConfig::MAX_CHANNELS] = {};
    uint32_t _lastFrameMs = 0;
};

// ============================================================================
// Platform auto-select
// ============================================================================

#if SFX_PLATFORM_ESP32
#include "esp32/esp_rmt_ppm_policy.h"
using PpmDecoder = PpmInput<EspRmtPpmPolicy>;
#elif SFX_PLATFORM_PICO
#include "pico/pico_isr_ppm_policy.h"
using PpmDecoder = PpmInput<PicoIsrPpmPolicy>;
#endif

#endif // SFX_PPM_INPUT_H
