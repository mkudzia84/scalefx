/*
 * DcMotorRole — uni-directional DC motor on a `PwmPort` with optional
 * current-sense stall detection.
 *
 * No bidirectional support — that's `BiDcMotorRole` on an `HBridgePort`.
 * This role just open-loop drives the duty cycle, then watches the
 * current-sense channel (if bound) for a sustained over-current event
 * that suggests the motor has stalled.
 *
 * Stall detection (when `_iSense` is non-null):
 *   - Trip threshold:    `_stallThreshold_mA` (default 2000 mA)
 *   - Sustained window:  `_stallWindow_ms`    (default 250 ms)
 *   - Latched flag (`stalled()`) — cleared by `clearStall()` or `setDuty(0)`.
 *   - Async `STALL_EVENT` fires once per latch via the registered callback.
 */

#ifndef SFX_DC_MOTOR_ROLE_H
#define SFX_DC_MOTOR_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <ports/pwm_port.h>
#include <ports/sensors.h>

namespace sfx_core {

class DcMotorRole {
public:
    /// Async stall callback: `(peak_mA, duration_ms)`.
    using StallCallback = std::function<void(uint16_t peak_mA, uint16_t duration_ms)>;

    DcMotorRole() = default;
    DcMotorRole(sfx_peripherals::PwmPort*       port,
                sfx_peripherals::CurrentSensor* iSense = nullptr,
                sfx_peripherals::VoltageSensor* vSense = nullptr)
        : _port(port), _iSense(iSense), _vSense(vSense) {}

    void bind(sfx_peripherals::PwmPort*       port,
              sfx_peripherals::CurrentSensor* iSense = nullptr,
              sfx_peripherals::VoltageSensor* vSense = nullptr) {
        _port = port; _iSense = iSense; _vSense = vSense;
    }

    /// Set duty in port-native units (0..port.maxDuty()).
    void setDuty(uint16_t duty);
    /// Brake — write duty 0.  (True brake would need an H-bridge.)
    void brake() { setDuty(0); clearStall(); }

    /// Stall-detection tuning.  Threshold = 0 disables.
    void setStallGuard(uint16_t threshold_mA, uint16_t window_ms);
    void clearStall();

    // Status accessors
    uint16_t duty()         const { return _commandedDuty; }
    bool     stalled()      const { return _stalled; }
    int16_t  voltage_mV()   const { return _vSense ? _vSense->voltage_mV() : 0; }
    int16_t  current_mA()   const { return _iSense ? _iSense->current_mA() : 0; }

    void onStall(StallCallback cb) { _onStall = std::move(cb); }

    /// Tick — call from `update()`.
    void tick();

private:
    sfx_peripherals::PwmPort*       _port           = nullptr;
    sfx_peripherals::CurrentSensor* _iSense         = nullptr;
    sfx_peripherals::VoltageSensor* _vSense         = nullptr;

    uint16_t _commandedDuty       = 0;
    bool     _stalled             = false;
    uint16_t _stallThreshold_mA   = 2000;
    uint16_t _stallWindow_ms      = 250;
    uint32_t _overcurrentStartMs  = 0;   ///< 0 = not currently over threshold
    uint16_t _peakDuringWindow_mA = 0;

    StallCallback _onStall;
};

}  // namespace sfx_core

#endif  // SFX_DC_MOTOR_ROLE_H
