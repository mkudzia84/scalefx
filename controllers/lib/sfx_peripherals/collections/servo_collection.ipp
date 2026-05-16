/*
 * ServoCollection<N, TServoCtrl>::*  — implementation.
 *
 * Thin templatised facade over N `ServoControl` instances:
 *   - configure() stores compile-time hardware specs (no I/O)
 *   - attach() calls ServoControl::begin(...) on each + registers the
 *     "target reached" callback that bridges into SlaveServer
 *   - detach() releases each ServoControl
 *   - update() advances the trapezoidal profile per channel + fires
 *     the registered TargetReachedCb exactly once per command
 *
 * Each public method does explicit bounds-checking so the BusServer
 * dispatch can return a clean SlaveError::INVALID_INDEX without UB.
 *
 * Indices are 0-based here (matching the wire format).
 */

#ifndef SFX_SERVO_COLLECTION_IPP
#define SFX_SERVO_COLLECTION_IPP

#include "servo_collection.h"

namespace sfx_peripherals {

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::attach() {
    if (_attached) return true;
    for (size_t i = 0; i < N; i++) {
        const auto& s = _specs[i];
        if (!_servos[i].begin(s.pin,
                              s.defaultMin_us,
                              s.defaultMax_us,
                              s.defaultCenter_us)) {
            // Roll back any partial attach so we don't half-engage.
            for (size_t j = 0; j < i; j++) _servos[j].end();
            return false;
        }
        _servos[i].setMotionProfile(s.defaultMaxSpeed,
                                    s.defaultAccel,
                                    s.defaultDecel);
        _armed[i] = false;
        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::Activated, 0);
    }
    _attached = true;
    return true;
}

template <size_t N, typename TServoCtrl>
void ServoCollection<N, TServoCtrl>::detach() {
    if (!_attached) return;
    for (size_t i = 0; i < N; i++) {
        _servos[i].end();
        _armed[i] = false;
        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::Deactivated, 0);
    }
    _attached = false;
}

template <size_t N, typename TServoCtrl>
void ServoCollection<N, TServoCtrl>::parkAtNeutral() {
    for (size_t i = 0; i < N; i++) {
        // Use safeStatePos_us if set, else defaultCenter_us.  The spec
        // initialiser leaves safeStatePos_us at 0 if the board
        // firmware didn't set it explicitly — fall back to centre.
        uint16_t safe = _specs[i].safeStatePos_us
                          ? _specs[i].safeStatePos_us
                          : _specs[i].defaultCenter_us;
        if (_attached) _servos[i].setTarget((int)safe);
        _armed[i] = false;     // safe-state move is silent (no target-reached emission)
        if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::SafeStateEntered, safe);
    }
}

template <size_t N, typename TServoCtrl>
void ServoCollection<N, TServoCtrl>::update() {
    if (!_attached) return;

    for (size_t i = 0; i < N; i++) {
        _servos[i].update();
        // Fire target-reached callback exactly once per command.
        if (_armed[i] && _servos[i].isAtTarget()) {
            _armed[i] = false;
            if (_onTargetReached) {
                _onTargetReached((uint8_t)i,
                                 (uint16_t)_servos[i].currentPositionUs());
            }
            if (_onEvent) _onEvent((uint8_t)i, ComponentEvent::MotionEnded,
                                   (uint16_t)_servos[i].currentPositionUs());
        }
    }

    // Periodic mid-motion update — for each servo currently armed
    // (profiling toward a target), emit a (pos, target, vel) snapshot
    // at the configured rate.  Skipped per channel as the profile
    // converges; full set restarts on the next setPosition_us call.
    if (_motionUpdatesEnabled && _onMotionUpdate) {
        const uint32_t now = millis();
        if (now - _lastMotionUpdate_ms >= _motionUpdate_period_ms) {
            _lastMotionUpdate_ms = now;
            for (size_t i = 0; i < N; i++) {
                if (!_armed[i]) continue;
                _onMotionUpdate(
                    (uint8_t)i,
                    (uint16_t)_servos[i].currentPositionUs(),
                    (uint16_t)_servos[i].targetPositionUs(),
                    (int16_t) _servos[i].velocityUsPerSec());
            }
        }
    }
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::setPosition_us(uint8_t idx, uint16_t pulse_us) {
    if (idx >= N || !_attached) return false;
    const bool wasAtTarget = _servos[idx].isAtTarget();
    _servos[idx].setTarget((int)pulse_us);
    _armed[idx] = true;
    if (_onEvent && wasAtTarget) {
        _onEvent(idx, ComponentEvent::MotionStarted, pulse_us);
    }
    return true;
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::setRange(uint8_t idx,
                                  uint16_t min_us,
                                  uint16_t max_us,
                                  uint16_t center_us) {
    if (idx >= N) return false;
    _specs[idx].defaultMin_us    = min_us;
    _specs[idx].defaultMax_us    = max_us;
    _specs[idx].defaultCenter_us = center_us;
    if (_attached) _servos[idx].setLimits((int)min_us, (int)max_us);
    return true;
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::setMotion(uint8_t idx,
                                   uint16_t maxSpeed,
                                   uint16_t accel,
                                   uint16_t decel) {
    if (idx >= N) return false;
    _specs[idx].defaultMaxSpeed = maxSpeed;
    _specs[idx].defaultAccel    = accel;
    _specs[idx].defaultDecel    = decel;
    if (_attached) _servos[idx].setMotionProfile((int)maxSpeed, (int)accel, (int)decel);
    return true;
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::applyJerk(uint8_t idx, int16_t offset_us, uint16_t duration_ms) {
    if (idx >= N || !_attached) return false;
    // ServoControl exposes a jerk-offset feature documented in
    // sfx_peripherals/servo/srv_control.h §"Jerk offset".  Forward
    // verbatim — the underlying driver handles the per-tick decay
    // back to the baseline target.
    _servos[idx].applyJerk(offset_us, duration_ms);
    return true;
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::setHold(uint8_t idx, bool held) {
    if (idx >= N || !_attached) return false;
    if (held) {
        // ServoControl::end() stops PWM emission while leaving the
        // ServoControl object alive (so target / profile state
        // survives).  A subsequent setHold(false) re-attaches.
        _servos[idx].end();
    } else {
        const auto& s = _specs[idx];
        _servos[idx].begin(s.pin, s.defaultMin_us, s.defaultMax_us, s.defaultCenter_us);
        _servos[idx].setMotionProfile(s.defaultMaxSpeed, s.defaultAccel, s.defaultDecel);
    }
    return true;
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::setPositionNorm(uint8_t idx, int16_t normSigned) {
    if (idx >= N) return false;
    if (normSigned < -1000) normSigned = -1000;
    if (normSigned >  1000) normSigned =  1000;
    const auto& s = _specs[idx];
    int32_t span = (normSigned >= 0)
        ? (int32_t)s.defaultMax_us - (int32_t)s.defaultCenter_us
        : (int32_t)s.defaultCenter_us - (int32_t)s.defaultMin_us;
    int32_t pulse = (int32_t)s.defaultCenter_us + (span * normSigned) / 1000;
    return setPosition_us(idx, (uint16_t)pulse);
}

template <size_t N, typename TServoCtrl>
bool ServoCollection<N, TServoCtrl>::query(uint8_t idx,
                               uint16_t& out_pulse_us,
                               uint16_t& out_target_us,
                               int16_t&  out_velocity) const {
    if (idx >= N) return false;
    if (!_attached) {
        out_pulse_us  = _specs[idx].defaultCenter_us;
        out_target_us = _specs[idx].defaultCenter_us;
        out_velocity  = 0;
        return true;
    }
    out_pulse_us  = (uint16_t)_servos[idx].currentPositionUs();
    out_target_us = (uint16_t)_servos[idx].targetPositionUs();
    out_velocity  = (int16_t)_servos[idx].velocityUsPerSec();
    return true;
}

}  // namespace sfx_peripherals

#endif  // SFX_SERVO_COLLECTION_IPP
