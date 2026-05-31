/*
 * pico_i2c_bus.h — native Raspberry Pi Pico SDK I2C master bus (no Arduino Wire).
 *
 * Concrete (no virtual / RTTI) wrapper over `hardware/i2c.h`, matching the
 * EspI2cBus surface so the shared drivers (I2CDevice + INA226/PCA9685) are
 * bus-type-agnostic.  Selected via `sfx_i2c.h`.
 */

#ifndef SFX_PICO_I2C_BUS_H
#define SFX_PICO_I2C_BUS_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_PICO

#include <cstdint>
#include <cstring>
#include <hardware/i2c.h>
#include <hardware/gpio.h>

namespace sfx_peripherals {

class PicoI2cBus {
public:
    PicoI2cBus() = default;

    bool begin(int sda, int scl, uint32_t hz = 400000, i2c_inst_t* inst = i2c0) {
        _inst = inst;
        i2c_init(_inst, hz);
        gpio_set_function((uint)sda, GPIO_FUNC_I2C);
        gpio_set_function((uint)scl, GPIO_FUNC_I2C);
        gpio_pull_up((uint)sda);
        gpio_pull_up((uint)scl);
        _ready = true;
        return true;
    }

    void setClock(uint32_t hz) { if (_inst) i2c_set_baudrate(_inst, hz); }
    bool ready() const { return _ready; }

    bool probe(uint8_t addr) {
        uint8_t dummy = 0;
        return i2c_read_blocking(_inst, addr, &dummy, 1, false) >= 0;
    }

    bool write(uint8_t addr, const uint8_t* data, size_t len) {
        return i2c_write_blocking(_inst, addr, data, len, false) == (int)len;
    }
    bool read(uint8_t addr, uint8_t* buf, size_t len) {
        return i2c_read_blocking(_inst, addr, buf, len, false) == (int)len;
    }
    bool writeReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len) {
        uint8_t buf[1 + kMaxRegWrite];
        if (len > kMaxRegWrite) return false;
        buf[0] = reg;
        if (len) std::memcpy(&buf[1], data, len);
        return i2c_write_blocking(_inst, addr, buf, 1 + len, false) == (int)(1 + len);
    }
    bool readReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
        if (i2c_write_blocking(_inst, addr, &reg, 1, true) != 1) return false;
        return i2c_read_blocking(_inst, addr, buf, len, false) == (int)len;
    }

private:
    static constexpr size_t kMaxRegWrite = 32;
    i2c_inst_t* _inst  = nullptr;
    bool        _ready = false;
};

}  // namespace sfx_peripherals

#endif  // SFX_PLATFORM_PICO
#endif  // SFX_PICO_I2C_BUS_H
