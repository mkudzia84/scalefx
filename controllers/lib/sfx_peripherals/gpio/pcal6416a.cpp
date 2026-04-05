/*
 * PCAL6416A GPIO Expander Driver - Implementation
 *
 * NXP PCAL6416AHF — 16-bit I2C GPIO expander.
 * See pcal6416a.h for API documentation.
 *
 * Reference: NXP PCAL6416A datasheet (Rev. 7, 2019-05-15)
 */

#include "pcal6416a.h"

// ============================================================================
// Initialization
// ============================================================================

bool PCAL6416A::begin(TwoWire& wire, uint8_t address) {
    if (!I2CDevice::begin(wire, address)) return false;

    // Device found and identified — cache port states
    // No additional init needed; device powers up with all pins as inputs
    return true;
}

bool PCAL6416A::identify() {
    // PCAL6416A has no dedicated device ID register.
    // The address probe (done by I2CDevice::begin) is the primary check.
    // As a secondary verification, confirm we can read the config registers.
    bool ok = false;
    readRegister8(PCAL6416AReg::CONFIG_PORT_0, &ok);
    return ok;
}

void PCAL6416A::reset() {
    if (!isAvailable()) return;

    // Reset all writable registers to power-on defaults (§7.1)
    writeRegister8(PCAL6416AReg::OUTPUT_PORT_0,  0xFF);
    writeRegister8(PCAL6416AReg::OUTPUT_PORT_1,  0xFF);
    writeRegister8(PCAL6416AReg::POLARITY_INV_0, 0x00);
    writeRegister8(PCAL6416AReg::POLARITY_INV_1, 0x00);
    writeRegister8(PCAL6416AReg::CONFIG_PORT_0,  0xFF);  // All inputs
    writeRegister8(PCAL6416AReg::CONFIG_PORT_1,  0xFF);  // All inputs

    // Extended registers
    writeRegister8(PCAL6416AReg::INPUT_LATCH_0,  0x00);
    writeRegister8(PCAL6416AReg::INPUT_LATCH_1,  0x00);
    writeRegister8(PCAL6416AReg::PUPD_ENABLE_0,  0x00);  // Pull resistors disabled
    writeRegister8(PCAL6416AReg::PUPD_ENABLE_1,  0x00);
    writeRegister8(PCAL6416AReg::PUPD_SELECT_0,  0xFF);  // Pull-up selected (default)
    writeRegister8(PCAL6416AReg::PUPD_SELECT_1,  0xFF);
    writeRegister8(PCAL6416AReg::IRQ_MASK_0,     0xFF);  // All interrupts masked
    writeRegister8(PCAL6416AReg::IRQ_MASK_1,     0xFF);
    writeRegister8(PCAL6416AReg::OUTPUT_PORT_CFG, 0x00); // Push-pull
}

// ============================================================================
// Port-Level I/O
// ============================================================================

bool PCAL6416A::setPortDirection(uint8_t port, uint8_t directionMask) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t reg = (port == 0) ? PCAL6416AReg::CONFIG_PORT_0
                               : PCAL6416AReg::CONFIG_PORT_1;
    return writeRegister8(reg, directionMask);
}

uint8_t PCAL6416A::getPortDirection(uint8_t port) {
    if (port > 1 || !isAvailable()) return 0xFF;
    uint8_t reg = (port == 0) ? PCAL6416AReg::CONFIG_PORT_0
                               : PCAL6416AReg::CONFIG_PORT_1;
    return readRegister8(reg);
}

bool PCAL6416A::writePort(uint8_t port, uint8_t value) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t reg = (port == 0) ? PCAL6416AReg::OUTPUT_PORT_0
                               : PCAL6416AReg::OUTPUT_PORT_1;
    return writeRegister8(reg, value);
}

uint8_t PCAL6416A::readPort(uint8_t port) {
    if (port > 1 || !isAvailable()) return 0x00;
    uint8_t reg = (port == 0) ? PCAL6416AReg::INPUT_PORT_0
                               : PCAL6416AReg::INPUT_PORT_1;
    return readRegister8(reg);
}

uint16_t PCAL6416A::readAll() {
    uint8_t lo = readPort(0);
    uint8_t hi = readPort(1);
    return (uint16_t)hi << 8 | lo;
}

bool PCAL6416A::writeAll(uint16_t value) {
    bool ok0 = writePort(0, value & 0xFF);
    bool ok1 = writePort(1, (value >> 8) & 0xFF);
    return ok0 && ok1;
}

// ============================================================================
// Individual Pin I/O
// ============================================================================

bool PCAL6416A::setPinDirection(uint8_t pin, bool isInput) {
    if (pin >= NUM_PINS) return false;
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;
    uint8_t current = getPortDirection(port);
    if (isInput)
        current |= (1 << bit);
    else
        current &= ~(1 << bit);
    return setPortDirection(port, current);
}

bool PCAL6416A::writePin(uint8_t pin, bool high) {
    if (pin >= NUM_PINS || !isAvailable()) return false;
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;
    uint8_t reg = (port == 0) ? PCAL6416AReg::OUTPUT_PORT_0
                               : PCAL6416AReg::OUTPUT_PORT_1;
    uint8_t current = readRegister8(reg);
    if (high)
        current |= (1 << bit);
    else
        current &= ~(1 << bit);
    return writeRegister8(reg, current);
}

bool PCAL6416A::readPin(uint8_t pin) {
    if (pin >= NUM_PINS) return false;
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;
    return (readPort(port) >> bit) & 0x01;
}

// ============================================================================
// Pull-Up / Pull-Down Configuration
// ============================================================================

bool PCAL6416A::setPullEnable(uint8_t port, uint8_t enableMask) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t reg = (port == 0) ? PCAL6416AReg::PUPD_ENABLE_0
                               : PCAL6416AReg::PUPD_ENABLE_1;
    return writeRegister8(reg, enableMask);
}

bool PCAL6416A::setPullSelect(uint8_t port, uint8_t selectMask) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t reg = (port == 0) ? PCAL6416AReg::PUPD_SELECT_0
                               : PCAL6416AReg::PUPD_SELECT_1;
    return writeRegister8(reg, selectMask);
}

// ============================================================================
// Polarity Inversion
// ============================================================================

bool PCAL6416A::setPolarityInversion(uint8_t port, uint8_t invertMask) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t reg = (port == 0) ? PCAL6416AReg::POLARITY_INV_0
                               : PCAL6416AReg::POLARITY_INV_1;
    return writeRegister8(reg, invertMask);
}

// ============================================================================
// Output Configuration
// ============================================================================

bool PCAL6416A::setOutputMode(uint8_t port, bool openDrain) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t current = readRegister8(PCAL6416AReg::OUTPUT_PORT_CFG);
    uint8_t bit = port;  // bit 0 = Port 0, bit 1 = Port 1
    if (openDrain)
        current |= (1 << bit);
    else
        current &= ~(1 << bit);
    return writeRegister8(PCAL6416AReg::OUTPUT_PORT_CFG, current);
}

// ============================================================================
// Interrupt Configuration
// ============================================================================

bool PCAL6416A::setInterruptMask(uint8_t port, uint8_t mask) {
    if (port > 1 || !isAvailable()) return false;
    uint8_t reg = (port == 0) ? PCAL6416AReg::IRQ_MASK_0
                               : PCAL6416AReg::IRQ_MASK_1;
    return writeRegister8(reg, mask);
}

uint8_t PCAL6416A::getInterruptStatus(uint8_t port) {
    if (port > 1 || !isAvailable()) return 0x00;
    uint8_t reg = (port == 0) ? PCAL6416AReg::IRQ_STATUS_0
                               : PCAL6416AReg::IRQ_STATUS_1;
    return readRegister8(reg);
}
