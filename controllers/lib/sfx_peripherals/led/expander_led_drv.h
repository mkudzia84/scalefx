/*
 * Expander LED Driver — Software BAM over I2C GPIO Expander
 *
 * Provides per-pin PWM on an I2C GPIO expander using Binary Angle
 * Modulation (BAM). Since these expanders have no hardware PWM, BAM achieves
 * brightness control by decomposing each channel's duty into bit-planes
 * and writing them in timed succession.
 *
 * The BAM engine and per-pin driver are templatized on the expander type.
 * Swapping to a different I2C GPIO expander (e.g., PCA9555, TCA6416A) only
 * requires a new `using` alias — no code changes to BAM logic or LED control.
 *
 * Expander policy requirements (duck-typed):
 *   - bool isAvailable() const
 *   - uint8_t getPortDirection(uint8_t port)
 *   - bool setPortDirection(uint8_t port, uint8_t mask)
 *   - uint8_t readPort(uint8_t port)
 *   - bool writePort(uint8_t port, uint8_t value)
 *
 * Architecture:
 *   ExpanderBamT<TExpander>       — Shared BAM engine (one per expander port)
 *   ExpanderLedDriverT<TExpander> — Per-channel driver bound to one pin
 *   ExpanderBam / ExpanderLedDriver — Aliases for PCAL6416A (current HW)
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
 *   ExpanderBam bamEngine;                // = ExpanderBamT<PCAL6416A>
 *   bamEngine.begin(&expander, 0);        // BAM on Port 0
 *
 *   ExpanderLedDriver ch0, ch1;           // = ExpanderLedDriverT<PCAL6416A>
 *   ch0.begin(&bamEngine, 0);             // Pin 0 on port
 *   ch1.begin(&bamEngine, 1);             // Pin 1 on port
 *
 *   ch0.writePwm(128);  // ~50% duty
 *   ch1.writePwm(255);  // Full on
 *
 *   // In loop() — MUST be called frequently (~3 kHz ideal):
 *   bamEngine.update();
 *
 * To use a different expander chip:
 *   using MyBam    = ExpanderBamT<MyNewExpander>;
 *   using MyDriver = ExpanderLedDriverT<MyNewExpander>;
 *
 * Brightness mapping:
 *   writePwm(0-255) is mapped to 5-bit BAM levels (0-31) using a non-linear
 *   perceptual curve for visually uniform brightness steps.
 *
 * Reference: en.wikipedia.org/wiki/Bit_angle_modulation
 */

#ifndef EXPANDER_LED_DRV_H
#define EXPANDER_LED_DRV_H

#include <Arduino.h>
#include "../gpio/gpio_expander.h"
#include "../gpio/pcal6416a.h"
#include "../gpio/aw9523b.h"

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
// ExpanderBamT — Shared BAM Engine (one per expander port)
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
 * Must call update() frequently from the main loop.
 */
template <typename TExpander>
class ExpanderBamT {
public:
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

    /**
     * @brief Set BAM duty for a pin (0-31)
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
// ExpanderLedDriverT — Per-pin driver using BAM engine
// ============================================================================

/**
 * @brief LED driver for a single pin on an ExpanderBamT engine.
 *
 * @tparam TExpander I2C GPIO expander type (must match the BAM engine's type)
 *
 * Each ExpanderLedDriverT instance binds to one pin on a shared BAM engine.
 * Multiple drivers share the same engine (and therefore the same I2C writes).
 *
 * Matches the LedDriver policy interface expected by LedControlT<TDriver>.
 */
template <typename TExpander>
class ExpanderLedDriverT {
public:
    ExpanderLedDriverT() = default;

    /**
     * @brief Bind to a pin on an ExpanderBamT engine
     * @param engine Pointer to initialized ExpanderBamT<TExpander>
     * @param pinIndex Pin index within the BAM port (0-7)
     * @return true on success
     */
    bool begin(ExpanderBamT<TExpander>* engine, uint8_t pinIndex) {
        if (!engine || !engine->isInitialized()) return false;
        if (pinIndex >= BAM_MAX_PINS) return false;
        if (!(engine->pinMask() & (1 << pinIndex))) return false;  // Pin not managed by engine

        _engine = engine;
        _pinIndex = pinIndex;
        _attached = true;

        // Start off
        _engine->setDuty(_pinIndex, 0);
        return true;
    }

    /**
     * @brief Release the pin
     */
    void end() {
        if (_attached && _engine) {
            _engine->setDuty(_pinIndex, 0);
        }
        _engine = nullptr;
        _pinIndex = 0;
        _attached = false;
    }

    bool isAttached() const { return _attached; }
    int  pin() const        { return _attached ? (int)_pinIndex : -1; }

    /**
     * @brief Digital on/off (active-high; caller inverts for active-low)
     */
    void writeDigital(bool on) {
        if (!_attached) return;
        _engine->setDuty(_pinIndex, on ? BAM_CYCLE_TICKS : 0);
    }

    /**
     * @brief PWM duty 0-255, mapped through perceptual LUT to BAM levels
     */
    void writePwm(uint8_t duty) {
        if (!_attached) return;
        _engine->setDuty(_pinIndex, BamLut::pwmToBam(duty));
    }

    /// BAM engine supports PWM
    bool supportsPwm() const { return true; }

    /**
     * @brief No-op — the shared ExpanderBam::update() drives the timing.
     * Caller MUST call ExpanderBam::update() in the main loop.
     */
    void update() {}

private:
    ExpanderBamT<TExpander>* _engine = nullptr;
    uint8_t _pinIndex = 0;
    bool _attached = false;
};

// ============================================================================
// Default aliases — current hardware: PCAL6416A
// To swap expander, change PCAL6416A to the new type here.
// ============================================================================

/// BAM engine for PCAL6416A (current board)
using ExpanderBam = ExpanderBamT<PCAL6416A>;

/// Per-pin LED driver for PCAL6416A (current board)
using ExpanderLedDriver = ExpanderLedDriverT<PCAL6416A>;

/// BAM engine for AW9523B (for GPIO-mode pins needing software BAM — rare,
/// since AW9523B has hardware PWM; prefer HwPwmLedDriverT<AW9523B> instead)
using AW9523BBam = ExpanderBamT<AW9523B>;

/// Per-pin software BAM driver for AW9523B (prefer HwPwmLedDriverT<AW9523B>)
using AW9523BBamDriver = ExpanderLedDriverT<AW9523B>;

#endif // EXPANDER_LED_DRV_H
