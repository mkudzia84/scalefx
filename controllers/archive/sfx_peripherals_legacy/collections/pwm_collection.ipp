/*
 * PwmCollection<N, TSense>::* — implementation skeleton.
 *
 * THIS FILE IS A FIRST CUT.  The actual PWM driving requires a generic
 * PwmOutput abstraction in lib/sfx_peripherals/pwm/ — the existing
 * pwm_control.h is for RC PWM INPUT measurement, not output.  Until
 * that lands, the methods here update the in-RAM state machine
 * (mode / duty / freq mirrors) but only emit `analogWrite()`-equivalent
 * output via a thin platform shim.  Sensing (voltage / current) is
 * routed through whatever ISenseProvider the board firmware supplies.
 *
 * What's solid here:
 *   - Mode-transition state machine + LedCollection coupling
 *     (PwmLed mode hand-off via writeDuty() — wrapped by PwmDutyAdapter
 *      callbacks)
 *   - Reconfigure atomicity (applyRuntimeConfig is the single mutator)
 *   - Bounds + capability-flag validation
 *
 * What needs another pass:
 *   - Actual PWM output emission (currently calls analogWrite as a
 *     placeholder; should go through a typed PwmOutput driver)
 *   - Frequency reconfiguration path (depends on the same driver)
 *   - Heater closed-loop bang-bang using the thermistor channel
 */

#ifndef SFX_PWM_COLLECTION_IPP
#define SFX_PWM_COLLECTION_IPP

#include <Arduino.h>

#include "pwm_collection.h"

namespace sfx_peripherals {

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::attach() {
    if (_attached) return true;
    for (size_t i = 0; i < N; i++) {
        const auto& s = _specs[i];

        // Native PWM init (placeholder — proper sfx_peripherals/pwm/
        // PwmOutput driver is the longer-term target).  Expander-
        // attached PWM pins still need their own code path; the
        // default mode for those should not be PwmGeneric until that
        // path lands.
        if (!s.useExpander) {
            pinMode(s.pin, OUTPUT);
            analogWrite(s.pin, 0);
            // analogWriteFreq is supported on Arduino-Pico (RP2040) and
            // ESP32 Arduino core — apply the spec's default frequency.
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
            analogWriteFreq(s.defaultFreq_Hz);
#endif
        }

        // DcMotor driver — initialised for every channel that COULD
        // be a motor (the wiring is fixed at compile time even if the
        // active mode swaps at runtime).  A PwmGeneric channel that
        // never enters PwmMotor mode just keeps its DcMotor instance
        // detached without cost.
        if (s.hwFlags & PwmFlags::MODE_MUTABLE) {
            sfx_peripherals::DcMotorConfig mcfg{};
            mcfg.topology       = s.motorTopology;
            mcfg.pwm_pin        = s.pin;
            mcfg.cw_pin         = s.motorCwPin;
            mcfg.ccw_pin        = s.motorCcwPin;
            mcfg.invert_dir     = s.motorInvertDir;
            mcfg.brake_capable  = s.motorBrakeCapable;
            _motors[i].begin(mcfg);
        }

        _runtime[i].mode     = s.defaultMode;
        _runtime[i].freq_Hz  = s.defaultFreq_Hz;
        _runtime[i].cfgFlags = 0;
        _runtime[i].maxDuty  = 1000;
        _duties[i]           = 0;
        _motorSpeeds[i]      = 0;

        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::Activated, 0);
    }
    _attached = true;
    return true;
}

template <size_t N, SensePolicy TSense>
void PwmCollection<N, TSense>::detach() {
    if (!_attached) return;
    for (size_t i = 0; i < N; i++) {
        if (_motors[i].isAttached())   _motors[i].end();
        if (!_specs[i].useExpander)    analogWrite(_specs[i].pin, 0);
        _duties[i]      = 0;
        _motorSpeeds[i] = 0;
        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::Deactivated, 0);
    }
    _attached = false;
}

template <size_t N, SensePolicy TSense>
void PwmCollection<N, TSense>::update() {
    if (!_attached) return;
    // Drive periodic sensor sampling work (no-op for NoSensing).
    _sense.update();

    // Per-channel stall guard — delegates the detection state machine
    // to `sfx_peripherals::StallDetector` (one instance per channel).
    // The collection owns only the protocol-level policy (flags,
    // latch, peak tracking, async event emission).
    for (size_t i = 0; i < N; i++) {
        StallGuard& g = _stallGuards[i];
        if (!(g.flags & ComponentPacket::StallFlags::ENABLED)) continue;
        if (g.threshold_mA == 0 || g.latched) continue;
        if (_runtime[i].mode != ComponentKind::PwmMotor) continue;
        if (!_stallDetectors[i].isActive())              continue;

        int32_t current_mA = 0;
        if (!_sense.readCurrent_mA((uint8_t)i, current_mA)) continue;
        uint16_t mag = (uint16_t)((current_mA < 0) ? -current_mA : current_mA);
        if (mag > g.peak_mA) g.peak_mA = mag;

        const auto result = _stallDetectors[i].update(mag);
        if (result == StallDetector::Result::RUNNING) continue;

        // Detector reached a terminal state — emit event + apply policy.
        // Map StallDetector results onto the wire-format peak/duration
        // fields.  duration_ms is approximate (the detector's confirm
        // window for STALL_CONFIRMED, or its full timeout for the
        // TIMEOUT_* results).
        const auto& cfg = _stallDetectors[i].config();
        uint16_t duration_ms =
            (result == StallDetector::Result::STALL_CONFIRMED) ? cfg.stallConfirm_ms
                                                                : cfg.timeout_ms;
        if (_onStall) _onStall((uint8_t)i, g.peak_mA, duration_ms);
        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::StallDetected, g.peak_mA);
        if (g.flags & ComponentPacket::StallFlags::AUTO_STOP) {
            // Stop motor — DcMotor brakes if AUTO_STOP+BRAKE_ON_STOP both set.
            const bool brake = (g.flags & ComponentPacket::StallFlags::BRAKE_ON_STOP) != 0;
            if (_motors[i].isAttached()) _motors[i].setSpeed(0, brake);
            else                          setDuty((uint8_t)i, 0);
            _motorSpeeds[i] = 0;
            if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::MotionEnded, 0);
        }
        if (g.flags & ComponentPacket::StallFlags::LATCH) {
            g.latched = true;
        }
        g.peak_mA = 0;
    }
}

template <size_t N, SensePolicy TSense>
void PwmCollection<N, TSense>::allOff() {
    for (size_t i = 0; i < N; i++) {
        if (_motors[i].isAttached()) _motors[i].stop();
        else if (_attached && !_specs[i].useExpander) {
            analogWrite(_specs[i].pin, 0);
        }
        _duties[i]      = 0;
        _motorSpeeds[i] = 0;
        if (_stallDetectors[i].isActive()) _stallDetectors[i].stop();
        // Mode flags + cfgFlags preserved per safe-state contract.
        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::SafeStateEntered, 0);
    }
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::setMode(uint8_t idx, ComponentKind newKind) {
    if (idx >= N) return false;
    PwmRuntimeConfig cfg = _runtime[idx];
    cfg.mode = newKind;
    return applyRuntimeConfig(idx, cfg);
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::setDuty(uint8_t idx, uint16_t duty_thousandths) {
    if (idx >= N || !_attached) return false;
    if (_runtime[idx].mode == ComponentKind::PwmLed) {
        // PwmLed channels are LED-runtime owned — direct duty writes
        // are rejected.  See refactor plan §"Runtime port reconfig".
        return false;
    }
    if (_stallGuards[idx].latched && duty_thousandths > 0) {
        // Stalled channel — reject motor / heater commands until
        // PWM_CLEAR_STALL.  Allow duty=0 so the master can defensively
        // zero the channel without first clearing the stall.
        return false;
    }
    if (duty_thousandths > _runtime[idx].maxDuty) {
        duty_thousandths = _runtime[idx].maxDuty;
    }
    _duties[idx] = duty_thousandths;
    if (!_specs[idx].useExpander) {
        // analogWrite uses 0..255 by default on Arduino-Pico / ESP32.
        // Map 0..1000 (thousandths) → 0..255.
        uint16_t pwm8 = (uint16_t)((uint32_t)duty_thousandths * 255u / 1000u);
        if (_runtime[idx].cfgFlags & PwmConfigFlags::INVERT_OUTPUT) pwm8 = 255 - pwm8;
        analogWrite(_specs[idx].pin, pwm8);
    }
    return true;
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::setMotor(uint8_t idx, int16_t speed_signed) {
    if (idx >= N) return false;
    if (_runtime[idx].mode != ComponentKind::PwmMotor) return false;
    if (_stallGuards[idx].latched && speed_signed != 0) return false;
    if (speed_signed < -1000) speed_signed = -1000;
    if (speed_signed >  1000) speed_signed =  1000;

    const int16_t prev = _motorSpeeds[idx];
    _motorSpeeds[idx] = speed_signed;

    // Apply max-duty clamp to the motor speed magnitude.
    int16_t clamped = speed_signed;
    {
        const int16_t lim = (int16_t)_runtime[idx].maxDuty;
        if (clamped >  lim) clamped =  lim;
        if (clamped < -lim) clamped = -lim;
    }

    // Stall-detector lifecycle.  Start the per-channel detector when
    // the motor transitions from stopped → moving (and the guard is
    // configured + enabled); stop it on transition back to 0.
    const bool guardArmed = (_stallGuards[idx].flags & ComponentPacket::StallFlags::ENABLED)
                            && _stallGuards[idx].threshold_mA > 0;
    if (clamped != 0 && prev == 0 && guardArmed) {
        _stallDetectors[idx].start();
    } else if (clamped == 0 && prev != 0) {
        _stallDetectors[idx].stop();
    }

    // Drive through the per-channel DcMotor instance — handles single-
    // pin / H-bridge-dual-GPIO / H-bridge-PWM+dir topologies via the
    // wiring captured in PwmSpec.  Brake-on-stop policy honoured at
    // speed=0 if the spec advertises BRAKE_ON_STOP.
    if (_motors[idx].isAttached()) {
        const bool brake = (clamped == 0)
                        && (_runtime[idx].cfgFlags & PwmConfigFlags::DC_BRAKE_ON_STOP);
        _motors[idx].setSpeed(clamped, brake);
        _duties[idx] = (uint16_t)((clamped < 0) ? -clamped : clamped);
    } else {
        // Channel without DcMotor (legacy / placeholder) — fall back to
        // direct duty write on the primary pin, magnitude only.
        const uint16_t mag = (uint16_t)((clamped < 0) ? -clamped : clamped);
        if (!setDuty(idx, mag)) return false;
    }

    // Emit motion events so the board can drive indicator LEDs.
    if (_onEvent) {
        if (prev == 0 && clamped != 0) {
            const uint16_t mag = (uint16_t)((clamped < 0) ? -clamped : clamped);
            _onEvent((uint8_t)idx, ComponentEvent::MotionStarted, mag);
        } else if (prev != 0 && clamped == 0) {
            _onEvent((uint8_t)idx, ComponentEvent::MotionEnded, 0);
        }
    }
    return true;
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::setHeater(uint8_t idx, uint16_t value_or_targetTemp) {
    if (idx >= N) return false;
    if (_runtime[idx].mode != ComponentKind::PwmHeater) return false;
    // Open-loop mode: value is duty (0..1000).
    // Closed-loop mode (HEATER_BANG_BANG): value is target temperature
    // in °C; update() ticks the bang-bang controller.  Placeholder.
    if (_runtime[idx].cfgFlags & PwmConfigFlags::HEATER_BANG_BANG) {
        // store target temp; controller in update() reads it.
        _duties[idx] = value_or_targetTemp;
        return true;
    }
    return setDuty(idx, value_or_targetTemp);
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::setFrequency(uint8_t idx, uint16_t freq_Hz) {
    if (idx >= N) return false;
    PwmRuntimeConfig cfg = _runtime[idx];
    cfg.freq_Hz = freq_Hz;
    return applyRuntimeConfig(idx, cfg);
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::reconfigure(uint8_t idx, const PwmRuntimeConfig& cfg) {
    return applyRuntimeConfig(idx, cfg);
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::setStallGuard(uint8_t idx, uint16_t threshold_mA,
                                              uint8_t debounce_ms, uint8_t flags) {
    if (idx >= N) return false;
    auto& g = _stallGuards[idx];
    g.threshold_mA = threshold_mA;
    g.debounce_ms  = debounce_ms;
    g.flags        = flags;
    g.peak_mA      = 0;
    g.latched      = false;

    // Push the threshold + debounce into the detector's config so the
    // next start() (triggered by setMotor with non-zero speed) picks
    // them up.  Other detector params (startup ignore, motor-presence
    // threshold, absolute timeout) keep their defaults — boards that
    // need to tune them can call into the detector directly via a
    // future per-channel accessor.
    StallDetector::Config cfg;
    cfg.stallThreshold_mA = threshold_mA;
    cfg.stallConfirm_ms   = debounce_ms;
    _stallDetectors[idx].configure(cfg);
    if (_stallDetectors[idx].isActive()) _stallDetectors[idx].stop();
    return true;
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::clearStall(uint8_t idx) {
    if (idx >= N) return false;
    auto& g = _stallGuards[idx];
    const bool wasLatched = g.latched;
    g.latched = false;
    g.peak_mA = 0;
    _stallDetectors[idx].stop();   // re-arm on next setMotor
    if (_onEvent && wasLatched) _onEvent(idx, ComponentEvent::StallCleared, 0);
    return true;
}

template <size_t N, SensePolicy TSense>
ComponentKind PwmCollection<N, TSense>::currentMode(uint8_t idx) const {
    if (idx >= N) return ComponentKind::None;
    return _runtime[idx].mode;
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::getRuntimeConfig(uint8_t idx, PwmRuntimeConfig& out) const {
    if (idx >= N) return false;
    out = _runtime[idx];
    return true;
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::query(uint8_t idx,
                             ComponentKind& out_mode,
                             uint16_t&      out_duty,
                             uint16_t&      out_freq_Hz,
                             int32_t&       out_voltage_mV,
                             int32_t&       out_current_mA) const {
    if (idx >= N) return false;
    out_mode    = _runtime[idx].mode;
    out_duty    = _duties[idx];
    out_freq_Hz = _runtime[idx].freq_Hz;
    out_voltage_mV = 0;
    out_current_mA = 0;
    // Sense policy is templated — concrete reads happen at compile time.
    // The policy returns false for channels that don't have sensing
    // wired; we leave the outputs at 0 in that case (callers can
    // distinguish "not sensed" from "0 V" via PWM_GET_CONFIG hwFlags).
    const_cast<TSense&>(_sense).readVoltage_mV(idx, out_voltage_mV);
    const_cast<TSense&>(_sense).readCurrent_mA(idx, out_current_mA);
    return true;
}

template <size_t N, SensePolicy TSense>
void PwmCollection<N, TSense>::writeDuty(uint8_t idx, uint16_t duty_thousandths) {
    // Called by PwmDutyAdapter on behalf of the LED runtime when a
    // channel is in PwmLed mode.  Bypasses mode gates — the wire
    // dispatcher already verified the channel is in PwmLed mode, and
    // the mode-change cleanup hook stops any active queue on mode flip.
    if (idx >= N || !_attached) return;
    _duties[idx] = duty_thousandths;
    if (!_specs[idx].useExpander) {
        uint16_t pwm8 = (uint16_t)((uint32_t)duty_thousandths * 255u / 1000u);
        if (_runtime[idx].cfgFlags & PwmConfigFlags::INVERT_OUTPUT) pwm8 = 255 - pwm8;
        analogWrite(_specs[idx].pin, pwm8);
    }
}

template <size_t N, SensePolicy TSense>
bool PwmCollection<N, TSense>::applyRuntimeConfig(uint8_t idx, const PwmRuntimeConfig& newCfg) {
    if (idx >= N) return false;
    const auto& spec = _specs[idx];

    // Gate: mode change requires MODE_MUTABLE; sensing-only flags
    // change is always allowed.
    if (newCfg.mode != _runtime[idx].mode) {
        if (!(spec.hwFlags & PwmFlags::MODE_MUTABLE)) return false;
        // Validate that the requested mode is a PWM-family kind.
        switch (newCfg.mode) {
            case ComponentKind::PwmGeneric:
            case ComponentKind::PwmLed:
            case ComponentKind::PwmMotor:
            case ComponentKind::PwmHeater:
                break;
            default: return false;
        }
    }

    // Side effects on transition.  Apply BEFORE swapping the runtime
    // config so the listeners see the right "leaving mode" value.
    bool leavingLed   = (_runtime[idx].mode == ComponentKind::PwmLed
                         && newCfg.mode != ComponentKind::PwmLed);
    bool enteringLed  = (_runtime[idx].mode != ComponentKind::PwmLed
                         && newCfg.mode == ComponentKind::PwmLed);

    if (leavingLed) {
        // Drop output and stop any program before flipping mode.
        if (_attached && !spec.useExpander) analogWrite(spec.pin, 0);
        _duties[idx] = 0;
        // (Notification to LedCollection happens via the CoreServer
        // wiring — the slave sets up onPwmLeftLedMode hooks at attach
        // time when both collections are bound.)
    }

    const ComponentKind prevMode = _runtime[idx].mode;
    const uint16_t      prevFreq = _runtime[idx].freq_Hz;

    // Atomic swap.
    _runtime[idx] = newCfg;

    if (enteringLed) {
        if (_attached && !spec.useExpander) analogWrite(spec.pin, 0);
        _duties[idx] = 0;
    }

    // Frequency reconfig — Arduino-Pico (RP2040) and ESP32 Arduino
    // core both expose analogWriteFreq() for changing the PWM
    // frequency of native-GPIO outputs.  Expander-attached pins
    // ignore this until the typed PwmOutput driver lands and routes
    // expander frequency configuration through the underlying chip's
    // own register set (e.g. AW9523B has a fixed-rate PWM but the
    // PCA9685 has a shared PRESCALE register).
    if (_attached && !spec.useExpander && newCfg.freq_Hz != prevFreq) {
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
        analogWriteFreq(newCfg.freq_Hz);
#endif
    }

    // Emit mode-change event for board indicator-LED hooks.
    if (_onEvent && newCfg.mode != prevMode) {
        _onEvent(idx, ComponentEvent::ModeChanged, (uint16_t)newCfg.mode);
    }
    return true;
}

}  // namespace sfx_peripherals

#endif  // SFX_PWM_COLLECTION_IPP
