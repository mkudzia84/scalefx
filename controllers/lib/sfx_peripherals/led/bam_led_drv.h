/*
 * BAM LED Driver — Software Binary Angle Modulation over I2C GPIO Expander
 *
 * Provides per-pin software PWM for I2C GPIO expanders that lack hardware PWM
 * (e.g., PCAL6416A, PCA9555, TCA6416A). Brightness is achieved by decomposing
 * each channel's duty into bit-planes and writing them in timed succession.
 *
 * ExpanderBamT satisfies the GPIO provider concept (see gpio/gpio_expander.h),
 * so LedControlT<ExpanderBamT<T>> works directly — no intermediate driver class.
 *
 * Expander policy requirements (duck-typed):
 *   - bool isAvailable() const
 *   - uint8_t getPortDirection(uint8_t port)
 *   - bool setPortDirection(uint8_t port, uint8_t mask)
 *   - uint8_t readPort(uint8_t port)
 *   - bool writePort(uint8_t port, uint8_t value)
 *
 * GPIO provider interface (satisfied by ExpanderBamT):
 *   - bool isAvailable() const
 *   - bool setPinDirection(uint8_t pin, bool isInput)
 *   - bool setLedMode(uint8_t pin, bool mode)
 *   - bool setLedBrightness(uint8_t pin, uint8_t duty)  // 0-255, mapped via BamLut
 *   - bool writePin(uint8_t pin, bool high)
 *   - static constexpr bool HAS_HW_PWM = false
 *
 * BAM algorithm:
 *   A 5-bit (32 levels) BAM cycle is divided into time slices weighted by
 *   bit significance:  bit0=1T, bit1=2T, bit2=4T, bit3=8T, bit4=16T
 *   Total cycle = 31T.  With T ≈ 323 µs → cycle ≈ 10 ms → ~100 Hz refresh.
 *   Each tick, the engine writes one bit-plane (all channels' Nth bit) as a
 *   single port write — very efficient over I2C (one transaction per tick).
 *
 * Usage:
 *   PCAL6416A expander;
 *   expander.begin(Wire, 0x20);
 *
 *   ExpanderBamT<PCAL6416A> bamEngine;
 *   bamEngine.begin(&expander, 0);                // BAM on Port 0
 *
 *   LedControlT<ExpanderBamT<PCAL6416A>> ch0, ch1;
 *   ch0.begin(&bamEngine, 0);                     // Pin 0
 *   ch1.begin(&bamEngine, 1, false, true);        // Pin 1, PWM mode
 *
 *   ch0.setBrightness(50);
 *   ch1.on();
 *
 *   // In loop() — MUST be called frequently (~3 kHz ideal):
 *   bamEngine.update();
 *
 * Brightness mapping:
 *   setLedBrightness(pin, 0-255) is mapped to 5-bit BAM levels (0-31) using
 *   a non-linear perceptual curve for visually uniform brightness steps.
 *
 * Reference: en.wikipedia.org/wiki/Bit_angle_modulation
 */

#ifndef BAM_LED_DRV_H
#define BAM_LED_DRV_H

#include <Arduino.h>
#include "../gpio/gpio_expander.h"
#include "../gpio/pcal6416a.h"

// ============================================================================
// BAM Configuration
// ============================================================================

/// Number of BAM bits (5 = 32 brightness levels, good balance of flicker vs I2C load)
constexpr uint8_t BAM_BITS = 5;

/// Total BAM weights per cycle: 1+2+4+8+16 = 31
constexpr uint8_t BAM_CYCLE_TICKS = (1 << BAM_BITS) - 1;

/// Maximum pins per port
constexpr uint8_t BAM_MAX_PINS = 8;

// ============================================================================
// Perceptual brightness LUT: 8-bit (0-255) → 5-bit BAM (0-31)
// ============================================================================

namespace BamLut {

/**
 * @brief Convert 0-255 PWM to 0-31 BAM duty with perceptual (gamma 2.2) curve.
 *
 * The lookup table provides visually uniform brightness steps. Using a
 * simple linear map (value >> 3) would make low-brightness steps invisible
 * and high-brightness steps too aggressive.
 */
inline uint8_t pwmToBam(uint8_t pwm) {
    // Gamma-corrected LUT: pwm(0-255) → bam(0-31)
    // Generated with: round(31 * (i/255)^2.2) for i in 0..255
    // Stored in PROGMEM for flash-only footprint (no RAM cost).
    static const uint8_t PROGMEM lut[256] = {
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,
         2,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,
         3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,
         5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  7,  7,  7,  7,
         7,  7,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9, 10, 10, 10,
        10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14,
        14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 18, 18,
        18, 18, 19, 19, 19, 20, 20, 20, 21, 21, 21, 22, 22, 22, 23, 23,
        23, 24, 24, 24, 25, 25, 25, 26, 26, 26, 27, 27, 27, 28, 28, 29,
        29, 29, 30, 30, 30, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
        31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
        31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31
    };
    return pgm_read_byte(&lut[pwm]);
}

} // namespace BamLut

// ============================================================================
// ExpanderBamT — BAM Engine + GPIO Provider (one per expander port)
// ============================================================================

/**
 * @brief Binary Angle Modulation engine for one port of an I2C GPIO expander.
 *
 * @tparam TExpander I2C GPIO expander type (e.g., PCAL6416A). Must provide:
 *   isAvailable(), getPortDirection(), setPortDirection(), readPort(), writePort()
 *
 * Manages the BAM cycle for up to 8 channels on a single port. All
 * channels are updated with a single I2C port-write per time slice.
 *
 * Satisfies the GPIO provider concept so it can be used directly as:
 *   LedControlT<ExpanderBamT<PCAL6416A>>   // single LED
 *   LedManager<N, ExpanderBamT<PCAL6416A>>  // multi-channel
 *
 * Must call update() frequently from the main loop.
 */
template <typename TExpander>
class ExpanderBamT {
public:
    /// Software BAM — no hardware PWM on the underlying expander
    static constexpr bool HAS_HW_PWM = false;

    ExpanderBamT() = default;

    /**
     * @brief Initialize the BAM engine
     * @param expander Pointer to initialized expander instance
     * @param port Port number (0 or 1)
     * @param pinMask Bitmask of pins to drive (1=BAM controlled). Default 0x3F = pins 0-5.
     * @return true on success
     */
    bool begin(TExpander* expander, uint8_t port, uint8_t pinMask = 0x3F) {
        if (!expander || !expander->isAvailable() || port > 1) return false;

        _expander = expander;
        _port = port;
        _pinMask = pinMask;

        // Configure managed pins as outputs (0 = output in the direction register)
        uint8_t currentDir = _expander->getPortDirection(port);
        currentDir &= ~pinMask;  // Clear bits = set as output
        _expander->setPortDirection(port, currentDir);

        // Start with all off
        for (uint8_t i = 0; i < BAM_MAX_PINS; i++) _duty[i] = 0;
        _expander->writePort(port, _lastPortValue & ~_pinMask);  // Clear managed pins

        _initialized = true;
        _currentBit = 0;
        _ticksRemaining = 1;  // First bit-plane starts immediately
        _lastUpdate_us = micros();

        return true;
    }

    // ========================================================================
    // GPIO Provider Concept — used by LedControlT / LedManager
    // ========================================================================

    /// BAM engine is available when initialized
    bool isAvailable() const { return _initialized; }

    /**
     * @brief Set pin direction. No-op — pins are configured as outputs in begin().
     * @return true if pin is in the managed mask
     */
    bool setPinDirection(uint8_t pin, bool /*isInput*/) {
        return pin < BAM_MAX_PINS && (_pinMask & (1 << pin));
    }

    /**
     * @brief Enable LED mode for a pin. No-op — BAM handles brightness for all pins.
     * @return true if pin is in the managed mask
     */
    bool setLedMode(uint8_t pin, bool /*mode*/) {
        return pin < BAM_MAX_PINS && (_pinMask & (1 << pin));
    }

    /**
     * @brief Set LED brightness (0-255) via perceptual LUT → BAM duty.
     *
     * Maps the 8-bit brightness value through the gamma-corrected BamLut
     * to a 5-bit BAM level, then sets the per-pin duty cycle.
     *
     * @param pin Pin index within the port (0-7)
     * @param brightness 0-255 brightness value (gamma-mapped internally)
     * @return true on success
     */
    bool setLedBrightness(uint8_t pin, uint8_t brightness) {
        if (pin >= BAM_MAX_PINS || !(_pinMask & (1 << pin))) return false;
        _duty[pin] = BamLut::pwmToBam(brightness);
        return true;
    }

    /**
     * @brief Digital write — full on or full off via BAM duty.
     * @param pin Pin index within the port (0-7)
     * @param high true = full brightness (31/31), false = off (0/31)
     * @return true on success
     */
    bool writePin(uint8_t pin, bool high) {
        if (pin >= BAM_MAX_PINS || !(_pinMask & (1 << pin))) return false;
        _duty[pin] = high ? BAM_CYCLE_TICKS : 0;
        return true;
    }

    // ========================================================================
    // BAM Engine — Direct API
    // ========================================================================

    /**
     * @brief Set raw BAM duty for a pin (0-31). Use setLedBrightness() for
     *        perceptual brightness; this is for direct BAM-level control.
     * @param pinIndex Pin index within the port (0-7)
     * @param duty BAM duty level (0 = off, 31 = full on)
     */
    void setDuty(uint8_t pinIndex, uint8_t duty) {
        if (pinIndex >= BAM_MAX_PINS) return;
        if (duty > BAM_CYCLE_TICKS) duty = BAM_CYCLE_TICKS;
        _duty[pinIndex] = duty;
    }

    /**
     * @brief Get current BAM duty for a pin
     */
    uint8_t getDuty(uint8_t pinIndex) const {
        if (pinIndex >= BAM_MAX_PINS) return 0;
        return _duty[pinIndex];
    }

    /**
     * @brief Tick the BAM engine. Call from loop() as frequently as possible.
     *
     * The base tick period is automatically calculated from BAM_BITS to maintain
     * approximately 100 Hz refresh. Each bit-plane is held for 2^N base periods.
     * One I2C port-write happens when a new bit-plane begins.
     */
    void update() {
        if (!_initialized) return;

        uint32_t now_us = micros();
        uint32_t elapsed_us = now_us - _lastUpdate_us;

        // Base tick period for ~100 Hz cycle
        // Cycle = BAM_CYCLE_TICKS * BASE_TICK_US ≈ 10 ms for 100 Hz
        constexpr uint32_t BASE_TICK_US = 10000 / BAM_CYCLE_TICKS;  // ~323 µs for 5-bit

        if (elapsed_us < BASE_TICK_US) return;  // Not time yet

        _lastUpdate_us = now_us;
        _ticksRemaining--;

        if (_ticksRemaining == 0) {
            // Time to advance to next bit-plane
            _writeBitPlane(_currentBit);

            // Set duration for this bit and advance
            _ticksRemaining = (1 << _currentBit);
            _currentBit++;
            if (_currentBit >= BAM_BITS) {
                _currentBit = 0;  // Wrap to start of cycle
            }
        }
    }

    // ========================================================================
    // Queries
    // ========================================================================

    bool isInitialized() const { return _initialized; }
    uint8_t port() const       { return _port; }
    uint8_t pinMask() const    { return _pinMask; }

private:
    /**
     * @brief Write one bit-plane: for each managed pin, set output to the value
     *        of the Nth bit of that pin's duty.
     */
    void _writeBitPlane(uint8_t bitIndex) {
        uint8_t portValue = 0;

        for (uint8_t i = 0; i < BAM_MAX_PINS; i++) {
            if (!(_pinMask & (1 << i))) continue;

            // Set this pin high if the bit is set in the duty value
            if (_duty[i] & (1 << bitIndex)) {
                portValue |= (1 << i);
            }
        }

        // Read current port to preserve non-managed pins, then merge
        uint8_t current = _expander->readPort(_port);
        current &= ~_pinMask;    // Clear managed pin bits
        current |= portValue;    // Set BAM values
        _lastPortValue = current;
        _expander->writePort(_port, current);
    }

    TExpander* _expander = nullptr;
    uint8_t _port = 0;
    uint8_t _pinMask = 0;
    bool _initialized = false;

    uint8_t _duty[BAM_MAX_PINS] = {};  // Per-pin BAM duty (0-31)
    uint8_t _currentBit = 0;            // Current bit-plane being displayed
    uint8_t _ticksRemaining = 0;        // Base ticks left in current bit-plane

    uint32_t _lastUpdate_us = 0;
    uint8_t _lastPortValue = 0;          // Cached port output for merge
};

#endif // BAM_LED_DRV_H
