/*
 * HeaterRole — heater on a `PwmPort`, dual-mode.
 *
 *   Closed-loop (a `TemperatureSensor` is bound): holds the temperature
 *   within `±_hysteresis_cx10` of `_target_cx10`.  Below the lower band
 *   → PWM at `_driveDuty`.  Above the upper band → PWM 0.  Inside the
 *   band → keep current state (hysteresis).  Both target and hysteresis
 *   are in tenths of degrees Celsius (235 → 23.5 °C).
 *
 *   Open-loop (no sensor bound): the role behaves as a simple gated
 *   PWM driver.  `_target_cx10 == INT16_MIN` → off; any other target
 *   value → drive at `_driveDuty` (defaults to the port's max duty).
 *   Used by low-power scale-model loads (smoke cartridges, vapor cells)
 *   that don't need closed-loop control.
 */

#ifndef SFX_HEATER_ROLE_H
#define SFX_HEATER_ROLE_H

#include <Arduino.h>
#include <cstdint>

#include <ports/pwm_port.h>
#include <ports/sensors.h>

#include "../element/element_scaling.h"

namespace sfx_core {

class HeaterRole {
public:
    HeaterRole() = default;
    HeaterRole(sfx_peripherals::PwmPort*           port,
               sfx_peripherals::TemperatureSensor* tSense)
        : _port(port), _tSense(tSense) {}

    /// Binds the port and (optionally) a temperature sensor.  `tSense`
    /// may be null → open-loop mode (drive duty when target is set, 0
    /// when target is INT16_MIN).
    bool bind(sfx_peripherals::PwmPort*           port,
              sfx_peripherals::TemperatureSensor* tSense) {
        if (!port) return false;
        _port = port; _tSense = tSense;
        return true;
    }

    /// Set target temperature in tenths of °C.  `target_cx10 == INT16_MIN`
    /// = off.  Default = INT16_MIN (off until configured).
    void setTarget(int16_t target_cx10);
    int16_t target() const { return _target_cx10; }

    /// Hysteresis half-width in tenths of °C (default 10 = ±1.0 °C).
    void setHysteresis(int16_t hysteresis_cx10) { _hysteresis_cx10 = hysteresis_cx10; }

    /// Drive duty applied while heating, expressed as 0..100 % of the
    /// element's RATED voltage. Defaults to 100 % (full-on, full-off
    /// bang-bang at the element's nameplate rating). Combined with the
    /// element config below to compute the actual port-native duty
    /// that delivers `_drivePct%` of the element's voltage.
    /// (Phase 2 of GunFX rollout, instructions/22 — voltage scaling
    /// moved to the role layer.)
    void setDrivePct(uint8_t pct)         { _drivePct = pct; }

    /// Element rated voltage + scaling mode (Phase 2). When both are
    /// set + the port carries `portMv` (Phase 0), the role drives at
    /// `(elementMv/portMv [² for Quadratic]) * drivePct` of port-max
    /// duty. `elementMv = 0` or `portMv = 0` → passthrough.
    void setElement(const ElementConfig& cfg)   { _element = cfg; }
    void setPortRailMv(uint16_t portMv)         { _portRailMv = portMv; }
    const ElementConfig& element() const        { return _element; }
    uint16_t portRailMv() const                 { return _portRailMv; }
    uint8_t  drivePct() const                   { return _drivePct; }
    int16_t  hysteresis_cx10() const            { return _hysteresis_cx10; }

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

    // Element scaling (Phase 2) — duty is computed from drivePct +
    // element vs port voltage. Falls back to 100 % at port-max when
    // either voltage is unknown.
    uint8_t       _drivePct      = 100;
    ElementConfig _element       = {};
    uint16_t      _portRailMv    = 0;
    uint16_t      _commandedDuty = 0;
};

}  // namespace sfx_core

#endif  // SFX_HEATER_ROLE_H
