/*
 * ServoActuatorRole implementation.
 *
 * Phase 2.9 of GunFX rollout (instructions/22): the role's motion
 * integrator is the shared `MotionProfile1D` from
 * `controllers/lib/sfx_board/motion/`.  All servo-driven effects
 * (gun yaw/pitch, gear servo, landing bay door) now feed targets at
 * the intent layer and let the role do the shaping — no duplicated
 * integrators in each effect.
 */

#include "servo_actuator_role.h"

#include "../server/effect_clock.h"

namespace sfx_core {

void ServoActuatorRole::initFromPort() {
    _minUs              = _port->minMicroseconds();
    _maxUs              = _port->maxMicroseconds();
    _profile.minUs      = _minUs;
    _profile.maxUs      = _maxUs;
    _profile.centerUs   = (_minUs + _maxUs) / 2;
    _mp.setProfile(_profile);
    _mp.snapTo(_profile.centerUs);
    _lastPosUs = _profile.centerUs;
}

void ServoActuatorRole::setLimits(uint16_t minUs, uint16_t maxUs) {
    if (minUs >= maxUs) return;
    if (_port) {
        const uint16_t hwMin = _port->minMicroseconds();
        const uint16_t hwMax = _port->maxMicroseconds();
        if (minUs < hwMin) minUs = hwMin;
        if (maxUs > hwMax) maxUs = hwMax;
    }
    _minUs = minUs;
    _maxUs = maxUs;
    rebuildProfileLimits();
}

void ServoActuatorRole::setProfile(const ServoMotionProfile& p) {
    _profile = p;
    rebuildProfileLimits();
}

void ServoActuatorRole::setMaxVelocity_us_per_s(uint16_t v) {
    _profile.maxSpeedUsPerSec = v;
    _mp.setProfile(_profile);
}

void ServoActuatorRole::rebuildProfileLimits() {
    // Calibration limits overlay the profile's min/max — role-level
    // limits are narrower than port-level limits.
    if (_profile.minUs < _minUs) _profile.minUs = _minUs;
    if (_profile.maxUs > _maxUs) _profile.maxUs = _maxUs;
    if (_profile.centerUs < _profile.minUs) _profile.centerUs = _profile.minUs;
    if (_profile.centerUs > _profile.maxUs) _profile.centerUs = _profile.maxUs;
    _mp.setProfile(_profile);
}

void ServoActuatorRole::setTarget(uint16_t target_us) {
    if (target_us < _minUs) target_us = _minUs;
    if (target_us > _maxUs) target_us = _maxUs;
    const uint16_t prev = _mp.target();
    _mp.setTarget(target_us);
    if (_mp.target() != _mp.current()) _wasAtTarget = false;
    (void)prev;   // no per-edge bookkeeping needed
}

void ServoActuatorRole::setPositionImmediate(uint16_t pos_us) {
    if (pos_us < _minUs) pos_us = _minUs;
    if (pos_us > _maxUs) pos_us = _maxUs;
    _mp.snapTo(pos_us);
    _velocity_us_per_s = 0;
    _lastPosUs   = pos_us;
    _wasAtTarget = true;
    if (_port) _port->writeMicroseconds(pos_us);
}

void ServoActuatorRole::tick() {
    if (!_port) return;
    const uint32_t dtMs = EffectClock::instance().dtMs();

    const uint16_t before = _mp.current();
    _mp.tick(dtMs);
    const uint16_t after  = _mp.current();

    // Velocity diagnostic for status reporting — derived from position
    // delta this tick, not from the integrator's internal float (which
    // is fine but less robust against profile reshapes).
    if (dtMs > 0) {
        const int32_t deltaUs = (int32_t)after - (int32_t)_lastPosUs;
        _velocity_us_per_s = (int16_t)((deltaUs * 1000) / (int32_t)dtMs);
        _lastPosUs = after;
    }

    if (after != before) {
        _port->writeMicroseconds(after);
    }

    if (_mp.atTarget()) {
        _velocity_us_per_s = 0;
        if (!_wasAtTarget) {
            _wasAtTarget = true;
            if (_onTargetReached) _onTargetReached(after);
        }
    }
}

}  // namespace sfx_core
