/*
 * motion_profile.h — generic 1-DoF servo motion: config + runtime.
 *
 *   `ServoMotionProfile` — declarative shape (clamp + speed + accel + jerk).
 *   `MotionProfile1D`    — runtime integrator that consumes the shape and
 *                          produces a per-tick `current()` µs converging
 *                          on `target()`.
 *
 * Shared across every effect that maps an input value (RC µs, fader,
 * slot selector, …) onto a position-commanded servo through a
 * trapezoidal / S-curve profile.  Lives on `ServoActuatorRole` per
 * Rule 42 ("Actuator mechanism on the role layer") — effects command
 * the role at the intent layer (`setTarget(us)`) and never integrate
 * a profile themselves.
 *
 * Owners:
 *   - `ServoActuatorRole` (Phase 2.9 of GunFX rollout, instructions/22)
 *   - Future: gear-control retract / extend servo, landing-light
 *     bay-door servo, any other position-commanded servo.
 *
 * Conventions (Rule 5 — physical units in names):
 *   *_Us, *_UsPerSec, *_UsPerSec2, *_UsPerSec3.
 *
 * YAML shape (consumed by the role-attach config in /hubfx.yaml):
 *
 *   profile:
 *     min_us:    1100              # clamp lower bound (motion CAP)
 *     max_us:    1900              # clamp upper bound (motion CAP)
 *     center_us: 1500              # neutral / failsafe
 *     max_speed_us_per_sec:  800   # slew limit (0 = unlimited)
 *     max_accel_us_per_sec2: 1600  # 0 = pure speed clamp, no trapezoid
 *     max_jerk_us_per_sec3:  0     # 0 = trapezoidal (no S-curve)
 *
 * There is NO direction/inversion concept here (removed 2.46.0): the
 * profile is a pure motion envelope.  Effects that need named positions
 * store ABSOLUTE µs targets in their own config.  (The old `inverted`
 * mirror inside clamp() was one of THREE direction layers that stacked
 * into the 2026-08-08 "double reverse" bench saga.)
 *
 * Algorithm (MotionProfile1D::tick):
 *   1. Target is clamped to [minUs, maxUs].
 *   2. If `hasSlew()` is false → write through, set current = target.
 *   3. Otherwise: integrate `dt` per tick.
 *      - Accel cap (`maxAccelUsPerSec2`) clamps velocity change.
 *      - Speed cap (`maxSpeedUsPerSec`) clamps velocity magnitude.
 *      - Jerk cap (`maxJerkUsPerSec3`, optional — 0 = trapezoidal)
 *        clamps the rate of acceleration change for an S-curve.
 *      - Lookahead: when remaining distance is too small to decelerate
 *        within `maxAccelUsPerSec2`, ramp velocity down so we land on
 *        target without overshoot.
 *
 * Note: `dtMs == 0` is a no-op (the EffectClock idempotent-latch
 * pattern from Rule 40 makes this common).  Sub-millisecond drift
 * accumulates in `_currentF`; only `current()` rounds to µs.
 */

#ifndef SFX_MOTION_PROFILE_H
#define SFX_MOTION_PROFILE_H

#include <cstdint>
#include <cmath>

namespace sfx_core {

// ─── Config shape ──────────────────────────────────────────────────

struct ServoMotionProfile {
    uint16_t minUs                = 1000;    ///< clamp lower bound, µs
    uint16_t maxUs                = 2000;    ///< clamp upper bound, µs
    uint16_t centerUs             = 1500;    ///< neutral / failsafe, µs
    uint16_t maxSpeedUsPerSec     = 800;     ///< slew limit (0 = unlimited)
    uint16_t maxAccelUsPerSec2    = 1600;    ///< 0 = pure speed clamp, no trapezoid
    uint16_t maxJerkUsPerSec3     = 0;       ///< 0 = trapezoidal (no S-curve)

    /// Clamp a target µs into [minUs, maxUs] — a pure cap, no mapping.
    uint16_t clamp(uint16_t target) const {
        if (target < minUs) target = minUs;
        if (target > maxUs) target = maxUs;
        return target;
    }

    /// True when at least one slew constraint is set; false means the
    /// integrator should just clamp + write through directly (no
    /// per-tick smoothing needed).  Includes jerk: a jerk-only profile
    /// (speed/accel left at 0 = uncapped) still wants the integrator to
    /// run so the jerk-limited accel ramp shapes the motion — otherwise
    /// setting only a jerk value is silently ignored (servo snaps).
    bool hasSlew() const {
        return maxSpeedUsPerSec > 0 || maxAccelUsPerSec2 > 0 || maxJerkUsPerSec3 > 0;
    }
};

// ─── Runtime integrator ────────────────────────────────────────────

class MotionProfile1D {
public:
    MotionProfile1D() = default;

    /// (Re-)set the profile.  Clamps `_currentF` into the new range.
    void setProfile(const ServoMotionProfile& p) {
        _profile = p;
        // If the live position was pulled to a NEW boundary, the integrator's
        // velocity/accel were aiming past that edge — zero them so the next
        // tick doesn't carry stale momentum out of (or along) the clamp.
        if (_currentF < p.minUs) { _currentF = p.minUs; _velocity = 0.0f; _accelLast = 0.0f; }
        if (_currentF > p.maxUs) { _currentF = p.maxUs; _velocity = 0.0f; _accelLast = 0.0f; }
        // Re-clamp the in-flight target into the new range (idempotent —
        // clamp() is a pure cap since 2.46.0).
        if (_targetUs < p.minUs) _targetUs = p.minUs;
        if (_targetUs > p.maxUs) _targetUs = p.maxUs;
    }

    /// Set the desired endpoint (clamped to the profile window).
    void setTarget(uint16_t targetUs) {
        _targetUs = _profile.clamp(targetUs);
    }

    /// Snap current → target instantly (no slew).  Use on first config
    /// to avoid a sweep from minUs to centerUs on startup.
    void snapTo(uint16_t targetUs) {
        _targetUs  = _profile.clamp(targetUs);
        _currentF  = static_cast<float>(_targetUs);
        _velocity  = 0.0f;
    }

    /// Integrate one tick.  `dtMs` is the EffectClock delta.
    void tick(uint32_t dtMs) {
        if (dtMs == 0) return;
        if (!_profile.hasSlew()) {
            _currentF = static_cast<float>(_targetUs);
            _velocity = 0.0f;
            return;
        }
        const float dt   = float(dtMs) * 0.001f;
        const float tgt  = static_cast<float>(_targetUs);
        const float maxV = (_profile.maxSpeedUsPerSec > 0)
                            ? float(_profile.maxSpeedUsPerSec) : 1e9f;
        const float maxA = (_profile.maxAccelUsPerSec2 > 0)
                            ? float(_profile.maxAccelUsPerSec2) : 1e9f;

        const float dist     = tgt - _currentF;
        const float distSign = dist >= 0 ? 1.0f : -1.0f;
        const float absDist  = std::fabs(dist);

        // Decel lookahead: brake distance = v² / (2·a).  When we're
        // closer than the brake distance, ramp velocity down so we land
        // on target without overshoot.
        float brakeDist = (_velocity * _velocity) / (2.0f * maxA);
        if (_profile.maxJerkUsPerSec3 > 0) {
            // Jerk-limited stop needs MORE distance than the trapezoidal
            // v²/2a: the deceleration itself has to ramp down (over ≈ a/j
            // seconds), adding ≈ |v|·a/(2·j).  Without widening the lookahead
            // the S-curve brakes too late and overshoots the target (then the
            // endpoint clamp snaps it back — a visible twitch). This makes it
            // start braking earlier so it arrives cleanly.
            brakeDist += std::fabs(_velocity) * maxA
                       / (2.0f * float(_profile.maxJerkUsPerSec3));
        }
        float desiredV;
        if (absDist <= brakeDist) {
            // Brake — decelerate toward 0 to land on target.
            desiredV = std::copysign(std::sqrt(2.0f * maxA * absDist), dist);
        } else {
            // Cruise — accelerate up to max speed in the target direction.
            desiredV = distSign * maxV;
        }

        // Clamp velocity change by accel (and optionally jerk).
        float dv = desiredV - _velocity;
        const float maxDv = maxA * dt;
        if      (dv >  maxDv) dv =  maxDv;
        else if (dv < -maxDv) dv = -maxDv;

        if (_profile.maxJerkUsPerSec3 > 0) {
            // S-curve: rate-of-acceleration limit.  Smooths the
            // velocity ramp so the operator doesn't see the bang-bang
            // accel switch in the corners.
            const float maxJ      = float(_profile.maxJerkUsPerSec3);
            const float currA     = _accelLast;
            const float desiredA  = dv / dt;
            float da = desiredA - currA;
            const float maxDa = maxJ * dt;
            if      (da >  maxDa) da =  maxDa;
            else if (da < -maxDa) da = -maxDa;
            const float newA = currA + da;
            dv         = newA * dt;
            _accelLast = newA;
        } else {
            _accelLast = dv / dt;   // for diagnostics; not used in trap
        }

        _velocity += dv;
        // Clamp speed
        if      (_velocity >  maxV) _velocity =  maxV;
        else if (_velocity < -maxV) _velocity = -maxV;

        _currentF += _velocity * dt;

        // Final clamp + snap-to-target when we're within ½µs (avoids
        // float-precision dither at the endpoint).
        if (_currentF < _profile.minUs) { _currentF = _profile.minUs; _velocity = 0; _accelLast = 0; }
        if (_currentF > _profile.maxUs) { _currentF = _profile.maxUs; _velocity = 0; _accelLast = 0; }
        if (std::fabs(_currentF - tgt) < 0.5f) {
            _currentF  = tgt;
            _velocity  = 0;
            _accelLast = 0;
        }
    }

    /// Current commanded position, rounded to µs.  This is what you
    /// write to the servo each tick.
    uint16_t current() const {
        return static_cast<uint16_t>(_currentF + 0.5f);
    }
    uint16_t target() const { return _targetUs; }

    /// True when current() == target() AND the integrator is idle
    /// (velocity ≈ 0).  Useful for "wait until move complete" gates.
    bool atTarget() const {
        return std::fabs(_currentF - _targetUs) < 0.5f && std::fabs(_velocity) < 0.1f;
    }

private:
    ServoMotionProfile _profile{};
    uint16_t           _targetUs  = 1500;
    float              _currentF  = 1500.0f;   ///< sub-µs precision
    float              _velocity  = 0.0f;      ///< µs/sec
    float              _accelLast = 0.0f;      ///< µs/sec², used by jerk-cap path
};

}  // namespace sfx_core

#endif  // SFX_MOTION_PROFILE_H
