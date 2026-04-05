/*
 * PCAL6416A GPIO Expander Driver - Header
 *
 * NXP PCAL6416AHF — 16-bit I2C GPIO expander with agile I/O.
 * Extended PCA9555 with additional features (drive strength, pull-up/down,
 * interrupt masking, output port configuration).
 *
 * Features:
 *   - 16 GPIO pins in two 8-bit ports (Port 0, Port 1)
 *   - Configurable input/output per pin
 *   - Configurable pull-up/pull-down resistors
 *   - Polarity inversion on input pins
 *   - Output drive strength control (0.25x or 1x)
 *   - Interrupt masking per pin
 *   - Agile I/O: push-pull or open-drain output modes
 *
 * I2C Address:
 *   Base address 0x20 (A0=GND, A1=GND). Range: 0x20-0x27.
 *   Address = 0x20 | (A2 << 2) | (A1 << 1) | A0
 *
 * Usage:
 *   PCAL6416A expander;
 *   expander.begin(Wire, 0x20);
 *   expander.setPortDirection(0, 0xFF);  // Port 0 all inputs
 *   expander.setPortDirection(1, 0x00);  // Port 1 all outputs
 *   expander.writePort(1, 0x55);         // Set Port 1 output
 *   uint8_t inputs = expander.readPort(0);
 *
 * Reference: NXP PCAL6416A datasheet (Rev. 7, 2019-05-15)
 */

#ifndef PCAL6416A_H
#define PCAL6416A_H

#include "../power/i2c_device.h"

// ============================================================================
// PCAL6416A I2C Address Configuration
// ============================================================================

namespace PCAL6416AAddress {
    constexpr uint8_t BASE    = 0x20;  // Base address (A0=GND, A1=GND, A2=GND)
    constexpr uint8_t MIN     = 0x20;
    constexpr uint8_t MAX     = 0x27;  // 8 possible addresses

    // Common configurations (A2 is often GND on PCAL6416AHF packages)
    constexpr uint8_t GND_GND = 0x20;  // A1=GND, A0=GND (default)
    constexpr uint8_t GND_VDD = 0x21;  // A1=GND, A0=VDD
    constexpr uint8_t VDD_GND = 0x22;  // A1=VDD, A0=GND
    constexpr uint8_t VDD_VDD = 0x23;  // A1=VDD, A0=VDD

    constexpr uint8_t DEFAULT_ADDR = GND_GND;

    /// Check if an address is within the valid range
    constexpr bool isValid(uint8_t addr) {
        return addr >= MIN && addr <= MAX;
    }
}

// ============================================================================
// PCAL6416A Register Definitions (§7.1)
// ============================================================================

namespace PCAL6416AReg {
    // ---- PCA9555-compatible registers ----
    constexpr uint8_t INPUT_PORT_0      = 0x00;  // Input Port 0 (R/O)
    constexpr uint8_t INPUT_PORT_1      = 0x01;  // Input Port 1 (R/O)
    constexpr uint8_t OUTPUT_PORT_0     = 0x02;  // Output Port 0 (R/W, reset: 0xFF)
    constexpr uint8_t OUTPUT_PORT_1     = 0x03;  // Output Port 1 (R/W, reset: 0xFF)
    constexpr uint8_t POLARITY_INV_0    = 0x04;  // Polarity Inversion 0 (R/W, reset: 0x00)
    constexpr uint8_t POLARITY_INV_1    = 0x05;  // Polarity Inversion 1 (R/W, reset: 0x00)
    constexpr uint8_t CONFIG_PORT_0     = 0x06;  // Configuration Port 0 (R/W, reset: 0xFF = all inputs)
    constexpr uint8_t CONFIG_PORT_1     = 0x07;  // Configuration Port 1 (R/W, reset: 0xFF = all inputs)

    // ---- PCAL6416A extended registers (§7.1.2) ----
    constexpr uint8_t DRIVE_STRENGTH_0_L = 0x40;  // Output drive strength Port 0 [7:0] (R/W)
    constexpr uint8_t DRIVE_STRENGTH_0_H = 0x41;  // Output drive strength Port 0 [15:8] (R/W)
    constexpr uint8_t DRIVE_STRENGTH_1_L = 0x42;  // Output drive strength Port 1 [7:0] (R/W)
    constexpr uint8_t DRIVE_STRENGTH_1_H = 0x43;  // Output drive strength Port 1 [15:8] (R/W)
    constexpr uint8_t INPUT_LATCH_0     = 0x44;  // Input latch Port 0 (R/W, reset: 0x00)
    constexpr uint8_t INPUT_LATCH_1     = 0x45;  // Input latch Port 1 (R/W, reset: 0x00)
    constexpr uint8_t PUPD_ENABLE_0     = 0x46;  // Pull-up/pull-down enable Port 0 (R/W, reset: 0x00)
    constexpr uint8_t PUPD_ENABLE_1     = 0x47;  // Pull-up/pull-down enable Port 1 (R/W, reset: 0x00)
    constexpr uint8_t PUPD_SELECT_0     = 0x48;  // Pull-up/pull-down select Port 0 (R/W, reset: 0xFF)
    constexpr uint8_t PUPD_SELECT_1     = 0x49;  // Pull-up/pull-down select Port 1 (R/W, reset: 0xFF)
    constexpr uint8_t IRQ_MASK_0        = 0x4A;  // Interrupt mask Port 0 (R/W, reset: 0xFF)
    constexpr uint8_t IRQ_MASK_1        = 0x4B;  // Interrupt mask Port 1 (R/W, reset: 0xFF)
    constexpr uint8_t IRQ_STATUS_0      = 0x4C;  // Interrupt status Port 0 (R/O)
    constexpr uint8_t IRQ_STATUS_1      = 0x4D;  // Interrupt status Port 1 (R/O)
    constexpr uint8_t OUTPUT_PORT_CFG   = 0x4F;  // Output port configuration (R/W, reset: 0x00)
                                                   // 0=push-pull (default), 1=open-drain per port
}

// ============================================================================
// PCAL6416A Class
// ============================================================================

class PCAL6416A : public I2CDevice {
public:
    /// Number of ports (Port 0 and Port 1)
    static constexpr uint8_t NUM_PORTS = 2;
    /// Total number of GPIO pins
    static constexpr uint8_t NUM_PINS = 16;

    PCAL6416A() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the PCAL6416A on the I2C bus
     * @param wire TwoWire instance to use
     * @param address I2C address (see PCAL6416AAddress namespace)
     * @return true if device found and initialized (all pins default to inputs)
     */
    bool begin(TwoWire& wire, uint8_t address = PCAL6416AAddress::DEFAULT_ADDR);

    /**
     * @brief Verify the device responds correctly.
     *
     * PCAL6416A has no dedicated ID register. Verification is done by reading
     * the configuration register (should be 0xFF at reset = all inputs).
     * Since the device may have been previously configured, we also accept
     * any value (the address probe in I2CDevice::begin() is the primary check).
     */
    bool identify() override;

    /**
     * @brief Reset all registers to power-on defaults.
     * Software reset by writing default values to all writable registers.
     */
    void reset();

    // ========================================================================
    // Port-Level I/O (8 pins per port)
    // ========================================================================

    /**
     * @brief Set port direction: 1=input, 0=output (per bit)
     * @param port Port number (0 or 1)
     * @param directionMask Bitmask (1=input, 0=output, default all inputs)
     * @return true on success
     */
    bool setPortDirection(uint8_t port, uint8_t directionMask);

    /**
     * @brief Read the current direction configuration of a port
     * @param port Port number (0 or 1)
     * @return Direction bitmask (1=input, 0=output), or 0xFF on error
     */
    uint8_t getPortDirection(uint8_t port);

    /**
     * @brief Write output values to a port
     * @param port Port number (0 or 1)
     * @param value Output value bitmask
     * @return true on success
     */
    bool writePort(uint8_t port, uint8_t value);

    /**
     * @brief Read input values from a port
     * @param port Port number (0 or 1)
     * @return Input value bitmask, or 0x00 on error
     */
    uint8_t readPort(uint8_t port);

    /**
     * @brief Read all 16 pins as a single 16-bit value
     * @return Port 0 in low byte, Port 1 in high byte
     */
    uint16_t readAll();

    /**
     * @brief Write all 16 output pins as a single 16-bit value
     * @param value Port 0 in low byte, Port 1 in high byte
     * @return true on success
     */
    bool writeAll(uint16_t value);

    // ========================================================================
    // Individual Pin I/O
    // ========================================================================

    /**
     * @brief Set a single pin as input or output
     * @param pin Pin number (0-15; 0-7 = Port 0, 8-15 = Port 1)
     * @param isInput true=input, false=output
     * @return true on success
     */
    bool setPinDirection(uint8_t pin, bool isInput);

    /**
     * @brief Write a single output pin
     * @param pin Pin number (0-15)
     * @param high true=high, false=low
     * @return true on success
     */
    bool writePin(uint8_t pin, bool high);

    /**
     * @brief Read a single input pin
     * @param pin Pin number (0-15)
     * @return Pin state (true=high, false=low)
     */
    bool readPin(uint8_t pin);

    // ========================================================================
    // Pull-Up / Pull-Down Configuration
    // ========================================================================

    /**
     * @brief Enable/disable pull resistors for a port
     * @param port Port number (0 or 1)
     * @param enableMask Bitmask (1=enabled, 0=disabled)
     * @return true on success
     */
    bool setPullEnable(uint8_t port, uint8_t enableMask);

    /**
     * @brief Select pull-up or pull-down for a port
     * @param port Port number (0 or 1)
     * @param selectMask Bitmask (1=pull-up, 0=pull-down)
     * @return true on success
     */
    bool setPullSelect(uint8_t port, uint8_t selectMask);

    // ========================================================================
    // Polarity Inversion
    // ========================================================================

    /**
     * @brief Set polarity inversion for input pins
     * @param port Port number (0 or 1)
     * @param invertMask Bitmask (1=inverted, 0=normal)
     * @return true on success
     */
    bool setPolarityInversion(uint8_t port, uint8_t invertMask);

    // ========================================================================
    // Output Configuration
    // ========================================================================

    /**
     * @brief Set output port mode: push-pull or open-drain
     * @param port Port number (0 or 1)
     * @param openDrain true=open-drain, false=push-pull (default)
     * @return true on success
     */
    bool setOutputMode(uint8_t port, bool openDrain);

    // ========================================================================
    // Interrupt Configuration
    // ========================================================================

    /**
     * @brief Set interrupt mask for a port
     * @param port Port number (0 or 1)
     * @param mask Bitmask (0=enabled, 1=masked/disabled)
     * @return true on success
     */
    bool setInterruptMask(uint8_t port, uint8_t mask);

    /**
     * @brief Read interrupt status for a port (clears on read)
     * @param port Port number (0 or 1)
     * @return Interrupt status bitmask (1=interrupt pending)
     */
    uint8_t getInterruptStatus(uint8_t port);

    // address(), isAvailable(), errorCount(), lastError() inherited from I2CDevice
};

#endif // PCAL6416A_H
