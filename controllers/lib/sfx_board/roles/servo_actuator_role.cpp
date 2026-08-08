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
    // Seed the integrator from the port's LAST WRITTEN pulse, not the
    // centre.  Roles re-emplace on every CONFIG_RELOAD (Studio Apply) while
    // the physical servo stays wherever it was — an unconditional centre
    // snap desynced the integrator (and the Studio live view) from reality,
    // and the next command made the servo jump unprofiled from its true
    // position.  A fresh boot reads the port's initial pulse (1500 centre),
    // so first-attach behaviour is unchanged.  Clamp into the hw window in
    // case the port was last driven by a wider prior calibration.
    uint16_t seed = _port->microseconds();
    if (seed < _minUs) seed = _minUs;
    if (seed > _maxUs) seed = _maxUs;
    _mp.snapTo(seed);
    _lastPosUs = seed;
    _outUs     = seed;
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
    // The role's calibration limits (_minUs/_maxUs — already hw-clamped by
    // setLimits) ARE the motion-profile's travel window: ASSIGN them, don't
    // one-directionally clamp.  The old code only pulled the profile INWARD
    // (min up / max down), so WIDENING the calibration — raising _maxUs —
    // left _profile.maxUs stuck at its prior narrower value and the
    // integrator kept clamping every target to the stale max.  A
    // re-calibration to a wider range was silently ignored: landing
    // deploy/retract (and any setTarget) never reached the new endpoint
    // (e.g. Studio max 1620 → role stuck at a prior 1587).  See the
    // 2026-06-06 diag trace.
    _profile.minUs = _minUs;
    _profile.maxUs = _maxUs;
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
    // Saturate the fraction, then map LINEARLY onto the CURRENT calibrated
    // [_minUs, _maxUs] (live — a re-calibration is picked up on the next
    // command).  Pure range map — direction semantics were REMOVED in 2.46.0
    // (positional effects command absolute µs via setTarget; see the class
    // comment).
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
        // A write-through (no-slew) jump can move the full travel in one tick,
        // overflowing int16 — compute in int32 and clamp before narrowing.
        int32_t v = (deltaUs * 1000) / (int32_t)dtMs;
        if      (v > INT16_MAX) v = INT16_MAX;
        else if (v < INT16_MIN) v = INT16_MIN;
        _velocity_us_per_s = (int16_t)v;
        _lastPosUs = finalUs;
    }

    if (finalUs != before) {
        _port->writeMicroseconds(finalUs);
    }
    _outUs = finalUs;

    // onTargetReached / onMotionDone reflect the AIM settling (recoil is
    // transient).  Both fire ONCE on the rising edge of atTarget() after a
    // commanded move; `_wasAtTarget` starts true so the initial idle state
    // never fires (a target command clears it in setTarget()).
    if (_mp.atTarget()) {
        if (!_wasAtTarget) {
            _wasAtTarget = true;
            if (_onTargetReached) _onTargetReached(finalUs);
            if (_onMotionDone)    _onMotionDone();
        }
    }
}

}  // namespace sfx_core
