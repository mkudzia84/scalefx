/*
 * esp_i2c_bus.h — native ESP-IDF I2C master bus (no Arduino Wire).
 *
 * Thin, concrete (no virtual, no RTTI) wrapper over the ESP-IDF legacy
 * `driver/i2c.h` master API.  Maps 1:1 to the Wire transaction model the
 * ScaleFX drivers were written against (`write_to_device` /
 * `write_read_device`), so the I2CDevice + INA226/PCA9685/TAS5825 drivers port
 * by swapping the bus type only.
 *
 * One instance per physical bus; multiple devices share it by address.  See
 * `sfx_i2c.h` for the platform-selected `SfxI2cBus` typedef.
 *
 * NOTE (bench): native I2C is behaviourally different from Arduino Wire only in
 * the driver underneath — the wire transactions are identical.  Still smoke-test
 * after flashing (codec audio, INA226 reads, PCA9685 PWM) since this swaps the
 * bus driver.
 */

#ifndef SFX_ESP_I2C_BUS_H
#define SFX_ESP_I2C_BUS_H

#include <platform/sfx_platform.h>
#if SFX_PLATFORM_ESP32

#include <cstdint>
#include <cstring>
#include <driver/i2c.h>

namespace sfx_peripherals {

class EspI2cBus {
public:
    EspI2cBus() = default;

    /// Bring up the bus on the given pins.  Idempotent: a second begin() on the
    /// same port re-configures without re-installing the driver.
    bool begin(int sda, int scl, uint32_t hz = 400000,
               i2c_port_t port = I2C_NUM_0) {
        _port = port;
        i2c_config_t cfg = {};
        cfg.mode             = I2C_MODE_MASTER;
        cfg.sda_io_num       = sda;
        cfg.scl_io_num       = scl;
        cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        cfg.master.clk_speed = hz;
        if (i2c_param_config(_port, &cfg) != ESP_OK) return false;
        if (!_installed) {
            const esp_err_t e = i2c_driver_install(_port, I2C_MODE_MASTER, 0, 0, 0);
            if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
            _installed = true;
        }
        _ready = true;
        return true;
    }

    void setClock(uint32_t hz) { _clockHz = hz; (void)hz; }   // applied at begin()
    bool ready() const { return _ready; }

    /// ACK-probe an address.  True if a device responds.
    bool probe(uint8_t addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        const esp_err_t e = i2c_master_cmd_begin(_port, cmd, kTimeoutTicks);
        i2c_cmd_link_delete(cmd);
        return e == ESP_OK;
    }

    /// Write raw bytes to a device (no register).
    bool write(uint8_t addr, const uint8_t* data, size_t len) {
        return i2c_master_write_to_device(_port, addr, data, len, kTimeoutTicks) == ESP_OK;
    }
    /// Read raw bytes from a device (no register).
    bool read(uint8_t addr, uint8_t* buf, size_t len) {
        return i2c_master_read_from_device(_port, addr, buf, len, kTimeoutTicks) == ESP_OK;
    }

    /// Write `len` bytes to `reg` (register address prepended).
    bool writeReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len) {
        uint8_t buf[1 + kMaxRegWrite];
        if (len > kMaxRegWrite) return false;
        buf[0] = reg;
        if (len) std::memcpy(&buf[1], data, len);
        return i2c_master_write_to_device(_port, addr, buf, 1 + len, kTimeoutTicks) == ESP_OK;
    }
    /// Read `len` bytes from `reg` (repeated-start).
    bool readReg(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
        return i2c_master_write_read_device(_port, addr, &reg, 1, buf, len,
                                            kTimeoutTicks) == ESP_OK;
    }

private:
    static constexpr TickType_t kTimeoutTicks = pdMS_TO_TICKS(50);
    static constexpr size_t     kMaxRegWrite  = 32;   // largest single reg write
    i2c_port_t _port      = I2C_NUM_0;
    uint32_t   _clockHz   = 400000;
    bool       _installed = false;
    bool       _ready     = false;
};

}  // namespace sfx_peripherals

#endif  // SFX_PLATFORM_ESP32
#endif  // SFX_ESP_I2C_BUS_H
