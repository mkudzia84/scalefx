/*
 * INA226 Power Monitor Library - Header
 * 
 * Texas Instruments INA226 high-precision current/power monitor.
 * 
 * Features:
 *   - Bus voltage measurement (0-36V)
 *   - Shunt voltage measurement (±81.92mV)
 *   - Current calculation via calibration
 *   - Power calculation
 *   - Configurable averaging and conversion time
 * 
 * Usage:
 *   INA226 powerMonitor;
 *   powerMonitor.begin(Wire, 0x40, 0.1f);  // 100mΩ shunt
 *   powerMonitor.update();
 *   float voltage = powerMonitor.busVoltage();
 *   float current = powerMonitor.current();
 */

#ifndef INA226_H
#define INA226_H

#include <Arduino.h>
#include <Wire.h>

// ============================================================================
// INA226 I2C Address Configuration
// ============================================================================

/**
 * The INA226 address is set by A0 and A1 pins:
 *   A1   A0   Address
 *   GND  GND  0x40 (default)
 *   GND  VS+  0x41
 *   GND  SDA  0x42
 *   GND  SCL  0x43
 *   VS+  GND  0x44
 *   VS+  VS+  0x45
 *   VS+  SDA  0x46
 *   VS+  SCL  0x47
 *   SDA  GND  0x48
 *   SDA  VS+  0x49
 *   SDA  SDA  0x4A
 *   SDA  SCL  0x4B
 *   SCL  GND  0x4C
 *   SCL  VS+  0x4D
 *   SCL  SDA  0x4E
 *   SCL  SCL  0x4F
 */
namespace INA226Address {
    constexpr uint8_t DEFAULT     = 0x40;
    constexpr uint8_t A0_VS       = 0x41;
    constexpr uint8_t A0_SDA      = 0x42;
    constexpr uint8_t A0_SCL      = 0x43;
    constexpr uint8_t A1_VS       = 0x44;
    constexpr uint8_t A1_VS_A0_VS = 0x45;
}

// ============================================================================
// INA226 Register Definitions
// ============================================================================

namespace INA226Reg {
    constexpr uint8_t CONFIG      = 0x00;  // Configuration Register
    constexpr uint8_t SHUNT_V     = 0x01;  // Shunt Voltage (R/O)
    constexpr uint8_t BUS_V       = 0x02;  // Bus Voltage (R/O)
    constexpr uint8_t POWER       = 0x03;  // Power (R/O)
    constexpr uint8_t CURRENT     = 0x04;  // Current (R/O)
    constexpr uint8_t CALIBRATION = 0x05;  // Calibration (R/W)
    constexpr uint8_t MASK_ENABLE = 0x06;  // Mask/Enable (R/W)
    constexpr uint8_t ALERT_LIMIT = 0x07;  // Alert Limit (R/W)
    constexpr uint8_t MFG_ID      = 0xFE;  // Manufacturer ID (R/O) = 0x5449
    constexpr uint8_t DIE_ID      = 0xFF;  // Die ID (R/O) = 0x2260
}

// ============================================================================
// INA226 Configuration Options
// ============================================================================

/**
 * Averaging mode - number of samples averaged for each measurement
 */
enum class INA226Averaging : uint8_t {
    AVG_1    = 0b000,  // 1 sample (default)
    AVG_4    = 0b001,  // 4 samples
    AVG_16   = 0b010,  // 16 samples
    AVG_64   = 0b011,  // 64 samples
    AVG_128  = 0b100,  // 128 samples
    AVG_256  = 0b101,  // 256 samples
    AVG_512  = 0b110,  // 512 samples
    AVG_1024 = 0b111   // 1024 samples
};

/**
 * Conversion time for voltage measurements
 */
enum class INA226ConvTime : uint8_t {
    CT_140US  = 0b000,  // 140µs
    CT_204US  = 0b001,  // 204µs
    CT_332US  = 0b010,  // 332µs
    CT_588US  = 0b011,  // 588µs
    CT_1100US = 0b100,  // 1.1ms (default)
    CT_2116US = 0b101,  // 2.116ms
    CT_4156US = 0b110,  // 4.156ms
    CT_8244US = 0b111   // 8.244ms
};

/**
 * Operating mode
 */
enum class INA226Mode : uint8_t {
    POWER_DOWN         = 0b000,
    SHUNT_TRIGGERED    = 0b001,
    BUS_TRIGGERED      = 0b010,
    SHUNT_BUS_TRIGGERED = 0b011,
    POWER_DOWN_2       = 0b100,
    SHUNT_CONTINUOUS   = 0b101,
    BUS_CONTINUOUS     = 0b110,
    SHUNT_BUS_CONTINUOUS = 0b111  // Default
};

// ============================================================================
// INA226 Class
// ============================================================================

class INA226 {
public:
    // LSB values (fixed by hardware)
    static constexpr float SHUNT_V_LSB = 2.5e-6f;   // 2.5µV per bit
    static constexpr float BUS_V_LSB   = 1.25e-3f;  // 1.25mV per bit
    
    // Expected ID values
    static constexpr uint16_t MANUFACTURER_ID = 0x5449;  // "TI"
    static constexpr uint16_t DIE_ID = 0x2260;
    
    INA226() = default;
    
    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the INA226
     * @param wire TwoWire instance to use
     * @param address I2C address (default 0x40)
     * @param shuntOhms Shunt resistor value in ohms
     * @param maxCurrentA Maximum expected current in amps (for calibration)
     * @return true if device found and initialized
     */
    bool begin(TwoWire& wire, uint8_t address = INA226Address::DEFAULT,
               float shuntOhms = 0.1f, float maxCurrentA = 3.2f);

    /**
     * @brief Check if device is available
     */
    bool isAvailable() const { return _available; }

    /**
     * @brief Reset the device
     */
    void reset();

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set averaging mode
     * @param avg Number of samples to average
     */
    void setAveraging(INA226Averaging avg);

    /**
     * @brief Set bus voltage conversion time
     * @param ct Conversion time
     */
    void setBusConversionTime(INA226ConvTime ct);

    /**
     * @brief Set shunt voltage conversion time
     * @param ct Conversion time
     */
    void setShuntConversionTime(INA226ConvTime ct);

    /**
     * @brief Set operating mode
     * @param mode Operating mode
     */
    void setMode(INA226Mode mode);

    /**
     * @brief Configure all settings at once
     */
    void configure(INA226Averaging avg, INA226ConvTime busCT, 
                   INA226ConvTime shuntCT, INA226Mode mode);

    /**
     * @brief Set calibration for current measurement
     * @param shuntOhms Shunt resistor value
     * @param maxCurrentA Maximum expected current
     */
    void setCalibration(float shuntOhms, float maxCurrentA);

    // ========================================================================
    // Measurements
    // ========================================================================

    /**
     * @brief Update all measurements (call periodically)
     */
    void update();

    /**
     * @brief Get bus voltage in volts
     */
    float busVoltage() const { return _busVoltage; }

    /**
     * @brief Get shunt voltage in volts
     */
    float shuntVoltage() const { return _shuntVoltage; }

    /**
     * @brief Get current in milliamps
     */
    float current() const { return _current; }

    /**
     * @brief Get power in milliwatts
     */
    float power() const { return _power; }

    /**
     * @brief Read bus voltage directly (without caching)
     */
    float readBusVoltage();

    /**
     * @brief Read shunt voltage directly (without caching)
     */
    float readShuntVoltage();

    /**
     * @brief Read current directly (without caching)
     */
    float readCurrent();

    /**
     * @brief Read power directly (without caching)
     */
    float readPower();

    // ========================================================================
    // Device Info
    // ========================================================================

    /**
     * @brief Get manufacturer ID (should be 0x5449 = "TI")
     */
    uint16_t manufacturerId();

    /**
     * @brief Get die ID (should be 0x2260)
     */
    uint16_t dieId();

    /**
     * @brief Get I2C address
     */
    uint8_t address() const { return _address; }

    /**
     * @brief Get current LSB value (mA per bit)
     */
    float currentLsb() const { return _currentLsb; }

private:
    void writeRegister(uint8_t reg, uint16_t value);
    uint16_t readRegister(uint8_t reg);
    void updateConfig();

    TwoWire* _wire = nullptr;
    uint8_t _address = INA226Address::DEFAULT;
    bool _available = false;
    
    // Configuration
    INA226Averaging _averaging = INA226Averaging::AVG_16;
    INA226ConvTime _busConvTime = INA226ConvTime::CT_1100US;
    INA226ConvTime _shuntConvTime = INA226ConvTime::CT_1100US;
    INA226Mode _mode = INA226Mode::SHUNT_BUS_CONTINUOUS;
    
    // Calibration
    float _shuntOhms = 0.1f;
    float _currentLsb = 0.0f;  // mA per bit
    float _powerLsb = 0.0f;    // mW per bit
    
    // Cached measurements
    float _busVoltage = 0.0f;
    float _shuntVoltage = 0.0f;
    float _current = 0.0f;
    float _power = 0.0f;
};

#endif // INA226_H
