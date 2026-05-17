/*
 * sensors.h — abstract sensor interfaces paired with PWM / H-bridge ports.
 *
 * A port can carry up to three optional sense channels.  Each is an
 * abstract interface — the board's driver setup picks the concrete
 * implementation (INA226 channel, ADC divider with NTC, …) and binds it
 * to the port via the descriptor's `with_iSense<>()`, `with_vSense<>()`,
 * `with_tSense<>()` helpers.
 *
 * Implementations live alongside the underlying hardware drivers in
 * sfx_peripherals (e.g., `power/ina226_battery.cpp` already exposes the
 * relevant samplers — adapter shims wrap them as `VoltageSensor` /
 * `CurrentSensor` here).
 */

#ifndef SFX_PERIPHERAL_PORT_SENSORS_H
#define SFX_PERIPHERAL_PORT_SENSORS_H

#include <cstdint>

namespace sfx_peripherals {

// ============================================================================
// Sensor interfaces
// ============================================================================

class VoltageSensor {
public:
    virtual ~VoltageSensor() = default;
    /// Current rail voltage in millivolts.  Returns 0 if not available.
    virtual int16_t voltage_mV() const = 0;
    /// True iff the most recent reading was successful.
    virtual bool    isAvailable() const = 0;
};

class CurrentSensor {
public:
    virtual ~CurrentSensor() = default;
    /// Current draw in milliamps.  Signed for bidirectional channels.
    virtual int16_t current_mA() const = 0;
    virtual bool    isAvailable() const = 0;
};

class TemperatureSensor {
public:
    virtual ~TemperatureSensor() = default;
    /// Temperature in tenths of degrees Celsius (e.g., 235 → 23.5 °C).
    virtual int16_t temperature_cx10() const = 0;
    virtual bool    isAvailable() const = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_PERIPHERAL_PORT_SENSORS_H
