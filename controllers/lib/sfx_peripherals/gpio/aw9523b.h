/*
 * AW9523B GPIO Expander Driver — Header
 *
 * Awinic AW9523B — 16-bit I2C GPIO expander with LED constant-current drivers.
 * Each pin can independently operate in GPIO mode or LED mode (hardware PWM).
 *
 * Features:
 *   - 16 GPIO pins in two 8-bit ports (P0_0–P0_7, P1_0–P1_7)
 *   - Per-pin GPIO or LED mode selection
 *   - LED mode: 256-step (8-bit) constant-current PWM dimming (~430 Hz)
 *   - Configurable global max current (IMAX, 3/4, 1/2, 1/4)
 *   - Push-pull or open-drain output for GPIO-mode pins
 *   - Hardware chip ID register (0x10) = 0x23
 *   - Software reset via register 0x7F
 *   - Interrupt output (active-low)
 *
 * Satisfies the GpioExpander concept (gpio_expander.h) PLUS hardware PWM:
 *   - HAS_HW_PWM = true
 *   - setLedMode(pin, bool)
 *   - setLedBrightness(pin, uint8_t)
 *
 * I2C Address:
 *   Base 0x58. Range: 0x58–0x5B (2 address pins: AD0, AD1).
 *   Address = 0x58 | (AD1 << 1) | AD0
 *
 * Pin numbering:
 *   0–7  = Port 0 (P0_0–P0_7)
 *   8–15 = Port 1 (P1_0–P1_7)
 *
 * Usage:
 *   AW9523B expander;
 *   expander.begin(Wire, 0x58);
 *
 *   // GPIO mode (codec control pins)
 *   expander.setPinDirection(8, false);  // P1_0 output
 *   expander.writePin(8, true);          // P1_0 HIGH
 *
 *   // LED mode (MOSFET-driven LEDs)
 *   expander.setLedMode(0, true);        // P0_0 → LED mode
 *   expander.setLedBrightness(0, 128);   // P0_0 → 50% duty PWM
 *
 * Reference: AW9523B datasheet Rev 1.2
 */

#ifndef AW9523B_H
#define AW9523B_H

#include "../power/i2c_device.h"
#include "gpio_expander.h"

// ============================================================================
// AW9523B I2C Address Configuration
// ============================================================================

namespace AW9523BAddress {
    constexpr uint8_t BASE    = 0x58;  // Base address (AD0=GND, AD1=GND)
    constexpr uint8_t MIN     = 0x58;
    constexpr uint8_t MAX     = 0x5B;  // 4 possible addresses

    constexpr uint8_t GND_GND = 0x58;  // AD1=GND, AD0=GND (default)
    constexpr uint8_t GND_VDD = 0x59;  // AD1=GND, AD0=VDD
    constexpr uint8_t VDD_GND = 0x5A;  // AD1=VDD, AD0=GND
    constexpr uint8_t VDD_VDD = 0x5B;  // AD1=VDD, AD0=VDD

    constexpr uint8_t DEFAULT_ADDR = GND_GND;

    /// Check if an address is within the valid range
    constexpr bool isValid(uint8_t addr) {
        return addr >= MIN && addr <= MAX;
    }
}

// ============================================================================
// AW9523B Register Definitions
// ============================================================================

namespace AW9523BReg {
    // ---- Input registers (read-only) ----
    constexpr uint8_t INPUT_PORT_0     = 0x00;  // P0 input state
    constexpr uint8_t INPUT_PORT_1     = 0x01;  // P1 input state

    // ---- Output registers ----
    constexpr uint8_t OUTPUT_PORT_0    = 0x02;  // P0 output (GPIO mode)
    constexpr uint8_t OUTPUT_PORT_1    = 0x03;  // P1 output (GPIO mode)

    // ---- Direction registers (0=output, 1=input — same as PCAL6416A) ----
    constexpr uint8_t CONFIG_PORT_0    = 0x04;  // P0 direction
    constexpr uint8_t CONFIG_PORT_1    = 0x05;  // P1 direction

    // ---- Interrupt control ----
    constexpr uint8_t INT_PORT_0       = 0x06;  // P0 interrupt enable (0=enable, 1=disable)
    constexpr uint8_t INT_PORT_1       = 0x07;  // P1 interrupt enable

    // ---- Chip ID ----
    constexpr uint8_t CHIP_ID         = 0x10;  // Read: 0x23

    // ---- Global Control ----
    constexpr uint8_t GCR             = 0x11;  // Global Control Register
                                                // Bit 4: P0 port mode (0=open-drain, 1=push-pull)
                                                // Bits 1:0: IMAX range (00=IMAX, 01=3/4, 10=1/2, 11=1/4)

    // ---- LED Mode Switch (0=LED mode, 1=GPIO mode — per bit) ----
    constexpr uint8_t LED_MODE_P0     = 0x12;  // P0 LED/GPIO mode select (reset: 0x00 = all LED)
    constexpr uint8_t LED_MODE_P1     = 0x13;  // P1 LED/GPIO mode select (reset: 0x00 = all LED)

    // ---- Individual LED brightness (DIM) registers (0x00=off, 0xFF=max) ----
    // P1 dimensions (DIM0–DIM7): registers 0x20–0x27
    constexpr uint8_t DIM_P1_BASE     = 0x20;  // DIM0 (P1_0) through DIM7 (P1_7)

    // P0 dimensions (DIM8–DIM11, DIM4–DIM7): registers 0x24–0x2B
    // Note: P0 pin mapping to DIM registers is non-sequential:
    //   P0_0 → DIM4  (0x24)    P0_4 → DIM8  (0x28)
    //   P0_1 → DIM5  (0x25)    P0_5 → DIM9  (0x29)
    //   P0_2 → DIM6  (0x26)    P0_6 → DIM10 (0x2A)
    //   P0_3 → DIM7  (0x27)    P0_7 → DIM11 (0x2B)
    constexpr uint8_t DIM_P0_BASE     = 0x24;  // DIM4 (P0_0) through DIM11 (P0_7)

    // ---- Software Reset ----
    constexpr uint8_t SOFT_RESET      = 0x7F;  // Write 0x00 to reset
}

// ---- Chip ID expected value ----
constexpr uint8_t AW9523B_CHIP_ID_VALUE = 0x23;

// ---- Global Control Register bits ----
constexpr uint8_t AW9523B_GCR_P0_PUSH_PULL = 0x10;  // Bit 4: P0 push-pull mode

// ---- IMAX current range (GCR bits 1:0) ----
enum class AW9523B_CurrentRange : uint8_t {
    IMAX_37mA    = 0x00,  // Maximum current (37mA per channel)
    IMAX_3_4     = 0x01,  // 3/4 IMAX (~27.75mA)
    IMAX_1_2     = 0x02,  // 1/2 IMAX (~18.5mA)
    IMAX_1_4     = 0x03   // 1/4 IMAX (~9.25mA)
};

// ============================================================================
// AW9523B Class
// ============================================================================

class AW9523B : public I2CDevice {
public:
    /// Number of ports (Port 0 and Port 1)
    static constexpr uint8_t NUM_PORTS = 2;
    /// Total number of GPIO pins
    static constexpr uint8_t NUM_PINS = 16;
    /// This expander has hardware PWM capability
    static constexpr bool HAS_HW_PWM = true;

    AW9523B() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the AW9523B on the I2C bus
     * @param wire TwoWire instance to use
     * @param address I2C address (see AW9523BAddress namespace)
     * @return true if device found and initialized
     */
    bool begin(TwoWire& wire, uint8_t address = AW9523BAddress::DEFAULT_ADDR);

    /**
     * @brief Verify chip ID register (0x10 should return 0x23)
     */
    bool identify() override;

    /**
     * @brief Software reset — all registers return to defaults
     *
     * After reset: all pins in LED mode, all directions = input,
     * all DIM values = 0, GCR = 0x00 (P0 open-drain, IMAX).
     */
    void reset();

    // ========================================================================
    // Port-Level I/O (GpioExpander concept)
    // ========================================================================

    /**
     * @brief Set port direction: 1=input, 0=output (per bit)
     * @param port Port number (0 or 1)
     * @param directionMask Bitmask (1=input, 0=output)
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
     * @brief Write output values to a port (GPIO mode pins only)
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
    // Individual Pin I/O (GpioExpander concept)
    // ========================================================================

    /**
     * @brief Set a single pin as input or output
     * @param pin Pin number (0-15; 0-7 = Port 0, 8-15 = Port 1)
     * @param isInput true=input, false=output
     * @return true on success
     */
    bool setPinDirection(uint8_t pin, bool isInput);

    /**
     * @brief Write a single output pin (GPIO mode)
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
    // LED Mode — Hardware PWM (AW9523B-specific, HAS_HW_PWM = true)
    // ========================================================================

    /**
     * @brief Switch a pin between GPIO mode and LED (hardware PWM) mode
     * @param pin Pin number (0-15)
     * @param ledMode true = LED mode (hardware PWM), false = GPIO mode
     * @return true on success
     *
     * In LED mode, the pin outputs a ~430 Hz PWM signal with duty cycle
     * controlled by setLedBrightness(). The pin must also be set as output.
     * When driving external MOSFETs, duty cycle controls gate drive.
     */
    bool setLedMode(uint8_t pin, bool ledMode);

    /**
     * @brief Set LED brightness for a pin in LED mode
     * @param pin Pin number (0-15)
     * @param brightness 0 = off (0% duty), 255 = full on (100% duty)
     * @return true on success
     *
     * This directly sets the DIM register for the pin. Only effective
     * when the pin is in LED mode (setLedMode(pin, true)).
     */
    bool setLedBrightness(uint8_t pin, uint8_t brightness);

    /**
     * @brief Set LED mode for an entire port at once
     * @param port Port number (0 or 1)
     * @param modeMask Bitmask: 0=LED mode, 1=GPIO mode (yes, inverted!)
     * @return true on success
     *
     * Note: AW9523B register convention is inverted — 0=LED, 1=GPIO.
     * This method uses the raw register value for efficiency.
     */
    bool setPortLedMode(uint8_t port, uint8_t modeMask);

    // ========================================================================
    // Global Configuration
    // ========================================================================

    /**
     * @brief Set the global maximum LED current range
     * @param range Current range (IMAX_37mA, 3/4, 1/2, 1/4)
     * @return true on success
     */
    bool setCurrentRange(AW9523B_CurrentRange range);

    /**
     * @brief Set Port 0 output mode (push-pull or open-drain)
     * @param pushPull true = push-pull, false = open-drain (default)
     * @return true on success
     *
     * Note: Only Port 0 supports push-pull/open-drain selection.
     * Port 1 is always push-pull.
     */
    bool setP0PushPull(bool pushPull);

    // ========================================================================
    // Interrupt Configuration
    // ========================================================================

    /**
     * @brief Set interrupt enable mask for a port
     * @param port Port number (0 or 1)
     * @param mask Bitmask (0=interrupt enabled, 1=disabled)
     * @return true on success
     */
    bool setInterruptMask(uint8_t port, uint8_t mask);

    // ========================================================================
    // Diagnostics
    // ========================================================================

    /**
     * @brief Read the chip ID register
     * @return Chip ID value (should be 0x23)
     */
    uint8_t readChipId();

    // address(), isAvailable(), errorCount(), lastError() inherited from I2CDevice

private:
    /**
     * @brief Get the DIM register address for a given pin number
     * @param pin Pin number (0-15)
     * @return DIM register address, or 0 if invalid
     *
     * The AW9523B DIM register layout is non-sequential:
     *   P1_0..P1_7 → 0x20..0x27 (sequential)
     *   P0_0..P0_7 → 0x24..0x2B (offset by 4 from P0 base perspective)
     */
    uint8_t _dimRegister(uint8_t pin) const;
};

#endif // AW9523B_H
