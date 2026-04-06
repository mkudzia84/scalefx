/*
 * AW9523B GPIO Expander Driver — Implementation
 *
 * See aw9523b.h for documentation.
 * Reference: AW9523B datasheet Rev 1.2
 */

#include "aw9523b.h"
#include <platform/sfx_platform.h>

// ============================================================================
// Initialization
// ============================================================================

bool AW9523B::begin(TwoWire& wire, uint8_t address) {
    if (!I2CDevice::begin(wire, address)) {
        return false;
    }

    // Software reset to known state
    reset();

    // Verify chip identity
    if (!identify()) {
        return false;
    }

    // After reset, all pins default to:
    //   - LED mode (LED_MODE_P0/P1 = 0x00)
    //   - Input direction (CONFIG_PORT = 0xFF)
    //   - DIM values = 0x00
    //   - GCR = 0x00 (P0 open-drain, IMAX)

    // Switch all pins to GPIO mode by default (same as PCAL6416A behavior).
    // Callers who want LED mode will explicitly call setLedMode().
    writeRegister8(AW9523BReg::LED_MODE_P0, 0xFF);  // All P0 = GPIO mode
    writeRegister8(AW9523BReg::LED_MODE_P1, 0xFF);  // All P1 = GPIO mode

    // Enable push-pull on Port 0 by default (matches PCAL6416A push-pull behavior)
    writeRegister8(AW9523BReg::GCR, AW9523B_GCR_P0_PUSH_PULL);

    return true;
}

bool AW9523B::identify() {
    uint8_t chipId = readChipId();
    return chipId == AW9523B_CHIP_ID_VALUE;
}

void AW9523B::reset() {
    // Writing 0x00 to register 0x7F triggers software reset
    writeRegister8(AW9523BReg::SOFT_RESET, 0x00);

    // Datasheet specifies ~1µs reset time; give a small margin
    SFX_DELAY_US(50);
}

// ============================================================================
// Port-Level I/O
// ============================================================================

bool AW9523B::setPortDirection(uint8_t port, uint8_t directionMask) {
    if (port >= NUM_PORTS) return false;
    uint8_t reg = (port == 0) ? AW9523BReg::CONFIG_PORT_0 : AW9523BReg::CONFIG_PORT_1;
    return writeRegister8(reg, directionMask);
}

uint8_t AW9523B::getPortDirection(uint8_t port) {
    if (port >= NUM_PORTS) return 0xFF;
    uint8_t reg = (port == 0) ? AW9523BReg::CONFIG_PORT_0 : AW9523BReg::CONFIG_PORT_1;
    return readRegister8(reg);
}

bool AW9523B::writePort(uint8_t port, uint8_t value) {
    if (port >= NUM_PORTS) return false;
    uint8_t reg = (port == 0) ? AW9523BReg::OUTPUT_PORT_0 : AW9523BReg::OUTPUT_PORT_1;
    return writeRegister8(reg, value);
}

uint8_t AW9523B::readPort(uint8_t port) {
    if (port >= NUM_PORTS) return 0x00;
    uint8_t reg = (port == 0) ? AW9523BReg::INPUT_PORT_0 : AW9523BReg::INPUT_PORT_1;
    return readRegister8(reg);
}

uint16_t AW9523B::readAll() {
    uint8_t lo = readPort(0);   // Port 0 → low byte
    uint8_t hi = readPort(1);   // Port 1 → high byte
    return (uint16_t(hi) << 8) | lo;
}

bool AW9523B::writeAll(uint16_t value) {
    bool ok = writePort(0, value & 0xFF);         // Port 0 = low byte
    ok &= writePort(1, (value >> 8) & 0xFF);      // Port 1 = high byte
    return ok;
}

// ============================================================================
// Individual Pin I/O
// ============================================================================

bool AW9523B::setPinDirection(uint8_t pin, bool isInput) {
    if (pin >= NUM_PINS) return false;
    uint8_t port = pin / 8;
    uint8_t bit  = pin % 8;

    uint8_t current = getPortDirection(port);
    if (isInput) {
        current |= (1 << bit);   // Set bit → input
    } else {
        current &= ~(1 << bit);  // Clear bit → output
    }
    return setPortDirection(port, current);
}

bool AW9523B::writePin(uint8_t pin, bool high) {
    if (pin >= NUM_PINS) return false;
    uint8_t port = pin / 8;
    uint8_t bit  = pin % 8;
    uint8_t reg  = (port == 0) ? AW9523BReg::OUTPUT_PORT_0 : AW9523BReg::OUTPUT_PORT_1;

    uint8_t current = readRegister8(reg);
    if (high) {
        current |= (1 << bit);
    } else {
        current &= ~(1 << bit);
    }
    return writeRegister8(reg, current);
}

bool AW9523B::readPin(uint8_t pin) {
    if (pin >= NUM_PINS) return false;
    uint8_t port = pin / 8;
    uint8_t bit  = pin % 8;
    return (readPort(port) >> bit) & 0x01;
}

// ============================================================================
// LED Mode — Hardware PWM
// ============================================================================

bool AW9523B::setLedMode(uint8_t pin, bool ledMode) {
    if (pin >= NUM_PINS) return false;
    uint8_t port = pin / 8;
    uint8_t bit  = pin % 8;
    uint8_t reg  = (port == 0) ? AW9523BReg::LED_MODE_P0 : AW9523BReg::LED_MODE_P1;

    // Register convention is inverted: 0 = LED mode, 1 = GPIO mode
    uint8_t current = readRegister8(reg);
    if (ledMode) {
        current &= ~(1 << bit);  // Clear bit → LED mode
    } else {
        current |= (1 << bit);   // Set bit → GPIO mode
    }
    return writeRegister8(reg, current);
}

bool AW9523B::setLedBrightness(uint8_t pin, uint8_t brightness) {
    if (pin >= NUM_PINS) return false;
    uint8_t reg = _dimRegister(pin);
    if (reg == 0) return false;
    return writeRegister8(reg, brightness);
}

bool AW9523B::setPortLedMode(uint8_t port, uint8_t modeMask) {
    if (port >= NUM_PORTS) return false;
    uint8_t reg = (port == 0) ? AW9523BReg::LED_MODE_P0 : AW9523BReg::LED_MODE_P1;
    return writeRegister8(reg, modeMask);
}

// ============================================================================
// Global Configuration
// ============================================================================

bool AW9523B::setCurrentRange(AW9523B_CurrentRange range) {
    uint8_t gcr = readRegister8(AW9523BReg::GCR);
    gcr = (gcr & 0xFC) | (static_cast<uint8_t>(range) & 0x03);
    return writeRegister8(AW9523BReg::GCR, gcr);
}

bool AW9523B::setP0PushPull(bool pushPull) {
    uint8_t gcr = readRegister8(AW9523BReg::GCR);
    if (pushPull) {
        gcr |= AW9523B_GCR_P0_PUSH_PULL;
    } else {
        gcr &= ~AW9523B_GCR_P0_PUSH_PULL;
    }
    return writeRegister8(AW9523BReg::GCR, gcr);
}

// ============================================================================
// Interrupt Configuration
// ============================================================================

bool AW9523B::setInterruptMask(uint8_t port, uint8_t mask) {
    if (port >= NUM_PORTS) return false;
    uint8_t reg = (port == 0) ? AW9523BReg::INT_PORT_0 : AW9523BReg::INT_PORT_1;
    return writeRegister8(reg, mask);
}

// ============================================================================
// Diagnostics
// ============================================================================

uint8_t AW9523B::readChipId() {
    return readRegister8(AW9523BReg::CHIP_ID);
}

// ============================================================================
// Internal Helpers
// ============================================================================

uint8_t AW9523B::_dimRegister(uint8_t pin) const {
    // AW9523B DIM register mapping (non-sequential):
    //
    // Port 1 (P1_0..P1_7) → DIM0..DIM7 (0x20..0x27) — sequential
    // Port 0 (P0_0..P0_7) → DIM4..DIM11 (0x24..0x2B) — offset +4 from 0x20
    //
    // Mapping table:
    //   P0_0 → 0x24 (DIM4)     P1_0 → 0x20 (DIM0)
    //   P0_1 → 0x25 (DIM5)     P1_1 → 0x21 (DIM1)
    //   P0_2 → 0x26 (DIM6)     P1_2 → 0x22 (DIM2)
    //   P0_3 → 0x27 (DIM7)     P1_3 → 0x23 (DIM3)
    //   P0_4 → 0x28 (DIM8)     P1_4 → 0x24 (DIM4)
    //   P0_5 → 0x29 (DIM9)     P1_5 → 0x25 (DIM5)
    //   P0_6 → 0x2A (DIM10)    P1_6 → 0x26 (DIM6)
    //   P0_7 → 0x2B (DIM11)    P1_7 → 0x27 (DIM7)

    if (pin >= NUM_PINS) return 0;

    if (pin < 8) {
        // Port 0: P0_n → 0x24 + n
        return AW9523BReg::DIM_P0_BASE + pin;
    } else {
        // Port 1: P1_n → 0x20 + (pin - 8)
        return AW9523BReg::DIM_P1_BASE + (pin - 8);
    }
}
