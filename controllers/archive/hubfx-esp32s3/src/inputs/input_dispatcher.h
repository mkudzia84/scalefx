/*
 * InputDispatcher — RC input → HubFX feature router (Phase 0)
 *
 * Reads RC channels from a single configurable source (PWM-per-pin, PPM sum,
 * S.Bus, or Jeti EX Bus) and dispatches edge-triggered events to hub-side
 * features (engine on/off, light program selector, future trigger fanout to
 * slave boards).
 *
 * The reader source is selected at runtime from `inputs.mode` in /hubfx.yaml
 * but only one reader is `begin()`'d per boot — the inactive ones live as
 * default-constructed members and consume nothing past their footprint. This
 * keeps the wiring simple while still letting the user flip modes without
 * reflashing.
 *
 * Pin alias: I1..I7 = GPIO 5,6,7,10,11,12,13. The 1-based YAML `channel`
 * field maps directly to the I-pin index in gpio mode and to the protocol
 * channel number in ppm/sbus/jeti modes.
 *
 * SBUS and Jeti are stubs in Phase 0 — begin() returns false and the
 * dispatcher logs a warning. They share the same call shape so adding the
 * real readers later is a one-line wire-up.
 */

#ifndef HUBFX_INPUT_DISPATCHER_H
#define HUBFX_INPUT_DISPATCHER_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <platform/diag_log.h>
#include <rx_input/rx_inputs.h>
#include <rx_input/ppm_input.h>
#include <rx_input/pwm_collection.h>

#include "../config/hubfx_settings.h"

class InputDispatcher {
public:
    /// Pin alias for the seven RC-input headers, 1-based (I1=index 0, …).
    static constexpr uint8_t  PIN_COUNT = 7;
    static constexpr int      PINS[PIN_COUNT] = { 5, 6, 7, 10, 11, 12, 13 };

    using EngineToggleCb     = std::function<void(uint16_t value_us, bool engaged)>;
    using LightProgramCb     = std::function<void(uint8_t programIndex)>;

    InputDispatcher() = default;

    InputDispatcher(const InputDispatcher&) = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;

    /**
     * @brief Configure callbacks. Safe to call before / after begin().
     */
    void onEngineToggle(EngineToggleCb cb) { _engineCb = std::move(cb); }
    void onLightProgram(LightProgramCb cb) { _lightCb  = std::move(cb); }

    /**
     * @brief Initialize the active reader and cache feature mappings.
     *
     * Re-callable: tears down any previous reader, applies the new config.
     * Returns true if the configured reader started cleanly (or `none`).
     */
    bool begin(const HubFxInputs& cfg) {
        end();
        _cfg = cfg;

        if (strcmp(cfg.mode, HubFxInputMode::NONE) == 0) {
            _mode = Mode::Disabled;
            SFX_LOG_INFO("[Inputs] mode=none — RC input dispatch disabled");
            return true;
        }
        if (strcmp(cfg.mode, HubFxInputMode::GPIO) == 0) {
            _mode = Mode::Gpio;
            // PwmCollection takes pins in 1-based channel order. We map every
            // I-pin so the YAML `channel` field == 1-based I-pin number.
            int pins[PIN_COUNT];
            for (uint8_t i = 0; i < PIN_COUNT; i++) pins[i] = PINS[i];
            bool ok = _pwm.source().begin(pins, PIN_COUNT);
            if (!ok) {
                SFX_LOG_ERROR("[Inputs] mode=gpio — PwmCollection.begin() failed");
            } else {
                SFX_LOG_INFO("[Inputs] mode=gpio — %u PWM channels on I1..I7", PIN_COUNT);
            }
            return ok;
        }
        if (strcmp(cfg.mode, HubFxInputMode::PPM) == 0) {
            _mode = Mode::Ppm;
            int pin = pinFor(cfg.ppmPin);
            if (pin < 0) {
                SFX_LOG_ERROR("[Inputs] mode=ppm — invalid ppm_pin=%u (use 1..7)", cfg.ppmPin);
                _mode = Mode::Disabled;
                return false;
            }
            bool ok = _ppm.source().begin(pin);
            if (!ok) {
                SFX_LOG_ERROR("[Inputs] mode=ppm — PpmDecoder.begin(GPIO%d) failed", pin);
            } else {
                SFX_LOG_INFO("[Inputs] mode=ppm — listening on I%u (GPIO%d)", cfg.ppmPin, pin);
            }
            return ok;
        }
        if (strcmp(cfg.mode, HubFxInputMode::SBUS) == 0 ||
            strcmp(cfg.mode, HubFxInputMode::JETI) == 0)
        {
            _mode = Mode::Disabled;
            SFX_LOG_WARN("[Inputs] mode=%s — not yet implemented (Phase 0 stub)", cfg.mode);
            return false;
        }

        _mode = Mode::Disabled;
        SFX_LOG_WARN("[Inputs] unknown mode '%s' — disabling", cfg.mode);
        return false;
    }

    /// Tear down any active reader and reset edge-detection state.
    void end() {
        switch (_mode) {
            case Mode::Gpio: _pwm.end(); break;
            case Mode::Ppm:  _ppm.end(); break;
            default: break;
        }
        _mode = Mode::Disabled;
        _engineLastEngaged = false;
        _engineLastSeen    = false;
        _lightLastIndex    = 0xFF;
    }

    /**
     * @brief Pump the active reader and dispatch any feature changes.
     *
     * Call from loop() at >50 Hz for responsive edge detection. The reader's
     * own update() is non-blocking so this is cheap.
     */
    void process() {
        switch (_mode) {
            case Mode::Disabled: return;
            case Mode::Gpio:     _pwm.update(); break;
            case Mode::Ppm:      _ppm.update(); break;
        }

        // ---- Engine on/off (PWM threshold with hysteresis-free edge) ----
        if (_cfg.engineOnOff.channel > 0) {
            uint16_t v = readChannel_us(_cfg.engineOnOff.channel);
            bool engaged = (v >= _cfg.engineOnOff.threshold_us);
            if (!_engineLastSeen || engaged != _engineLastEngaged) {
                _engineLastSeen    = true;
                _engineLastEngaged = engaged;
                if (_engineCb) _engineCb(v, engaged);
            }
        }

        // ---- Light program selector (PWM range → 0..N-1 program index) ----
        if (_cfg.lightProgramSelect.channel > 0 &&
            _cfg.lightProgramSelect.programCount > 0)
        {
            uint16_t v   = readChannel_us(_cfg.lightProgramSelect.channel);
            uint8_t  idx = pwmToIndex(v, _cfg.lightProgramSelect.programCount);
            if (idx != _lightLastIndex) {
                _lightLastIndex = idx;
                if (_lightCb) _lightCb(idx);
            }
        }
    }

    /// Reader is initialized and currently producing valid frames.
    bool hasSignal() const {
        switch (_mode) {
            case Mode::Gpio: return _pwm.isValid();
            case Mode::Ppm:  return _ppm.isValid();
            default:         return false;
        }
    }

    /// Snapshot of channel pulse width in µs (0 if disabled / out of range).
    uint16_t readChannel_us(uint8_t channel) const {
        if (channel == 0) return 0;
        switch (_mode) {
            case Mode::Gpio:
                if (channel > PIN_COUNT) return 0;
                return _pwm.channel_us(channel);
            case Mode::Ppm:
                return _ppm.channel_us(channel);
            default:
                return 0;
        }
    }

private:
    enum class Mode { Disabled, Gpio, Ppm };

    /// 1-based I-pin index → GPIO number, or -1 on error.
    static int pinFor(uint8_t iPin) {
        if (iPin < 1 || iPin > PIN_COUNT) return -1;
        return PINS[iPin - 1];
    }

    /// Map a PWM µs reading uniformly across N program slots.
    /// Pulses below MIN/above MAX are clamped to the first / last slot.
    static uint8_t pwmToIndex(uint16_t us, uint8_t slots) {
        if (slots <= 1) return 0;
        constexpr uint16_t MIN = RxConfig::MIN_PULSE_US;
        constexpr uint16_t MAX = RxConfig::MAX_PULSE_US;
        if (us <= MIN) return 0;
        if (us >= MAX) return uint8_t(slots - 1);
        uint32_t span = uint32_t(MAX - MIN);
        uint32_t pos  = uint32_t(us  - MIN) * uint32_t(slots);
        uint8_t  idx  = uint8_t(pos / span);
        if (idx >= slots) idx = uint8_t(slots - 1);
        return idx;
    }

    Mode                    _mode = Mode::Disabled;
    HubFxInputs             _cfg;
    RxInputs<PpmDecoder>    _ppm;     // active when mode=ppm
    RxInputs<PwmCollection> _pwm;     // active when mode=gpio

    EngineToggleCb _engineCb;
    LightProgramCb _lightCb;

    // Edge-detection state — initialized to "no observation yet"
    bool    _engineLastSeen    = false;
    bool    _engineLastEngaged = false;
    uint8_t _lightLastIndex    = 0xFF;  // never matches a real index
};

#endif // HUBFX_INPUT_DISPATCHER_H
