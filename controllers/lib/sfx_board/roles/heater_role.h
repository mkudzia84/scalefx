/*
 * HeaterRole — bang-bang heater on a `PwmPort` driven by a
 * `TemperatureSensor`.
 *
 * Closed-loop: holds the temperature within `±_hysteresis_cx10` of
 * `_target_cx10`.  Below the lower band → PWM at `_driveDuty`.  Above
 * the upper band → PWM 0.  Inside the band → keep current state
 * (hysteresis).
 *
 * Both target and hysteresis are in tenths of degrees Celsius (235 →
 * 23.5 °C).  The role refuses to attach if no temperature sensor is
 * bound — heater without sense would be a fire hazard.
 */

#ifndef SFX_HEATER_ROLE_H
#define SFX_HEATER_ROLE_H

#include <Arduino.h>
#include <cstdint>

#include <ports/pwm_port.h>
#include <ports/sensors.h>

namespace sfx_core {

class HeaterRole {
public:
    HeaterRole() = default;
    HeaterRole(sfx_peripherals::PwmPort*           port,
               sfx_peripherals::TemperatureSensor* tSense)
        : _port(port), _tSense(tSense) {}

    /// Returns false if `tSense` is null — heater requires temp sense.
    bool bind(sfx_peripherals::PwmPort*           port,
              sfx_peripherals::TemperatureSensor* tSense) {
        if (!tSense) return false;
        _port = port; _tSense = tSense;
        return true;
    }

    /// Set target temperature in tenths of °C.  `target_cx10 == INT16_MIN`
    /// = off.  Default = INT16_MIN (off until configured).
    void setTarget(int16_t target_cx10);
    int16_t target() const { return _target_cx10; }

    /// Hysteresis half-width in tenths of °C (default 10 = ±1.0 °C).
    void setHysteresis(int16_t hysteresis_cx10) { _hysteresis_cx10 = hysteresis_cx10; }

    /// Drive duty applied while heating (0..port.maxDuty()).  Defaults
    /// to port.maxDuty() (full-on, full-off bang-bang).
    void setDriveDuty(uint16_t duty) { _driveDuty = duty; _driveDutyExplicit = true; }

    int16_t  actual_cx10() const { return _tSense ? _tSense->temperature_cx10() : 0; }
    uint16_t commandedDuty() const { return _commandedDuty; }
    bool     heating() const { return _commandedDuty > 0; }

    /// Tick — runs the bang-bang loop.
    void tick();

private:
    sfx_peripherals::PwmPort*           _port               = nullptr;
    sfx_peripherals::TemperatureSensor* _tSense             = nullptr;

    int16_t  _target_cx10        = INT16_MIN;   ///< off
    int16_t  _hysteresis_cx10    = 10;          ///< ±1.0 °C
    uint16_t _driveDuty          = 0;
    bool     _driveDutyExplicit  = false;
    uint16_t _commandedDuty      = 0;
};

}  // namespace sfx_core

#endif  // SFX_HEATER_ROLE_H
