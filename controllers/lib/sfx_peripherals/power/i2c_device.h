/*
 * i2c_device.h — RTTI-free CRTP base for I2C peripheral drivers.
 *
 * Compile-time polymorphism (Rule 18/33): a driver derives from
 * `I2CDeviceT<Self>` and provides a non-virtual `bool identify()`; the base's
 * `attach()` probes the address then dispatches to `identify()` via CRTP — no
 * vtable, no RTTI.  Register I/O delegates to the native `SfxI2cBus` (ESP-IDF /
 * Pico), so drivers never name Arduino `TwoWire`.
 *
 * Usage:
 *   class MySensor : public sfx_peripherals::I2CDeviceT<MySensor> {
 *   public:
 *     bool identify() { return readRegister8(WHO_AM_I) == EXPECTED; }
 *     bool begin(SfxI2cBus& bus, uint8_t addr) {
 *       if (!attach(bus, addr)) return false;
 *       // device-specific init …
 *       return true;
 *     }
 *   };
 */

#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <cstdint>
#include <cstddef>
#include <cstring>

#include <i2c/sfx_i2c.h>   // sfx_peripherals::SfxI2cBus (native)

// I2C error codes (kept for API compatibility with existing drivers).
enum class I2CError : uint8_t {
    OK            = 0,
    DATA_TOO_LONG = 1,
    NACK_ADDRESS  = 2,
    NACK_DATA     = 3,
    OTHER         = 4,
    TIMEOUT       = 5
};

namespace sfx_peripherals {

template <class Derived>
class I2CDeviceT {
public:
    /// Probe the address + verify identity (CRTP → Derived::identify()).
    /// Subclasses call this from their own begin(), then do device init.
    bool attach(SfxI2cBus& bus, uint8_t address) {
        _bus = &bus;
        _address = address;
        _available = false;
        if (!bus.probe(address)) {
            _lastError = I2CError::NACK_ADDRESS;
            _errorCount++;
            return false;
        }
        if (!static_cast<Derived*>(this)->identify()) return false;
        _available = true;
        return true;
    }

    bool      isAvailable() const { return _available; }
    uint8_t   address()     const { return _address; }
    uint32_t  errorCount()  const { return _errorCount; }
    I2CError  lastError()   const { return _lastError; }

    /// Generic ACK probe / range scan (any device that ACKs).
    static bool probe(SfxI2cBus& bus, uint8_t address) { return bus.probe(address); }
    static uint8_t scan(SfxI2cBus& bus, uint8_t minAddr, uint8_t maxAddr,
                        uint8_t* out, uint8_t maxDevices) {
        uint8_t n = 0;
        for (uint16_t a = minAddr; a <= maxAddr && n < maxDevices; ++a)
            if (bus.probe((uint8_t)a)) out[n++] = (uint8_t)a;
        return n;
    }

protected:
    bool writeRegister8(uint8_t reg, uint8_t value) {
        return track(_bus && _bus->writeReg(_address, reg, &value, 1));
    }
    bool writeRegister16(uint8_t reg, uint16_t value) {     // MSB first
        const uint8_t b[2] = { (uint8_t)(value >> 8), (uint8_t)value };
        return track(_bus && _bus->writeReg(_address, reg, b, 2));
    }
    uint8_t readRegister8(uint8_t reg, bool* ok = nullptr) {
        uint8_t v = 0;
        const bool r = _bus && _bus->readReg(_address, reg, &v, 1);
        track(r); if (ok) *ok = r; return r ? v : 0;
    }
    uint16_t readRegister16(uint8_t reg, bool* ok = nullptr) {   // MSB first
        uint8_t b[2] = { 0, 0 };
        const bool r = _bus && _bus->readReg(_address, reg, b, 2);
        track(r); if (ok) *ok = r;
        return r ? (uint16_t)((b[0] << 8) | b[1]) : 0;
    }
    bool writeBytes(uint8_t reg, const uint8_t* data, uint8_t len) {
        return track(_bus && _bus->writeReg(_address, reg, data, len));
    }
    uint8_t readBytes(uint8_t reg, uint8_t* data, uint8_t len) {
        const bool r = _bus && _bus->readReg(_address, reg, data, len);
        track(r); return r ? len : 0;
    }

    bool track(bool ok) {
        if (!ok) { _errorCount++; if (_lastError == I2CError::OK) _lastError = I2CError::OTHER; }
        return ok;
    }

    SfxI2cBus* _bus        = nullptr;
    uint8_t    _address    = 0;
    bool       _available  = false;
    uint32_t   _errorCount = 0;
    I2CError   _lastError  = I2CError::OK;
};

}  // namespace sfx_peripherals

#endif  // I2C_DEVICE_H
