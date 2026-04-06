/*
 * NativeGpio — Thin GPIO abstraction for native MCU pins
 *
 * Wraps the MCU's own GPIO and analogWrite into the same pin-level
 * interface that I2C GPIO expanders expose.  This lets templates like
 * HwPwmLedDriverT<T> and LedControlT<T> work identically with native
 * pins or I2C expanders — just change the template argument.
 *
 * Satisfies the GpioExpander pin-level concept (gpio_expander.h):
 *   - HAS_HW_PWM = true  (via analogWrite / ledc on ESP32)
 *   - setPinDirection, writePin, readPin
 *   - setLedMode (no-op), setLedBrightness (analogWrite)
 *
 * Does NOT provide port-level bulk I/O (setPortDirection, writePort,
 * readPort) because native GPIO pins are not grouped into I2C-style
 * ports.  This means ExpanderBamT<NativeGpio> won't compile — which is
 * intentional: native GPIO has real PWM and doesn't need software BAM.
 *
 * Platform support:
 *   - RP2040 / RP2350 (Arduino-Pico): analogWrite on every pin
 *   - ESP32-S3 (Arduino-ESP32):       LEDC-backed analogWrite
 *
 * Usage:
 *   NativeGpio gpio;
 *   gpio.begin();
 *
 *   gpio.setPinDirection(13, false);     // Output
 *   gpio.writePin(13, true);            // HIGH
 *
 *   gpio.setLedBrightness(13, 128);     // 50% PWM via analogWrite
 *
 * With HwPwmLedDriverT:
 *   NativeGpio gpio;
 *   gpio.begin();
 *
 *   HwPwmLedDriverT<NativeGpio> drv;
 *   drv.begin(&gpio, 13);
 *   drv.writePwm(200);                  // → analogWrite(13, 200)
 */

#ifndef NATIVE_GPIO_H
#define NATIVE_GPIO_H

#include <Arduino.h>
#include "gpio_expander.h"

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
 * Provides the same pin-level interface as I2C GPIO expanders (PCAL6416A,
 * AW9523B) so that templates can be parameterized on any GPIO provider.
 *
 * Unlike I2C expanders, this class has no I2C bus, no address, and no
 * port-level bulk I/O. It's essentially zero-overhead — each method is
 * a direct call to the Arduino GPIO API.
 */
class NativeGpio {
public:
    /// Informational pin count (not a hard limit — GPIO validity depends on board)
    static constexpr uint8_t NUM_PINS = NATIVE_GPIO_MAX_PINS;

    /// Native GPIO always supports PWM via analogWrite
    static constexpr bool HAS_HW_PWM = true;

    NativeGpio() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Mark the GPIO provider as available
     * @return Always true (native GPIO is always present)
     */
    bool begin() {
        _available = true;
        return true;
    }

    /**
     * @brief Check if the provider is initialized
     */
    bool isAvailable() const { return _available; }

    // ========================================================================
    // Pin-Level I/O
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
     * @brief Read a digital value from an input pin
     * @param pin MCU GPIO pin number
     * @return true if HIGH, false if LOW
     */
    bool readPin(uint8_t pin) {
        return digitalRead(pin) == HIGH;
    }

    // ========================================================================
    // Hardware PWM (via analogWrite)
    // ========================================================================

    /**
     * @brief No-op — native GPIO pins are always PWM-capable.
     *
     * Provided for interface compatibility with I2C expanders (AW9523B)
     * where per-pin LED/GPIO mode switching is required.
     *
     * @param pin MCU GPIO pin number (unused)
     * @param ledMode true = PWM mode, false = GPIO mode (ignored)
     * @return true
     */
    bool setLedMode(uint8_t pin, bool ledMode) {
        (void)pin;
        (void)ledMode;
        return true;
    }

    /**
     * @brief Set PWM duty cycle on a pin via analogWrite
     * @param pin MCU GPIO pin number
     * @param brightness 0 = off, 255 = full on
     * @return true
     *
     * On RP2040/RP2350: uses hardware PWM slice (all pins support it).
     * On ESP32-S3: uses LEDC peripheral (auto-assigned channel).
     */
    bool setLedBrightness(uint8_t pin, uint8_t brightness) {
        analogWrite(pin, brightness);
        return true;
    }

private:
    bool _available = false;
};

#endif // NATIVE_GPIO_H
