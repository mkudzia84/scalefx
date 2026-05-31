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
 *   float voltage_mV = powerMonitor.busVoltage_mV();  // millivolts
 *   float current_mA = powerMonitor.current_mA();     // milliamps
 *   float power_mW   = powerMonitor.power_mW();       // milliwatts
 */

#ifndef INA226_H
#define INA226_H

#include "i2c_device.h"

// ============================================================================
// INA226 I2C Address Configuration (§8.5.5.1)
// ============================================================================

/**
 * The INA226 7-bit slave address is configured via A0 and A1 pins.
 * Address format: 0b100_[A1]_[A0]  (base = 0x40, range 0x40–0x4F)
 *
 *   A1 selects bits [3:2], A0 selects bits [1:0]:
 *     GND = 0b00, VS+ = 0b01, SDA = 0b10, SCL = 0b11
 *
 *   A1   A0   Addr   Binary (7-bit: base|A1|A0)
 *   ---  ---  ----   -------------------------
 *   GND  GND  0x40   100 | 00 | 00
 *   GND  VS+  0x41   100 | 00 | 01
 *   GND  SDA  0x42   100 | 00 | 10
 *   GND  SCL  0x43   100 | 00 | 11
 *   VS+  GND  0x44   100 | 01 | 00
 *   VS+  VS+  0x45   100 | 01 | 01
 *   VS+  SDA  0x46   100 | 01 | 10
 *   VS+  SCL  0x47   100 | 01 | 11
 *   SDA  GND  0x48   100 | 10 | 00
 *   SDA  VS+  0x49   100 | 10 | 01
 *   SDA  SDA  0x4A   100 | 10 | 10
 *   SDA  SCL  0x4B   100 | 10 | 11
 *   SCL  GND  0x4C   100 | 11 | 00
 *   SCL  VS+  0x4D   100 | 11 | 01
 *   SCL  SDA  0x4E   100 | 11 | 10
 *   SCL  SCL  0x4F   100 | 11 | 11
 *
 * Bitmasks:
 *   Address base:  0x40 (bits [6:4] = 100)
 *   A0 field:      bits [1:0], mask = 0x03
 *   A1 field:      bits [3:2], mask = 0x0C
 *   Full range:    0x40-0x4F, offset mask = 0x0F
 */
namespace INA226Address {
    // Address field masks
    constexpr uint8_t BASE     = 0x40;  // Base address (A0=GND, A1=GND)
    constexpr uint8_t A0_MASK  = 0x03;  // Bits [1:0] — A0 pin setting
    constexpr uint8_t A1_MASK  = 0x0C;  // Bits [3:2] — A1 pin setting
    constexpr uint8_t RANGE    = 0x0F;  // Bits [3:0] — full address offset mask
    constexpr uint8_t MIN      = 0x40;
    constexpr uint8_t MAX      = 0x4F;

    // All 16 addresses (A1=row, A0=column)
    constexpr uint8_t GND_GND  = 0x40;  // A1=GND, A0=GND (default)
    constexpr uint8_t GND_VS   = 0x41;  // A1=GND, A0=VS+
    constexpr uint8_t GND_SDA  = 0x42;  // A1=GND, A0=SDA
    constexpr uint8_t GND_SCL  = 0x43;  // A1=GND, A0=SCL
    constexpr uint8_t VS_GND   = 0x44;  // A1=VS+, A0=GND
    constexpr uint8_t VS_VS    = 0x45;  // A1=VS+, A0=VS+
    constexpr uint8_t VS_SDA   = 0x46;  // A1=VS+, A0=SDA
    constexpr uint8_t VS_SCL   = 0x47;  // A1=VS+, A0=SCL
    constexpr uint8_t SDA_GND  = 0x48;  // A1=SDA, A0=GND
    constexpr uint8_t SDA_VS   = 0x49;  // A1=SDA, A0=VS+
    constexpr uint8_t SDA_SDA  = 0x4A;  // A1=SDA, A0=SDA
    constexpr uint8_t SDA_SCL  = 0x4B;  // A1=SDA, A0=SCL
    constexpr uint8_t SCL_GND  = 0x4C;  // A1=SCL, A0=GND
    constexpr uint8_t SCL_VS   = 0x4D;  // A1=SCL, A0=VS+
    constexpr uint8_t SCL_SDA  = 0x4E;  // A1=SCL, A0=SDA
    constexpr uint8_t SCL_SCL  = 0x4F;  // A1=SCL, A0=SCL

    // Default address alias (named DEFAULT_ADDR to avoid collision with
    // Arduino.h's `#define DEFAULT 1` macro on ESP32)
    constexpr uint8_t DEFAULT_ADDR = GND_GND;

    /// Check if an address is within the valid INA226 range
    constexpr bool isValid(uint8_t addr) {
        return addr >= MIN && addr <= MAX;
    }
}

// ============================================================================
// INA226 Register Definitions (§7.6)
// ============================================================================

namespace INA226Reg {
    // Register addresses
    constexpr uint8_t CONFIG      = 0x00;  // Configuration Register (R/W, reset: 0x4127)
    constexpr uint8_t SHUNT_V     = 0x01;  // Shunt Voltage (R/O)
    constexpr uint8_t BUS_V       = 0x02;  // Bus Voltage (R/O)
    constexpr uint8_t POWER       = 0x03;  // Power (R/O)
    constexpr uint8_t CURRENT     = 0x04;  // Current (R/O)
    constexpr uint8_t CALIBRATION = 0x05;  // Calibration (R/W)
    constexpr uint8_t MASK_ENABLE = 0x06;  // Mask/Enable (R/W)
    constexpr uint8_t ALERT_LIMIT = 0x07;  // Alert Limit (R/W)
    constexpr uint8_t MFG_ID      = 0xFE;  // Manufacturer ID (R/O) = 0x5449
    constexpr uint8_t DIE_ID      = 0xFF;  // Die ID (R/O) = 0x2260

    // CONFIG register bitmasks (§7.6.1)
    //   Bit layout: [RST:15][---:14-12][AVG:11-9][VBUSCT:8-6][VSHCT:5-3][MODE:2-0]
    constexpr uint16_t CFG_RESET        = 0x8000;  // Bit 15: Software reset
    constexpr uint16_t CFG_AVG_MASK     = 0x0E00;  // Bits [11:9]: Averaging mode
    constexpr uint8_t  CFG_AVG_SHIFT    = 9;
    constexpr uint16_t CFG_VBUS_MASK    = 0x01C0;  // Bits [8:6]: Bus voltage conv time
    constexpr uint8_t  CFG_VBUS_SHIFT   = 6;
    constexpr uint16_t CFG_VSHUNT_MASK  = 0x0038;  // Bits [5:3]: Shunt voltage conv time
    constexpr uint8_t  CFG_VSHUNT_SHIFT = 3;
    constexpr uint16_t CFG_MODE_MASK    = 0x0007;  // Bits [2:0]: Operating mode
    constexpr uint16_t CFG_DEFAULT      = 0x4127;  // Reset/default value

    // MASK/ENABLE register bitmasks (§7.6.7)
    //   Bit layout: [SOL:15][SUL:14][BOL:13][BUL:12][POL:11][CNVR:10][---:9-6]
    //               [AFF:5][CVRF:4][OVF:3][APOL:2][LEN:1][---:0]
    constexpr uint16_t MASK_SOL   = 0x8000;  // Shunt Over-Voltage (alert enable)
    constexpr uint16_t MASK_SUL   = 0x4000;  // Shunt Under-Voltage (alert enable)
    constexpr uint16_t MASK_BOL   = 0x2000;  // Bus Over-Voltage (alert enable)
    constexpr uint16_t MASK_BUL   = 0x1000;  // Bus Under-Voltage (alert enable)
    constexpr uint16_t MASK_POL   = 0x0800;  // Power Over-Limit (alert enable)
    constexpr uint16_t MASK_CNVR  = 0x0400;  // Conversion Ready (alert enable)
    constexpr uint16_t FLAG_AFF   = 0x0020;  // Alert Function Flag (R/O)
    constexpr uint16_t FLAG_CVRF  = 0x0010;  // Conversion Ready Flag (R/O)
    constexpr uint16_t FLAG_OVF   = 0x0008;  // Math Overflow Flag (R/O)
    constexpr uint16_t CFG_APOL   = 0x0004;  // Alert Polarity (0=active-low)
    constexpr uint16_t CFG_LEN    = 0x0002;  // Alert Latch Enable
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
// INA226 Channel Configuration
// ============================================================================

/**
 * Configuration for a single INA226 channel.
 * Use with begin(wire, config) for cleaner multi-channel initialization.
 *
 * Example (two monitors on the same bus):
 *   INA226Config configs[] = {
 *       { .address = INA226Address::GND_GND, .shuntResistance_ohms = 0.1f,
 *         .maxCurrent_A = 3.2f, .channel = 0 },
 *       { .address = INA226Address::GND_VS,  .shuntResistance_ohms = 0.05f,
 *         .maxCurrent_A = 6.4f, .channel = 1 },
 *   };
 */
struct INA226Config {
    uint8_t address = INA226Address::DEFAULT_ADDR;         // I2C address (0x40-0x4F)
    float shuntResistance_ohms = 0.1f;                     // Shunt resistor value
    float maxCurrent_A = 3.2f;                             // Max expected current (calibration)
    INA226Averaging averaging = INA226Averaging::AVG_16;
    INA226ConvTime busConvTime = INA226ConvTime::CT_1100US;
    INA226ConvTime shuntConvTime = INA226ConvTime::CT_1100US;
    INA226Mode mode = INA226Mode::SHUNT_BUS_CONTINUOUS;
    uint8_t channel = 0;                                   // Channel index for identification
};

// ============================================================================
// INA226 Class
// ============================================================================

class INA226 : public sfx_peripherals::I2CDeviceT<INA226> {
public:
    // LSB values per INA226 datasheet (TI SBOS547) §7.6
    static constexpr float SHUNT_V_LSB_uV = 2.5f;     // 2.5 µV per bit (§7.6.1)
    static constexpr float BUS_V_LSB_mV   = 1.25f;    // 1.25 mV per bit (§7.6.2)
    
    // Expected ID values
    static constexpr uint16_t MANUFACTURER_ID = 0x5449;  // "TI"
    static constexpr uint16_t DIE_ID = 0x2260;
    
    INA226() = default;
    
    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the INA226 with individual parameters
     * @param wire TwoWire instance to use
     * @param address I2C address (see INA226Address namespace)
     * @param shuntResistance_ohms Shunt resistor value in ohms
     * @param maxCurrent_A Maximum expected current in amps (for calibration)
     * @return true if device found and initialized
     */
    bool begin(sfx_peripherals::SfxI2cBus& wire, uint8_t address = INA226Address::DEFAULT_ADDR,
               float shuntResistance_ohms = 0.1f, float maxCurrent_A = 3.2f);

    /**
     * @brief Initialize the INA226 from a config struct
     * @param wire TwoWire instance to use
     * @param config Channel configuration (address, shunt, calibration, etc.)
     * @return true if device found and initialized
     */
    bool begin(sfx_peripherals::SfxI2cBus& wire, const INA226Config& config);

    /**
     * @brief Scan I2C bus for all INA226 devices in the 0x40-0x4F range
     * @param wire TwoWire instance to scan
     * @param addresses Output array to receive found addresses
     * @param maxDevices Size of the output array
     * @return Number of INA226 devices found
     */
    static uint8_t scan(sfx_peripherals::SfxI2cBus& wire, uint8_t* addresses, uint8_t maxDevices);

    /**
     * @brief Verify this device is an INA226 (checks MFG_ID and DIE_ID)
     * Called automatically by I2CDevice::begin()
     */
    bool identify();

    /**
     * @brief Reset the device (software reset via CONFIG register)
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
     * @param shuntResistance_ohms Shunt resistor value in ohms
     * @param maxCurrent_A Maximum expected current in amps
     */
    void setCalibration(float shuntResistance_ohms, float maxCurrent_A);

    // ========================================================================
    // Measurements
    // ========================================================================

    /**
     * @brief Update all measurements (call periodically)
     */
    void update();

    /**
     * @brief Get bus voltage in millivolts (INA226 range: 0–36000 mV)
     */
    float busVoltage_mV() const { return _busVoltage_mV; }

    /**
     * @brief Get shunt voltage in microvolts (INA226 range: ±81920 µV)
     */
    float shuntVoltage_uV() const { return _shuntVoltage_uV; }

    /**
     * @brief Get current in milliamps (via calibrated Current Register)
     */
    float current_mA() const { return _current_mA; }

    /**
     * @brief Get power in milliwatts (via Power Register, LSB = 25 × Current_LSB)
     */
    float power_mW() const { return _power_mW; }

    /**
     * @brief Read bus voltage directly in millivolts (without caching)
     */
    float readBusVoltage_mV();

    /**
     * @brief Read shunt voltage directly in microvolts (without caching)
     */
    float readShuntVoltage_uV();

    /**
     * @brief Read current directly in milliamps (without caching)
     */
    float readCurrent_mA();

    /**
     * @brief Read power directly in milliwatts (without caching)
     */
    float readPower_mW();

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
     * @brief True iff the chip reported the canonical TI INA226
     *        manufacturer + die IDs at `begin()` time.
     *
     * `begin()` itself is lenient — it accepts any chip that ACKs at
     * the configured address and configures it as if it were an
     * INA226 (counterfeits and pin-compatible clones generally
     * implement the same calibration / measurement register layout
     * and produce usable readings, just at non-spec accuracy).  This
     * flag lets callers surface the distinction in diagnostics
     * without rejecting the chip outright.
     */
    bool isCanonical() const { return _idMatches; }

    /// The mfg/die IDs read during begin() — handy for boot diag.
    uint16_t bootMfgId() const { return _bootMfgId; }
    uint16_t bootDieId() const { return _bootDieId; }

    // address(), isAvailable(), errorCount(), lastError() inherited from I2CDevice

    /**
     * @brief Get current LSB value (mA per bit, set by calibration)
     */
    float currentLsb_mA() const { return _currentLsb_mA; }

    /**
     * @brief Get power LSB value (mW per bit = 25 x Current_LSB)
     */
    float powerLsb_mW() const { return _powerLsb_mW; }

    /**
     * @brief Get channel index (for multi-monitor identification)
     */
    uint8_t channel() const { return _channel; }

    /**
     * @brief Get shunt resistance in ohms
     */
    float shuntResistance_ohms() const { return _shuntResistance_ohms; }

private:
    void updateConfig();

    uint8_t _channel = 0;
    
    // Configuration
    INA226Averaging _averaging = INA226Averaging::AVG_16;
    INA226ConvTime _busConvTime = INA226ConvTime::CT_1100US;
    INA226ConvTime _shuntConvTime = INA226ConvTime::CT_1100US;
    INA226Mode _mode = INA226Mode::SHUNT_BUS_CONTINUOUS;
    
    // Calibration
    float _shuntResistance_ohms = 0.1f;   // Shunt resistor value
    float _currentLsb_mA = 0.0f;          // mA per bit (INA226 §7.6.3)
    float _powerLsb_mW = 0.0f;            // mW per bit (= 25 x Current_LSB, INA226 §7.6.4)
    
    // Cached measurements (units in suffix)
    float _busVoltage_mV = 0.0f;    // millivolts
    float _shuntVoltage_uV = 0.0f;  // microvolts
    float _current_mA = 0.0f;       // milliamps
    float _power_mW = 0.0f;         // milliwatts

    // Identity readback captured during begin().  `_idMatches` is the
    // outcome of comparing the readbacks against MANUFACTURER_ID /
    // DIE_ID — a counterfeit / pin-compatible clone reads as ACKed +
    // configured but flips this to false so callers can surface a
    // warning at boot.
    uint16_t _bootMfgId = 0;
    uint16_t _bootDieId = 0;
    bool     _idMatches = false;
};

#endif // INA226_H
