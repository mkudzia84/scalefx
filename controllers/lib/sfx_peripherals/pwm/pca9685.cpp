/*
 * PCA9685 — 16-channel 12-bit I²C PWM driver (implementation)
 *
 * See pca9685.h for the API surface and a full description of the
 * conservative init flow + the rationale for clamping duty to [1, 4095]
 * (avoiding the ON==OFF undefined-behaviour case and the FULL_ON /
 * FULL_OFF glitch transitions).
 */

#include "pca9685.h"

// ============================================================================
// Static helpers
// ============================================================================

uint8_t PCA9685::computePrescale(uint16_t freqHz) {
    if (freqHz < 24)   freqHz = 24;
    if (freqHz > 1526) freqHz = 1526;

    // Datasheet §7.3.5: prescale = round(25 MHz / (4096 × freq)) − 1.
    //   add half a divisor (2048 × freq) to the numerator for rounding.
    uint32_t value = (INTERNAL_OSC_HZ + 2048UL * freqHz)
                     / (4096UL * freqHz);
    if (value > 0) value--;
    if (value < 3)   value = 3;
    if (value > 255) value = 255;
    return (uint8_t)value;
}

bool PCA9685::broadcastReset(TwoWire& wire) {
    // Address byte = 0x00 (general call), data byte = 0x06 (SWRST opcode).
    // Any PCA9685 on the bus with ALLCALL enabled (POR default) will reset.
    wire.beginTransmission(PCA9685Address::GENERAL_CALL);
    wire.write(PCA9685Reg::SWRST_BYTE);
    return wire.endTransmission() == 0;
}

// ============================================================================
// Initialization
// ============================================================================

bool PCA9685::begin(TwoWire& wire, uint8_t address, uint16_t freqHz) {
    // The I2CDevice base class probes the address and calls identify().
    if (!I2CDevice::begin(wire, address)) return false;

    // Step 1: broadcast SWRST. Belt-and-braces — also recovers a
    // chip that's silent at its address. Harmless on a healthy chip.
    broadcastReset(*_wire);
    delay(2);

    // Step 2: park in SLEEP (PRESCALE only writable while SLEEP=1).
    writeRegister8(PCA9685Reg::MODE1,
                   PCA9685Reg::MODE1_SLEEP | PCA9685Reg::MODE1_ALLCALL);
    delay(1);

    // Step 3: PRESCALE.
    const uint8_t prescaleValue = computePrescale(freqHz);
    writeRegister8(PCA9685Reg::PRESCALE, prescaleValue);
    _frequencyHz = (uint16_t)(INTERNAL_OSC_HZ / (4096UL * ((uint32_t)prescaleValue + 1)));

    // Step 4: MODE2 — push-pull OUTDRV (recommended for MOSFET-gate drive).
    writeRegister8(PCA9685Reg::MODE2, PCA9685Reg::MODE2_OUTDRV);

    // Step 5: wake — clear SLEEP, enable AI for burst writes.
    writeRegister8(PCA9685Reg::MODE1,
                   PCA9685Reg::MODE1_AI | PCA9685Reg::MODE1_ALLCALL);
    delayMicroseconds(500);   // §7.3.1.1 oscillator stabilisation
    writeRegister8(PCA9685Reg::MODE1,
                   PCA9685Reg::MODE1_AI | PCA9685Reg::MODE1_ALLCALL |
                   PCA9685Reg::MODE1_RESTART);
    delay(1);

    // Step 6: park ALL channels at OFF (FULL_OFF flag — safe because no
    // outputs are running yet, no transition glitch possible).
    writeBurst4(PCA9685Reg::ALL_LED_ON_L, 0x00, 0x00, 0x00, PCA9685Reg::LED_FULL);

    return true;
}

bool PCA9685::identify() {
    // PCA9685 has no chip-ID register, so we rely on the probe ACK that
    // I2CDevice::begin() already did. Read MODE1 back to confirm we can
    // talk to the chip — any sensible value indicates a real device.
    bool ok = false;
    (void)readRegister8(PCA9685Reg::MODE1, &ok);
    return ok;
}

void PCA9685::reset() {
    if (!_available || !_wire) return;
    broadcastReset(*_wire);
    delay(2);
    _frequencyHz = 0;  // POR PRESCALE = 0x1E (~200 Hz), but caller should
                      // re-configure via setFrequency() rather than rely
                      // on the POR default.
}

// ============================================================================
// PWM frequency / mode
// ============================================================================

uint16_t PCA9685::setFrequency(uint16_t freqHz) {
    if (!_available) return 0;

    const uint8_t prescaleValue = computePrescale(freqHz);

    // PRESCALE is only writable while MODE1.SLEEP = 1. Read MODE1 so we
    // can restore the AI / ALLCALL bits afterwards.
    bool readOk = false;
    const uint8_t mode1Old = readRegister8(PCA9685Reg::MODE1, &readOk);
    const uint8_t mode1Keep = readOk ? (mode1Old & ~PCA9685Reg::MODE1_RESTART)
                                     : (PCA9685Reg::MODE1_AI |
                                        PCA9685Reg::MODE1_ALLCALL);

    writeRegister8(PCA9685Reg::MODE1, mode1Keep | PCA9685Reg::MODE1_SLEEP);
    delay(1);
    writeRegister8(PCA9685Reg::PRESCALE, prescaleValue);
    writeRegister8(PCA9685Reg::MODE1, mode1Keep & ~PCA9685Reg::MODE1_SLEEP);
    delayMicroseconds(500);
    writeRegister8(PCA9685Reg::MODE1,
                   (mode1Keep & ~PCA9685Reg::MODE1_SLEEP) | PCA9685Reg::MODE1_RESTART);

    _frequencyHz = (uint16_t)(INTERNAL_OSC_HZ / (4096UL * ((uint32_t)prescaleValue + 1)));
    return _frequencyHz;
}

bool PCA9685::sleep() {
    if (!_available) return false;
    bool readOk = false;
    const uint8_t mode1Old = readRegister8(PCA9685Reg::MODE1, &readOk);
    const uint8_t base = readOk ? (mode1Old & ~PCA9685Reg::MODE1_RESTART)
                                : (PCA9685Reg::MODE1_AI |
                                   PCA9685Reg::MODE1_ALLCALL);
    return writeRegister8(PCA9685Reg::MODE1, base | PCA9685Reg::MODE1_SLEEP);
}

bool PCA9685::wake() {
    if (!_available) return false;
    bool readOk = false;
    const uint8_t mode1Old = readRegister8(PCA9685Reg::MODE1, &readOk);
    const uint8_t base = readOk ? (mode1Old & ~(PCA9685Reg::MODE1_SLEEP |
                                                PCA9685Reg::MODE1_RESTART))
                                : (PCA9685Reg::MODE1_AI |
                                   PCA9685Reg::MODE1_ALLCALL);
    if (!writeRegister8(PCA9685Reg::MODE1, base)) return false;
    delayMicroseconds(500);  // §7.3.1.1 oscillator stabilisation
    return writeRegister8(PCA9685Reg::MODE1, base | PCA9685Reg::MODE1_RESTART);
}

// ============================================================================
// Channel writes
// ============================================================================

bool PCA9685::writeBurst4(uint8_t firstReg,
                          uint8_t on_l, uint8_t on_h,
                          uint8_t off_l, uint8_t off_h) {
    if (!_wire || !_available) return false;

    _wire->beginTransmission(_address);
    _wire->write(firstReg);
    _wire->write(on_l);
    _wire->write(on_h);
    _wire->write(off_l);
    _wire->write(off_h);
    const uint8_t err = _wire->endTransmission();
    if (err != 0) {
        _errorCount++;
        _lastError = static_cast<I2CError>(err);
        return false;
    }
    return true;
}

bool PCA9685::writeChannelRaw(uint8_t firstReg, uint16_t duty12) {
    if (duty12 < 1)        duty12 = 1;
    if (duty12 > DUTY_MAX) duty12 = DUTY_MAX;

    const uint8_t off_l = (uint8_t)(duty12 & 0xFF);
    const uint8_t off_h = (uint8_t)((duty12 >> 8) & 0x0F);
    return writeBurst4(firstReg, 0x00, 0x00, off_l, off_h);
}

bool PCA9685::setChannel(uint8_t channel, uint16_t duty12) {
    if (channel >= NUM_PINS) return false;
    const uint8_t firstReg = (uint8_t)(PCA9685Reg::LED0_ON_L + 4 * channel);
    return writeChannelRaw(firstReg, duty12);
}

bool PCA9685::setAllChannels(uint16_t duty12) {
    return writeChannelRaw(PCA9685Reg::ALL_LED_ON_L, duty12);
}

// ============================================================================
// PwmOutput concept adapter
// ============================================================================

bool PCA9685::setPinDirection(uint8_t /*pin*/, bool /*isInput*/) {
    // PCA9685 channels are always outputs. Return availability so the
    // caller can detect "chip absent" via the return value.
    return _available;
}

bool PCA9685::setLedBrightness(uint8_t pin, uint8_t brightness) {
    if (pin >= NUM_PINS) return false;

    // Map 8-bit (0..255) → 12-bit (0..4095) with proper rounding.
    // brightness = 0 collapses to duty=1 (sub-visible single tick) per
    // the clamp in writeChannelRaw(). brightness = 255 maps to duty=4095.
    uint16_t duty12;
    if (brightness == 0) {
        duty12 = 0;  // writeChannelRaw clamps to 1
    } else {
        // (brightness * 4095 + 127) / 255 — exact mapping with rounding.
        duty12 = (uint16_t)(((uint32_t)brightness * (uint32_t)DUTY_MAX + 127UL) / 255UL);
    }
    return writeChannelRaw((uint8_t)(PCA9685Reg::LED0_ON_L + 4 * pin), duty12);
}

bool PCA9685::writePin(uint8_t pin, bool high) {
    return setLedBrightness(pin, high ? 255 : 0);
}
