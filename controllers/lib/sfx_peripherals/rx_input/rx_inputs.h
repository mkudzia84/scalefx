/*
 * RxInputs — Unified RC receiver input template
 *
 * Wraps any ChannelSource and provides a uniform API for querying
 * channel values, detecting signal loss, and applying failsafe defaults.
 *
 * Supported ChannelSource implementations:
 *   - PpmDecoder          PPM sum on a single pin (ESP32 RMT / Pico ISR)
 *   - PwmCollection       Individual PWM wires, one per channel
 *   - SbusInput           Futaba S.Bus (inverted UART, 100000 8E2)
 *   - JetiExChannelSource Adapter bridging JetiExBus → ChannelSource
 *
 * Usage:
 *
 *   // PPM receiver on a single pin:
 *   RxInputs<PpmDecoder> rx;
 *   rx.source().begin(PIN_PPM_IN, true);
 *   rx.update();
 *   uint16_t throttle = rx.channel_us(3);
 *
 *   // PWM receiver on individual pins:
 *   RxInputs<PwmCollection> rx;
 *   int pins[] = {4, 5, 6, 7, 8, 9};
 *   rx.source().begin(pins, 6);
 *   rx.update();
 *   uint16_t aileron = rx.channel_us(1);
 *
 *   // Futaba S.Bus (user configures inverted UART externally):
 *   //   ESP32: Serial1.begin(100000, SERIAL_8E2, RX, -1, true);
 *   //   Pico:  Serial1.setRX(RX); Serial1.setInvert(true,false,false);
 *   //          Serial1.begin(100000, SERIAL_8E2);
 *   RxInputs<SbusInput> rx;
 *   rx.source().begin(&Serial1);
 *   rx.update();
 *   bool failsafe = rx.source().failsafe();
 *
 *   // Jeti EX Bus (channel data via adapter, bus owned externally):
 *   JetiExBus jeti;
 *   jeti.begin(&Serial2);
 *   jeti.setSensorInfo(0xA4CB, 0x0001, "ScaleFX");
 *   RxInputs<JetiExChannelSource> rx;
 *   rx.source().attach(&jeti);
 *   jeti.update();          // protocol + channels + telemetry
 *   rx.update();            // signal-loss detection + helpers
 *   float aileron = rx.channelNorm(1);
 *
 * ChannelSource concept (duck-typed — no virtual ABC):
 *   void         update()
 *   void         end()
 *   uint16_t     channel_us(uint8_t ch)   — 1-based
 *   uint8_t      channelCount()
 *   bool         isValid()
 *   bool         isInitialized()
 */

#ifndef SFX_RX_INPUTS_H
#define SFX_RX_INPUTS_H

#include <cstdint>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include "rx_common.h"

template <typename TSource>
class RxInputs {
public:
    RxInputs() = default;
    ~RxInputs() { end(); }

    RxInputs(const RxInputs&) = delete;
    RxInputs& operator=(const RxInputs&) = delete;

    // ════════════════════════════════════════════════════════════
    //  Source access — call source().begin(...) to initialize
    // ════════════════════════════════════════════════════════════

    TSource&       source()       { return _source; }
    const TSource& source() const { return _source; }

    // ════════════════════════════════════════════════════════════
    //  Lifecycle
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Update the source and detect signal loss
     *
     * Call this regularly (every loop iteration).  If the source
     * reports valid data, the last-good timestamp is refreshed.
     */
    void update() {
        _source.update();
        if (_source.isValid()) {
            _lastValid_ms = SFX_MILLIS();
            _signalLost   = false;
        } else if (_lastValid_ms > 0 &&
                   (SFX_MILLIS() - _lastValid_ms) > RxConfig::SIGNAL_TIMEOUT_MS)
        {
            _signalLost = true;
        }
    }

    /**
     * @brief Stop the source
     */
    void end() {
        _source.end();
        _signalLost   = false;
        _lastValid_ms = 0;
    }

    // ════════════════════════════════════════════════════════════
    //  Channel queries (ChannelSource pass-through)
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Get channel pulse width
     * @param ch  1-based channel number
     * @return Pulse width in µs.  Returns failsafe (CENTER_US) when
     *         signal is lost or channel is out of range.
     */
    uint16_t channel_us(uint8_t ch) const {
        if (_signalLost) return RxConfig::CENTER_US;
        return _source.channel_us(ch);
    }

    /**
     * @brief Number of active channels from the source
     */
    uint8_t channelCount() const { return _source.channelCount(); }

    /**
     * @brief Source has valid fresh data
     */
    bool isValid() const { return _source.isValid() && !_signalLost; }

    /**
     * @brief Source begin() was called
     */
    bool isInitialized() const { return _source.isInitialized(); }

    // ════════════════════════════════════════════════════════════
    //  Signal health
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Signal lost (no valid frame for > SIGNAL_TIMEOUT_MS)
     */
    bool signalLost() const { return _signalLost; }

    /**
     * @brief Milliseconds since last valid frame (0 if never received)
     */
    uint32_t timeSinceValid_ms() const {
        if (_lastValid_ms == 0) return 0;
        return SFX_MILLIS() - _lastValid_ms;
    }

    // ════════════════════════════════════════════════════════════
    //  Convenience helpers
    // ════════════════════════════════════════════════════════════

    /**
     * @brief Get channel as normalized float [-1.0 … +1.0]
     *
     * Maps MIN_PULSE_US → -1.0, CENTER_US → 0.0, MAX_PULSE_US → +1.0
     * @param ch 1-based channel
     */
    float channelNorm(uint8_t ch) const {
        int val = static_cast<int>(channel_us(ch));
        float norm = (val - static_cast<int>(RxConfig::CENTER_US))
                   / static_cast<float>(RxConfig::MAX_PULSE_US - RxConfig::CENTER_US);
        if (norm < -1.0f) norm = -1.0f;
        if (norm >  1.0f) norm =  1.0f;
        return norm;
    }

    /**
     * @brief Get channel as unsigned percent [0 … 100]
     *
     * Maps MIN_PULSE_US → 0, MAX_PULSE_US → 100
     * @param ch 1-based channel
     */
    uint8_t channelPct(uint8_t ch) const {
        int val   = static_cast<int>(channel_us(ch));
        int range = RxConfig::MAX_PULSE_US - RxConfig::MIN_PULSE_US;
        int pct   = ((val - static_cast<int>(RxConfig::MIN_PULSE_US)) * 100) / range;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        return static_cast<uint8_t>(pct);
    }

private:
    TSource  _source;
    uint32_t _lastValid_ms = 0;
    bool     _signalLost   = false;
};

#endif // SFX_RX_INPUTS_H
