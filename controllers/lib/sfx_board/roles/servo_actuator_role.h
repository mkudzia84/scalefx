/*
 * ServoActuatorRole — trapezoidal motion-profile actuator on a
 * `ServoPort` (output mode).
 *
 * Holds the role's runtime state (target, current commanded position,
 * velocity) and writes pulse widths to the port at each tick.  The
 * profile is intentionally minimal: linear ramp toward the target at
 * `_maxVelocity_us_per_s` (no separate accel/decel).  The full
 * accel/decel trapezoid lives in the legacy `ServoControl` and can be
 * grafted in later if a board needs it.
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

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <ports/servo_port.h>

namespace sfx_core {

class ServoActuatorRole {
public:
    using TargetReachedCallback = std::function<void(uint16_t pos_us)>;

    ServoActuatorRole() = default;
    explicit ServoActuatorRole(sfx_peripherals::ServoPort* port) : _port(port) {
        if (port) {
            _minUs       = port->minMicroseconds();
            _maxUs       = port->maxMicroseconds();
            _commanded   = (_minUs + _maxUs) / 2;
            _target      = _commanded;
        }
    }

    void bind(sfx_peripherals::ServoPort* port) {
        _port = port;
        if (port) {
            _minUs     = port->minMicroseconds();
            _maxUs     = port->maxMicroseconds();
            _commanded = (_minUs + _maxUs) / 2;
            _target    = _commanded;
        }
    }

    /// Calibration limits (must stay within the port's hardware bounds —
    /// values outside are clamped silently).
    void setLimits(uint16_t minUs, uint16_t maxUs);
    /// Reversed flag — swaps which calibration endpoint is "open".
    void setReversed(bool reversed) { _reversed = reversed; }
    /// Max velocity in µs/s (default 4000).
    void setMaxVelocity_us_per_s(uint16_t v) { _maxVelocity_us_per_s = v; }

    /// Target setters
    void setTarget(uint16_t target_us);
    void setPositionImmediate(uint16_t pos_us);

    /// Endpoints honour the REV flag.
    uint16_t openEndpoint()  const { return _reversed ? _minUs : _maxUs; }
    uint16_t closeEndpoint() const { return _reversed ? _maxUs : _minUs; }

    uint16_t position() const { return _commanded; }
    uint16_t target()   const { return _target; }
    int16_t  velocity_us_per_s() const { return _velocity_us_per_s; }
    bool     atTarget() const { return _commanded == _target; }

    void onTargetReached(TargetReachedCallback cb) { _onTargetReached = std::move(cb); }

    /// Tick — call from `update()`.  Steps `_commanded` toward `_target`
    /// at `_maxVelocity_us_per_s` and writes to the port.
    void tick();

private:
    sfx_peripherals::ServoPort* _port = nullptr;

    uint16_t _minUs                = 500;
    uint16_t _maxUs                = 2500;
    bool     _reversed             = false;
    uint16_t _maxVelocity_us_per_s = 4000;

    uint16_t _commanded            = 1500;
    uint16_t _target               = 1500;
    int16_t  _velocity_us_per_s    = 0;
    uint32_t _lastTickMs           = 0;
    bool     _wasAtTarget          = true;

    TargetReachedCallback _onTargetReached;
};

}  // namespace sfx_core

#endif  // SFX_SERVO_ACTUATOR_ROLE_H
