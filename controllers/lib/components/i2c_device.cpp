/*
 * I2C Device Base Class - Implementation
 * 
 * Common I2C register read/write, bus probing, and error tracking.
 */

#include "i2c_device.h"

// ============================================================================
// Initialization
// ============================================================================

bool I2CDevice::begin(TwoWire& wire, uint8_t address) {
    _wire = &wire;
    _address = address;
    _available = false;
    _errorCount = 0;
    _lastError = I2CError::OK;

    // Probe address — does any device ACK?
    if (!probe(wire, address)) {
        _lastError = I2CError::NACK_ADDRESS;
        return false;
    }

    // Verify device identity (virtual — subclass can check WHO_AM_I, etc.)
    if (!identify()) {
        return false;
    }

    _available = true;
    return true;
}

bool I2CDevice::identify() {
    return true;  // Default: accept any device that ACKs
}

// ============================================================================
// Static Bus Utilities
// ============================================================================

bool I2CDevice::probe(TwoWire& wire, uint8_t address) {
    wire.beginTransmission(address);
    return wire.endTransmission() == 0;
}

uint8_t I2CDevice::scan(TwoWire& wire, uint8_t minAddr, uint8_t maxAddr,
                         uint8_t* addresses, uint8_t maxDevices) {
    uint8_t found = 0;
    for (uint8_t addr = minAddr; addr <= maxAddr && found < maxDevices; addr++) {
        if (probe(wire, addr)) {
            addresses[found++] = addr;
        }
    }
    return found;
}

// ============================================================================
// Register I/O
// ============================================================================

bool I2CDevice::writeRegister8(uint8_t reg, uint8_t value) {
    if (!_wire) return false;

    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write(value);
    uint8_t err = _wire->endTransmission();
    if (err != 0) {
        _lastError = static_cast<I2CError>(err);
        _errorCount++;
        return false;
    }
    _lastError = I2CError::OK;
    return true;
}

bool I2CDevice::writeRegister16(uint8_t reg, uint16_t value) {
    if (!_wire) return false;

    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write((value >> 8) & 0xFF);  // MSB first (I2C convention)
    _wire->write(value & 0xFF);         // LSB
    uint8_t err = _wire->endTransmission();
    if (err != 0) {
        _lastError = static_cast<I2CError>(err);
        _errorCount++;
        return false;
    }
    _lastError = I2CError::OK;
    return true;
}

uint8_t I2CDevice::readRegister8(uint8_t reg, bool* ok) {
    if (!_wire) {
        if (ok) *ok = false;
        return 0;
    }

    _wire->beginTransmission(_address);
    _wire->write(reg);
    uint8_t err = _wire->endTransmission(false);  // Repeated start
    if (err != 0) {
        _lastError = static_cast<I2CError>(err);
        _errorCount++;
        if (ok) *ok = false;
        return 0;
    }

    _wire->requestFrom(_address, (uint8_t)1);
    if (_wire->available() >= 1) {
        _lastError = I2CError::OK;
        if (ok) *ok = true;
        return _wire->read();
    }

    _errorCount++;
    _lastError = I2CError::OTHER;
    if (ok) *ok = false;
    return 0;
}

uint16_t I2CDevice::readRegister16(uint8_t reg, bool* ok) {
    if (!_wire) {
        if (ok) *ok = false;
        return 0;
    }

    _wire->beginTransmission(_address);
    _wire->write(reg);
    uint8_t err = _wire->endTransmission(false);  // Repeated start
    if (err != 0) {
        _lastError = static_cast<I2CError>(err);
        _errorCount++;
        if (ok) *ok = false;
        return 0;
    }

    _wire->requestFrom(_address, (uint8_t)2);
    if (_wire->available() >= 2) {
        _lastError = I2CError::OK;
        if (ok) *ok = true;
        uint16_t value = _wire->read() << 8;  // MSB first
        value |= _wire->read();
        return value;
    }

    _errorCount++;
    _lastError = I2CError::OTHER;
    if (ok) *ok = false;
    return 0;
}

bool I2CDevice::writeBytes(uint8_t reg, const uint8_t* data, uint8_t len) {
    if (!_wire) return false;

    _wire->beginTransmission(_address);
    _wire->write(reg);
    for (uint8_t i = 0; i < len; i++) {
        _wire->write(data[i]);
    }
    uint8_t err = _wire->endTransmission();
    if (err != 0) {
        _lastError = static_cast<I2CError>(err);
        _errorCount++;
        return false;
    }
    _lastError = I2CError::OK;
    return true;
}

uint8_t I2CDevice::readBytes(uint8_t reg, uint8_t* data, uint8_t len) {
    if (!_wire) return 0;

    _wire->beginTransmission(_address);
    _wire->write(reg);
    uint8_t err = _wire->endTransmission(false);  // Repeated start
    if (err != 0) {
        _lastError = static_cast<I2CError>(err);
        _errorCount++;
        return 0;
    }

    _wire->requestFrom(_address, len);
    uint8_t count = 0;
    while (_wire->available() && count < len) {
        data[count++] = _wire->read();
    }
    if (count < len) {
        _errorCount++;
        _lastError = I2CError::OTHER;
    } else {
        _lastError = I2CError::OK;
    }
    return count;
}
