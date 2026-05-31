/*
 * ServoActuatorRole — trapezoidal motion-profile actuator on a
 * `ServoPort` (output mode).
 *
 * Phase 2.9 of GunFX rollout (instructions/22): the role now owns a
 * shared `sfx_core::MotionProfile1D` integrator and consumes a
 * `ServoMotionProfile` config — same motion shape used by every
 * servo consumer in the system (gun yaw / pitch, gear extend / retract,
 * landing bay door, …).  The legacy velocity-only slew is retired.
 *
 * Architectural pattern (mirrors Rule 42 — element scaling lives on
 * the role layer): the EFFECT commands the role at the INTENT layer
 * (`setTarget(us)`), the ROLE owns the motion shape (clamp + slew +
 * accel + jerk) via its `MotionProfile1D`, and the PORT writes the
 * resulting µs to the hardware.  Effects don't integrate motion
 * profiles themselves — they push a target and let the role shape it.
 *
 * Limits (calibration) and reversed flag live in the role — the port
 * only knows hardware-safe absolute bounds.  Role-level limits are
 * narrower than port-level limits.
 *
 * `TARGET_REACHED` fires once via the registered callback when the
 * commanded position first equals the target after motion.
 */

#ifndef SFX_SERVO_ACTUATOR_ROLE_H
#define SFX_SERVO_ACTUATOR_ROLE_H

#include <cstdint>
#include <functional>

#include <ports/servo_port.h>

#include "../motion/motion_profile.h"

namespace sfx_core {

class ServoActuatorRole {
public:
    using TargetReachedCallback = std::function<void(uint16_t pos_us)>;

    ServoActuatorRole() = default;
    explicit ServoActuatorRole(sfx_peripherals::ServoPort* port) : _port(port) {
        if (port) initFromPort();
    }

    void bind(sfx_peripherals::ServoPort* port) {
        _port = port;
        if (port) initFromPort();
    }

    /// Calibration limits (must stay within the port's hardware bounds —
    /// values outside are clamped silently).
    void setLimits(uint16_t minUs, uint16_t maxUs);

    /// Reversed flag — swaps which calibration endpoint is "open".
    void setReversed(bool reversed) { _reversed = reversed; }

    /// Replace the full motion profile.  Speed / accel / jerk all in
    /// one shot — this is the canonical configuration entry-point;
    /// the legacy velocity-only setter below stays for back-compat
    /// callers that only care about top speed.
    void setProfile(const ServoMotionProfile& p);

    /// Back-compat shim — sets only the max-speed field of the profile,
    /// leaving accel/jerk at their existing values.  Lets old call
    /// sites (`role.setMaxVelocity_us_per_s(...)`) keep working without
    /// re-plumbing.
    void setMaxVelocity_us_per_s(uint16_t v);

    /// Read the active profile (e.g. for verbose status / Studio mirror).
    const ServoMotionProfile& profile() const { return _profile; }

    /// Target setters — INTENT layer.  The role's `tick()` integrates
    /// via `MotionProfile1D` and writes the resulting µs to the port.
    void setTarget(uint16_t target_us);

    /// Recoil impulse — add `offsetUs` to the output for `durationMs`, then
    /// auto-remove ("de-jerk").  Rides ON TOP of the aim (the motion profile
    /// keeps tracking its target underneath), so it works whether the servo is
    /// moving or stationary and never fights the aim.  GunFx calls this once
    /// per shot with a random ± offset.  Calling again restarts the window.
    void applyRecoil(int16_t offsetUs, uint16_t durationMs);

    /// Endpoints honour the REV flag.
    uint16_t openEndpoint()  const { return _reversed ? _minUs : _maxUs; }
    uint16_t closeEndpoint() const { return _reversed ? _maxUs : _minUs; }

    // `position()` reports the ACTUAL output µs (aim + active recoil offset), so
    // telemetry/UI show the kick; `target()` stays the aim the profile slews to.
    uint16_t position() const { return _outUs; }
    uint16_t target()   const { return _mp.target(); }
    int16_t  velocity_us_per_s() const { return _velocity_us_per_s; }
    bool     atTarget() const { return _mp.atTarget(); }

    void onTargetReached(TargetReachedCallback cb) { _onTargetReached = std::move(cb); }

    /// Tick — call from `update()`.  Integrates the motion profile and
    /// writes the resulting µs to the port.
    void tick();

private:
    void initFromPort();
    void rebuildProfileLimits();

    sfx_peripherals::ServoPort* _port = nullptr;

    uint16_t _minUs                = 500;
    uint16_t _maxUs                = 2500;
    bool     _reversed             = false;

    ServoMotionProfile _profile;          ///< speed/accel/jerk shape
    MotionProfile1D    _mp;               ///< runtime integrator
    int16_t            _velocity_us_per_s = 0;
    uint16_t           _lastPosUs         = 1500;  ///< for velocity diag
    bool               _wasAtTarget       = true;
    uint16_t           _outUs             = 1500;  ///< last µs written (aim + recoil)

    // Recoil impulse — additive offset applied to the output for a window.
    int16_t            _recoilOffsetUs    = 0;     ///< 0 = inactive
    uint32_t           _recoilUntilMs     = 0;     ///< EffectClock deadline

    TargetReachedCallback _onTargetReached;
};

}  // namespace sfx_core

#endif  // SFX_SERVO_ACTUATOR_ROLE_H
