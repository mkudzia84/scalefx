/*
 * INA226 Power Monitor Library - Implementation
 * 
 * Texas Instruments INA226 high-precision current/power monitor.
 */

#include "ina226.h"
#include "platform/sfx_platform.h"

// ============================================================================
// Initialization
// ============================================================================

bool INA226::begin(TwoWire& wire, uint8_t address, float shuntResistance_ohms, float maxCurrent_A) {
    _shuntResistance_ohms = shuntResistance_ohms;

    // Base class: store wire/address, probe bus, call identify()
    if (!I2CDevice::begin(wire, address)) {
        return false;
    }

    // INA226-specific initialization
    reset();
    SFX_DELAY_MS(1);
    setCalibration(shuntResistance_ohms, maxCurrent_A);
    configure(INA226Averaging::AVG_16, 
              INA226ConvTime::CT_1100US,
              INA226ConvTime::CT_1100US,
              INA226Mode::SHUNT_BUS_CONTINUOUS);
    
    return true;
}

bool INA226::begin(TwoWire& wire, const INA226Config& config) {
    _channel = config.channel;
    if (!begin(wire, config.address, config.shuntResistance_ohms, config.maxCurrent_A)) {
        return false;
    }
    // Apply config-specific settings (overrides defaults from begin())
    configure(config.averaging, config.busConvTime, config.shuntConvTime, config.mode);
    return true;
}

bool INA226::identify() {
    uint16_t mfgId = readRegister16(INA226Reg::MFG_ID);
    if (mfgId != MANUFACTURER_ID) return false;
    uint16_t dId = readRegister16(INA226Reg::DIE_ID);
    if (dId != DIE_ID) return false;
    return true;
}

uint8_t INA226::scan(TwoWire& wire, uint8_t* addresses, uint8_t maxDevices) {
    uint8_t found = 0;
    for (uint8_t addr = INA226Address::MIN; addr <= INA226Address::MAX && found < maxDevices; addr++) {
        if (!probe(wire, addr)) continue;

        // Verify identity using temporary instance with base class I/O
        INA226 temp;
        temp._wire = &wire;
        temp._address = addr;
        if (temp.identify()) {
            addresses[found++] = addr;
        }
    }
    return found;
}

void INA226::reset() {
    writeRegister16(INA226Reg::CONFIG, INA226Reg::CFG_RESET);
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
    if (!_available) return;
    
    uint16_t config = 0;
    config |= ((uint8_t)_averaging & 0x07) << INA226Reg::CFG_AVG_SHIFT;
    config |= ((uint8_t)_busConvTime & 0x07) << INA226Reg::CFG_VBUS_SHIFT;
    config |= ((uint8_t)_shuntConvTime & 0x07) << INA226Reg::CFG_VSHUNT_SHIFT;
    config |= ((uint8_t)_mode & 0x07);
    
    writeRegister16(INA226Reg::CONFIG, config);
}

void INA226::setCalibration(float shuntResistance_ohms, float maxCurrent_A) {
    _shuntResistance_ohms = shuntResistance_ohms;
    
    // Current_LSB = MaxCurrent / 2^15 (INA226 datasheet §7.6.3)
    _currentLsb_mA = (maxCurrent_A * 1000.0f) / 32768.0f;  // mA per bit
    
    // CAL = 0.00512 / (Current_LSB_A x R_shunt) (INA226 datasheet §7.6.5)
    float currentLsbA = _currentLsb_mA / 1000.0f;  // Convert to amps for formula
    uint16_t calibration = (uint16_t)(0.00512f / (currentLsbA * shuntResistance_ohms));
    
    // Power_LSB = 25 × Current_LSB (INA226 datasheet §7.6.4)
    _powerLsb_mW = 25.0f * _currentLsb_mA;  // mW per bit
    
    writeRegister16(INA226Reg::CALIBRATION, calibration);
}

// ============================================================================
// Measurements
// ============================================================================

void INA226::update() {
    if (!_available) return;
    
    _busVoltage_mV = readBusVoltage_mV();
    _shuntVoltage_uV = readShuntVoltage_uV();
    _current_mA = readCurrent_mA();
    _power_mW = readPower_mW();
}

float INA226::readBusVoltage_mV() {
    int16_t raw = (int16_t)readRegister16(INA226Reg::BUS_V);
    return raw * BUS_V_LSB_mV;  // mV (LSB = 1.25 mV, INA226 §7.6.2)
}

float INA226::readShuntVoltage_uV() {
    int16_t raw = (int16_t)readRegister16(INA226Reg::SHUNT_V);
    return raw * SHUNT_V_LSB_uV;  // µV (LSB = 2.5 µV, INA226 §7.6.1)
}

float INA226::readCurrent_mA() {
    int16_t raw = (int16_t)readRegister16(INA226Reg::CURRENT);
    return raw * _currentLsb_mA;  // mA (LSB set by calibration, INA226 §7.6.3)
}

float INA226::readPower_mW() {
    uint16_t raw = readRegister16(INA226Reg::POWER);
    return raw * _powerLsb_mW;  // mW (LSB = 25 × Current_LSB, INA226 §7.6.4)
}

// ============================================================================
// Device Info
// ============================================================================

uint16_t INA226::manufacturerId() {
    return readRegister16(INA226Reg::MFG_ID);
}

uint16_t INA226::dieId() {
    return readRegister16(INA226Reg::DIE_ID);
}
