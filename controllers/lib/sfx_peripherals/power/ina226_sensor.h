/*
 * ina226_sensor.h — adapters that expose an `INA226` chip as a
 * `VoltageSensor` and a `CurrentSensor` for the port-layer sense
 * channels.
 *
 * One INA226 measures both rail voltage and shunt current.  In the
 * port-layer model those are two separate concepts (a `PwmBinding`
 * carries optional `VoltageSensor*` and `CurrentSensor*` independently),
 * so this header ships two thin adapter classes that point at the same
 * underlying chip:
 *
 *   INA226 ina(Wire, 0x40);
 *   Ina226VoltageSensor vSense(ina);
 *   Ina226CurrentSensor iSense(ina);
 *
 * Caller is responsible for running `ina.update()` periodically — the
 * adapters just expose the chip's cached readings.
 */

#ifndef SFX_INA226_SENSOR_H
#define SFX_INA226_SENSOR_H

#include <cstdint>

#include "ina226.h"
#include <ports/sensors.h>

namespace sfx_peripherals {

class Ina226VoltageSensor final : public VoltageSensor {
public:
    Ina226VoltageSensor(INA226& chip) : _chip(&chip) {}

    int16_t voltage_mV() const override {
        // INA226 reports up to ~36 V — fits comfortably in int16 (mV).
        return (int16_t)_chip->busVoltage_mV();
    }
    bool isAvailable() const override { return _chip->isAvailable(); }

private:
    INA226* _chip;
};

class Ina226CurrentSensor final : public CurrentSensor {
public:
    Ina226CurrentSensor(INA226& chip) : _chip(&chip) {}

    int16_t current_mA() const override {
        return (int16_t)_chip->current_mA();
    }
    bool isAvailable() const override { return _chip->isAvailable(); }

private:
    INA226* _chip;
};

}  // namespace sfx_peripherals

#endif  // SFX_INA226_SENSOR_H
