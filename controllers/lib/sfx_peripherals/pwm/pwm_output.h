/*
 * PwmOutput — Compile-time concept for pin-level PWM output providers
 *
 * Defines the single duck-typed interface that every PWM-capable output
 * provider (MCU pins via analogWrite, dedicated I²C PWM chips like PCA9685)
 * must satisfy.  LedControlT<T> and LedManager<N, T> template on this
 * concept, so adding a new provider is just "define a class with these
 * methods" — no virtual dispatch, no inheritance, no LED-runtime changes.
 *
 * Implementations in the library:
 *   - NativeGpio  — MCU GPIO with PWM via analogWrite       (gpio/native_gpio.h)
 *   - PCA9685     — NXP 16-channel 12-bit I²C PWM driver    (pwm/pca9685.h)
 *
 * The concept is intentionally minimal — the only operations the LED
 * runtime needs are:
 *   - probe availability      isAvailable()
 *   - mark a pin as output    setPinDirection(pin, isInput)
 *   - drive HIGH / LOW        writePin(pin, high)
 *   - set 8-bit PWM duty      setLedBrightness(pin, brightness)
 *
 * The `setLedBrightness` name is historical — for PCA9685 the same call
 * is used to emit servo / motor / generic-PWM duty values; "LED" is just
 * the most common use.  Renaming on a provider is mechanical if needed.
 *
 * Providers MAY additionally expose static traits for callers that want
 * compile-time information (e.g., `static constexpr uint8_t NUM_PINS`,
 * `static constexpr bool HAS_HW_PWM`); these are not required by the
 * concept itself.
 */

#ifndef PWM_OUTPUT_H
#define PWM_OUTPUT_H

#include <stdint.h>
#include <concepts>
#include <type_traits>

// ============================================================================
// Compile-time contract — C++20 concept
// ============================================================================

/// Minimum surface for a pin-level PWM output provider.
template <typename T>
concept PwmOutput = requires(T t, uint8_t pin, uint8_t brightness, bool high) {
    { t.isAvailable() }                     -> std::convertible_to<bool>;
    { t.setPinDirection(pin, false) }       -> std::convertible_to<bool>;
    { t.writePin(pin, high) }               -> std::convertible_to<bool>;
    { t.setLedBrightness(pin, brightness) } -> std::convertible_to<bool>;
};

#endif // PWM_OUTPUT_H
