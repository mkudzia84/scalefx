/*
 * NativeGpio — Thin GPIO abstraction for native MCU pins
 *
 * Wraps the MCU's own GPIO and analogWrite into the same pin-level
 * interface that I²C PWM chips expose.  This lets templates like
 * LedControlT<T> and LedManager<N, T> work identically whether the
 * outputs sit on the MCU directly or on an external PCA9685 — just
 * change the template argument.
 *
 * Satisfies the PwmOutput concept (pwm/pwm_output.h):
 *   - bool isAvailable() const
 *   - bool setPinDirection(uint8_t pin, bool isInput)
 *   - bool writePin(uint8_t pin, bool high)
 *   - bool setLedBrightness(uint8_t pin, uint8_t brightness)  // via analogWrite
 *
 * Also exposes `readPin(pin)` for callers that need raw GPIO input —
 * not part of the PwmOutput contract, but available for board-level
 * code that wants the convenience.
 *
 * Provides a thread-safe singleton via instance() for convenient use
 * with LedControlT<NativeGpio> and LedManager<N, NativeGpio>.
 *
 * Platform support:
 *   - RP2040 / RP2350 (Arduino-Pico): analogWrite on every pin
 *   - ESP32-S3 (Arduino-ESP32):       LEDC-backed analogWrite
 *
 * Usage (singleton — preferred):
 *   NativeGpio& gpio = NativeGpio::instance();
 *   gpio.setPinDirection(13, false);     // Output
 *   gpio.writePin(13, true);             // HIGH
 *   gpio.setLedBrightness(13, 128);      // 50% PWM
 *
 * With LedControlT (pin convenience uses singleton automatically):
 *   LedControl led;                      // = LedControlT<NativeGpio>
 *   led.begin(13, false, true);          // → begin(&NativeGpio::instance(), 13, ...)
 */

#ifndef NATIVE_GPIO_H
#define NATIVE_GPIO_H

#include <Arduino.h>
#include "../pwm/pwm_output.h"

// Platform-conditional max pin count (informational, not enforced)
#if SFX_PLATFORM_ESP32
    constexpr uint8_t NATIVE_GPIO_MAX_PINS = 48;   // ESP32-S3 has GPIO 0-48
#else
    constexpr uint8_t NATIVE_GPIO_MAX_PINS = 30;   // RP2040/RP2350 has GP0-GP29
#endif

// ============================================================================
// NativeGpio — MCU GPIO with analogWrite PWM
// ============================================================================

/**
 * @brief Thin wrapper around native MCU GPIO pins.
 *
 * Provides the same pin-level surface as an I²C PWM chip so templates
 * can be parameterised on any PwmOutput provider.  Zero-overhead —
 * each method is a direct call to the Arduino GPIO API.
 *
 * Use NativeGpio::instance() for the shared singleton. LedControlT<NativeGpio>
 * and LedManager<N, NativeGpio> use the singleton automatically for their
 * convenience begin(pin, ...) overloads.
 */
class NativeGpio {
public:
    /// Thread-safe singleton (C++11 static local)
    static NativeGpio& instance() {
        static NativeGpio inst;
        return inst;
    }

    /// Informational pin count (not a hard limit — GPIO validity depends on board)
    static constexpr uint8_t NUM_PINS = NATIVE_GPIO_MAX_PINS;

    /// Native GPIO always supports PWM via analogWrite
    static constexpr bool HAS_HW_PWM = true;

    NativeGpio() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief No-op — native GPIO is always present.
     * @return Always true
     */
    bool begin() { return true; }

    /**
     * @brief Native GPIO is always available — no probing needed.
     */
    bool isAvailable() const { return true; }

    // ========================================================================
    // Pin-Level I/O — PwmOutput concept surface
    // ========================================================================

    /**
     * @brief Set a pin as input or output
     * @param pin MCU GPIO pin number
     * @param isInput true = INPUT, false = OUTPUT
     * @return true (always succeeds for valid pins; no hardware error detection)
     */
    bool setPinDirection(uint8_t pin, bool isInput) {
        pinMode(pin, isInput ? INPUT : OUTPUT);
        return true;
    }

    /**
     * @brief Write a digital value to an output pin
     * @param pin MCU GPIO pin number
     * @param high true = HIGH, false = LOW
     * @return true
     */
    bool writePin(uint8_t pin, bool high) {
        digitalWrite(pin, high ? HIGH : LOW);
        return true;
    }

    /**
     * @brief Set PWM duty cycle on a pin via analogWrite.
     *
     * On RP2040/RP2350: uses hardware PWM slice (all pins support it).
     * On ESP32-S3: uses LEDC peripheral (auto-assigned channel).
     *
     * @param pin MCU GPIO pin number
     * @param brightness 0 = off, 255 = full on
     * @return true
     */
    bool setLedBrightness(uint8_t pin, uint8_t brightness) {
        analogWrite(pin, brightness);
        return true;
    }

    // ========================================================================
    // Extra — GPIO input (not part of the PwmOutput contract)
    // ========================================================================

    /**
     * @brief Read a digital value from an input pin
     * @param pin MCU GPIO pin number
     * @return true if HIGH, false if LOW
     */
    bool readPin(uint8_t pin) {
        return digitalRead(pin) == HIGH;
    }
};

#endif // NATIVE_GPIO_H
