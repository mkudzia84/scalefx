/*
 * I2C Device Base Class - Header
 * 
 * Common abstraction for I2C peripheral devices on a shared bus.
 * 
 * Provides:
 *   - Safe register read/write operations (8-bit and 16-bit)
 *   - Bus probing and scanning
 *   - Error tracking with I2C error codes
 *   - Virtual identify() for device-specific ID verification
 * 
 * Subclass this for each I2C device type and override identify()
 * to verify the device identity (WHO_AM_I, manufacturer ID, etc.).
 * 
 * Usage:
 *   class MySensor : public I2CDevice {
 *   public:
 *       bool identify() override {
 *           return readRegister8(WHO_AM_I_REG) == EXPECTED_ID;
 *       }
 *       bool begin(TwoWire& wire, uint8_t address) {
 *           if (!I2CDevice::begin(wire, address)) return false;
 *           // Device-specific init...
 *           return true;
 *       }
 *   };
 */

#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <Arduino.h>
#include <Wire.h>

// ============================================================================
// I2C Error Codes (Wire endTransmission return values)
// ============================================================================

enum class I2CError : uint8_t {
    OK            = 0,  // Success
    DATA_TOO_LONG = 1,  // Data too long for transmit buffer
    NACK_ADDRESS  = 2,  // NACK on transmit of address
    NACK_DATA     = 3,  // NACK on transmit of data
    OTHER         = 4,  // Other error
    TIMEOUT       = 5   // Timeout
};

// ============================================================================
// I2C Device Base Class
// ============================================================================

class I2CDevice {
public:
    I2CDevice() = default;
    virtual ~I2CDevice() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the device on the I2C bus
     * 
     * Probes the address, then calls identify() to verify the device.
     * Subclasses should call this from their own begin(), then do
     * device-specific setup.
     * 
     * @param wire TwoWire instance (e.g. Wire, Wire1)
     * @param address 7-bit I2C slave address
     * @return true if device found and identified
     */
    virtual bool begin(TwoWire& wire, uint8_t address);

    /**
     * @brief Verify device identity (override in subclass)
     * 
     * Called by begin() after the device ACKs its address.
     * Override to check WHO_AM_I, manufacturer ID, die ID, etc.
     * Default returns true (accepts any device that ACKs).
     */
    virtual bool identify();

    // ========================================================================
    // Status
    // ========================================================================

    /** @brief Check if device was successfully initialized */
    bool isAvailable() const { return _available; }

    /** @brief Get I2C slave address */
    uint8_t address() const { return _address; }

    /** @brief Get cumulative I2C error count since begin() */
    uint32_t errorCount() const { return _errorCount; }

    /** @brief Get most recent I2C error code */
    I2CError lastError() const { return _lastError; }

    // ========================================================================
    // Static Bus Utilities
    // ========================================================================

    /**
     * @brief Probe a single I2C address for any device (ACK check)
     * @param wire TwoWire instance
     * @param address 7-bit I2C address to probe
     * @return true if a device ACKs the address
     */
    static bool probe(TwoWire& wire, uint8_t address);

    /**
     * @brief Scan an I2C address range for any devices (ACK probe only)
     * 
     * This is a generic scan — it finds anything that ACKs, regardless of
     * device type. For device-specific scanning with ID verification,
     * use the device subclass's scan() method (e.g. INA226::scan()).
     * 
     * @param wire TwoWire instance
     * @param minAddr Start of range (inclusive)
     * @param maxAddr End of range (inclusive)
     * @param addresses Output array to receive found addresses
     * @param maxDevices Size of the output array
     * @return Number of devices found
     */
    static uint8_t scan(TwoWire& wire, uint8_t minAddr, uint8_t maxAddr,
                        uint8_t* addresses, uint8_t maxDevices);

protected:
    // ========================================================================
    // Register I/O (for subclasses)
    // ========================================================================

    /**
     * @brief Write an 8-bit value to a register
     * @return true on success
     */
    bool writeRegister8(uint8_t reg, uint8_t value);

    /**
     * @brief Write a 16-bit value to a register (MSB first — I2C convention)
     * @return true on success
     */
    bool writeRegister16(uint8_t reg, uint16_t value);

    /**
     * @brief Read an 8-bit value from a register
     * @param reg Register address
     * @param ok Optional output flag (true on success)
     * @return Register value, or 0 on error
     */
    uint8_t readRegister8(uint8_t reg, bool* ok = nullptr);

    /**
     * @brief Read a 16-bit value from a register (MSB first — I2C convention)
     * @param reg Register address
     * @param ok Optional output flag (true on success)
     * @return Register value, or 0 on error
     */
    uint16_t readRegister16(uint8_t reg, bool* ok = nullptr);

    /**
     * @brief Write raw bytes to a register
     * @return true on success
     */
    bool writeBytes(uint8_t reg, const uint8_t* data, uint8_t len);

    /**
     * @brief Read raw bytes from a register
     * @return Number of bytes actually read
     */
    uint8_t readBytes(uint8_t reg, uint8_t* data, uint8_t len);

    // ========================================================================
    // Protected Members
    // ========================================================================

    TwoWire* _wire = nullptr;
    uint8_t _address = 0;
    bool _available = false;
    uint32_t _errorCount = 0;
    I2CError _lastError = I2CError::OK;
};

#endif // I2C_DEVICE_H
