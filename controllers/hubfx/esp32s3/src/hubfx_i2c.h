/*
 * hubfx_i2c.h — the HubFX board's single native I2C bus.
 *
 * One shared `SfxI2cBus` for the TAS5825P codec, the PCA9685, and the INA226(s)
 * — brought up once in setup() and used by the boot probe, the codec adapter,
 * and the board's I²C-scan.  Singleton (Rule 14) so the three call sites share
 * one bus instance without threading a reference through every layer.
 */

#ifndef HUBFX_I2C_H
#define HUBFX_I2C_H

#include <i2c/sfx_i2c.h>   // sfx_peripherals::SfxI2cBus (native ESP-IDF)

inline sfx_peripherals::SfxI2cBus& hubI2cBus() {
    static sfx_peripherals::SfxI2cBus bus;   // C++11 thread-safe function-local static
    return bus;
}

#endif  // HUBFX_I2C_H
