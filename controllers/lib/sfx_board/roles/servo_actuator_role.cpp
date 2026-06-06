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
    _outUs     = _profile.centerUs;
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

void ServoActuatorRole::setNormalizedTarget(uint16_t pos) {
    // Saturate the fraction, then map it LINEARLY onto the CURRENT calibrated
    // [_minUs, _maxUs] (live — so a re-calibration is picked up on the next
    // command).  Hand the mapped µs to setTarget() so it gets the identical
    // clamp + inverted reflection a direct target does (no double-reflection:
    // the position is in SERVO space pre-reflection, exactly like a setTarget()
    // argument — kept linear, NOT piecewise-through-centre, so the reflection
    // stays correct under REV).
    if (pos > kPosNormFull) pos = kPosNormFull;
    const uint16_t us = static_cast<uint16_t>(
        _minUs + static_cast<uint32_t>(_maxUs - _minUs) * pos / kPosNormFull);
    setTarget(us);
}

void ServoActuatorRole::applyRecoil(int16_t offsetUs, uint16_t durationMs) {
    // Start (or restart) a recoil window.  tick() adds the offset to the output
    // on top of the aim and removes it when the deadline passes.
    _recoilOffsetUs = offsetUs;
    _recoilUntilMs  = EffectClock::instance().nowMs() + durationMs;
}

void ServoActuatorRole::tick() {
    if (!_port) return;
    const uint32_t dtMs = EffectClock::instance().dtMs();

    const uint16_t before = _outUs;
    _mp.tick(dtMs);                       // integrate the aim (slew/clamp/jerk)
    const uint16_t base = _mp.current();

    // De-jerk: drop the recoil offset once its window expires.
    if (_recoilOffsetUs != 0 &&
        (int32_t)(EffectClock::instance().nowMs() - _recoilUntilMs) >= 0) {
        _recoilOffsetUs = 0;
    }

    // Output = aim + active recoil offset, clamped to the servo window.  The
    // recoil rides ON TOP of the aim so it works moving or stationary.
    int32_t out = (int32_t)base + (int32_t)_recoilOffsetUs;
    if (out < (int32_t)_minUs) out = _minUs;
    if (out > (int32_t)_maxUs) out = _maxUs;
    const uint16_t finalUs = (uint16_t)out;

    // Velocity diagnostic from the OUTPUT delta (so a recoil reads as motion).
    if (dtMs > 0) {
        const int32_t deltaUs = (int32_t)finalUs - (int32_t)_lastPosUs;
        _velocity_us_per_s = (int16_t)((deltaUs * 1000) / (int32_t)dtMs);
        _lastPosUs = finalUs;
    }

    if (finalUs != before) {
        _port->writeMicroseconds(finalUs);
    }
    _outUs = finalUs;

    // onTargetReached reflects the AIM settling (recoil is transient).
    if (_mp.atTarget()) {
        if (!_wasAtTarget) {
            _wasAtTarget = true;
            if (_onTargetReached) _onTargetReached(finalUs);
        }
    }
}

}  // namespace sfx_core
