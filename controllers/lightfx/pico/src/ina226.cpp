/*
 * INA226 Power Monitor Library - Implementation
 * 
 * Texas Instruments INA226 high-precision current/power monitor.
 */

#include "ina226.h"

// ============================================================================
// Initialization
// ============================================================================

bool INA226::begin(TwoWire& wire, uint8_t address, float shuntOhms, float maxCurrentA) {
    _wire = &wire;
    _address = address;
    _shuntOhms = shuntOhms;
    _available = false;
    
    // Check manufacturer ID
    uint16_t mfgId = manufacturerId();
    if (mfgId != MANUFACTURER_ID) {
        return false;
    }
    
    // Check die ID
    uint16_t dId = dieId();
    if (dId != DIE_ID) {
        return false;
    }
    
    _available = true;
    
    // Reset and configure
    reset();
    delay(1);
    
    // Set calibration
    setCalibration(shuntOhms, maxCurrentA);
    
    // Configure with sensible defaults
    configure(INA226Averaging::AVG_16, 
              INA226ConvTime::CT_1100US,
              INA226ConvTime::CT_1100US,
              INA226Mode::SHUNT_BUS_CONTINUOUS);
    
    return true;
}

void INA226::reset() {
    if (!_wire) return;
    writeRegister(INA226Reg::CONFIG, 0x8000);  // Set reset bit
}

// ============================================================================
// Configuration
// ============================================================================

void INA226::setAveraging(INA226Averaging avg) {
    _averaging = avg;
    updateConfig();
}

void INA226::setBusConversionTime(INA226ConvTime ct) {
    _busConvTime = ct;
    updateConfig();
}

void INA226::setShuntConversionTime(INA226ConvTime ct) {
    _shuntConvTime = ct;
    updateConfig();
}

void INA226::setMode(INA226Mode mode) {
    _mode = mode;
    updateConfig();
}

void INA226::configure(INA226Averaging avg, INA226ConvTime busCT,
                       INA226ConvTime shuntCT, INA226Mode mode) {
    _averaging = avg;
    _busConvTime = busCT;
    _shuntConvTime = shuntCT;
    _mode = mode;
    updateConfig();
}

void INA226::updateConfig() {
    if (!_wire || !_available) return;
    
    uint16_t config = 0;
    config |= ((uint8_t)_averaging & 0x07) << 9;
    config |= ((uint8_t)_busConvTime & 0x07) << 6;
    config |= ((uint8_t)_shuntConvTime & 0x07) << 3;
    config |= ((uint8_t)_mode & 0x07);
    
    writeRegister(INA226Reg::CONFIG, config);
}

void INA226::setCalibration(float shuntOhms, float maxCurrentA) {
    _shuntOhms = shuntOhms;
    
    // Calculate Current_LSB = MaxCurrent / 2^15
    _currentLsb = (maxCurrentA * 1000.0f) / 32768.0f;  // mA per bit
    
    // Calculate Calibration = 0.00512 / (Current_LSB_A * R_shunt)
    float currentLsbA = _currentLsb / 1000.0f;  // Convert to amps
    uint16_t calibration = (uint16_t)(0.00512f / (currentLsbA * shuntOhms));
    
    // Power_LSB = 25 * Current_LSB
    _powerLsb = 25.0f * _currentLsb;  // mW per bit
    
    writeRegister(INA226Reg::CALIBRATION, calibration);
}

// ============================================================================
// Measurements
// ============================================================================

void INA226::update() {
    if (!_available) return;
    
    _busVoltage = readBusVoltage();
    _shuntVoltage = readShuntVoltage();
    _current = readCurrent();
    _power = readPower();
}

float INA226::readBusVoltage() {
    int16_t raw = (int16_t)readRegister(INA226Reg::BUS_V);
    return raw * BUS_V_LSB;  // Volts
}

float INA226::readShuntVoltage() {
    int16_t raw = (int16_t)readRegister(INA226Reg::SHUNT_V);
    return raw * SHUNT_V_LSB;  // Volts
}

float INA226::readCurrent() {
    int16_t raw = (int16_t)readRegister(INA226Reg::CURRENT);
    return raw * _currentLsb;  // mA
}

float INA226::readPower() {
    uint16_t raw = readRegister(INA226Reg::POWER);
    return raw * _powerLsb;  // mW
}

// ============================================================================
// Device Info
// ============================================================================

uint16_t INA226::manufacturerId() {
    return readRegister(INA226Reg::MFG_ID);
}

uint16_t INA226::dieId() {
    return readRegister(INA226Reg::DIE_ID);
}

// ============================================================================
// Private Methods
// ============================================================================

void INA226::writeRegister(uint8_t reg, uint16_t value) {
    if (!_wire) return;
    
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write((value >> 8) & 0xFF);  // MSB first
    _wire->write(value & 0xFF);
    _wire->endTransmission();
}

uint16_t INA226::readRegister(uint8_t reg) {
    if (!_wire) return 0;
    
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->endTransmission(false);  // Repeated start
    
    _wire->requestFrom(_address, (uint8_t)2);
    if (_wire->available() >= 2) {
        uint16_t value = _wire->read() << 8;
        value |= _wire->read();
        return value;
    }
    return 0;
}
