/*
 * sfx_i2c.h — platform-selected native I2C bus.
 *
 * Resolves `sfx_peripherals::SfxI2cBus` to the concrete native bus for the
 * target (EspI2cBus on ESP-IDF, PicoI2cBus on the Pico SDK).  Compile-time
 * dispatch only — no virtual, no RTTI.  Drivers (I2CDevice + INA226 / PCA9685 /
 * TAS5825) name `SfxI2cBus`, never a platform type or Arduino `TwoWire`.
 */

#ifndef SFX_I2C_H
#define SFX_I2C_H

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32
    #include "esp_i2c_bus.h"
    namespace sfx_peripherals { using SfxI2cBus = EspI2cBus; }
#elif SFX_PLATFORM_PICO
    #include "pico_i2c_bus.h"
    namespace sfx_peripherals { using SfxI2cBus = PicoI2cBus; }
#endif

#endif  // SFX_I2C_H
