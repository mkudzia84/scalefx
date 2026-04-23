/*
 * GPIO Expander — Concept and Common Types
 *
 * Defines the compile-time concept (duck-typed interface) that all I2C GPIO
 * expander drivers must satisfy. Using C++ templates rather than virtual
 * dispatch for zero-overhead abstraction at LED tick rates.
 *
 * Expander Concept Requirements:
 *   An I2C GPIO expander type T must provide the following interface:
 *
 *     static constexpr uint8_t NUM_PORTS;             // Number of 8-bit ports
 *     static constexpr uint8_t NUM_PINS;              // Total pin count
 *
 *     bool begin(TwoWire& wire, uint8_t address);     // Init on I2C bus
 *     bool isAvailable() const;                        // Device online?
 *     void reset();                                    // Reset to defaults
 *
 *     // Port-level I/O (8 pins per port)
 *     bool    setPortDirection(uint8_t port, uint8_t mask);  // 1=input, 0=output
 *     uint8_t getPortDirection(uint8_t port);
 *     bool    writePort(uint8_t port, uint8_t value);
 *     uint8_t readPort(uint8_t port);
 *
 *     // Pin-level I/O
 *     bool setPinDirection(uint8_t pin, bool isInput);
 *     bool writePin(uint8_t pin, bool high);
 *     bool readPin(uint8_t pin);
 *
 *   Optional (for advanced features — used only if present):
 *
 *     // Hardware PWM (AW9523B)
 *     static constexpr bool HAS_HW_PWM = true|false;
 *     bool setLedMode(uint8_t pin, bool ledMode);      // true = HW PWM mode
 *     bool setLedBrightness(uint8_t pin, uint8_t val);  // 0-255 duty cycle
 *
 *     // Pull-up/down (PCAL6416A, AW9523B)
 *     bool setPullEnable(uint8_t port, uint8_t enableMask);
 *     bool setPullSelect(uint8_t port, uint8_t selectMask);
 *
 *     // Interrupts (PCAL6416A, AW9523B)
 *     bool    setInterruptMask(uint8_t port, uint8_t mask);
 *     uint8_t getInterruptStatus(uint8_t port);
 *
 * Current implementations:
 *   - PCAL6416A:  16-pin NXP I2C expander, GPIO only, BAM for PWM (pcal6416a.h)
 *   - AW9523B:    16-pin Awinic I2C expander, GPIO + hardware LED PWM (aw9523b.h)
 *   - NativeGpio: MCU GPIO wrapper, HW PWM via analogWrite (native_gpio.h)
 *
 * Template consumers (all template on the GPIO provider directly):
 *   - ExpanderBamT<TExpander> — Software BAM engine + GPIO provider (bam_led_drv.h)
 *   - LedControlT<TGpio>     — Single LED controller (led_control.h)
 *   - LedManager<N, TGpio>   — Multi-channel LED manager (led_manager.h)
 */

#ifndef GPIO_EXPANDER_H
#define GPIO_EXPANDER_H

#include <stdint.h>
#include <concepts>
#include <type_traits>

class TwoWire;

// ============================================================================
// Common Types
// ============================================================================

/// Pin direction
enum class ExpanderPinDir : uint8_t {
    Output = 0,
    Input  = 1
};

/// Pin drive mode
enum class ExpanderDriveMode : uint8_t {
    PushPull  = 0,
    OpenDrain = 1
};

/// Pin pull configuration  
enum class ExpanderPull : uint8_t {
    None     = 0,
    PullUp   = 1,
    PullDown = 2
};

// ============================================================================
// Compile-time Contract — C++20 Concepts
// ============================================================================
//
// `GpioExpander` is the minimum surface every expander driver must satisfy.
// Optional features (hardware PWM, LED brightness) live in their own
// concepts so consumers can branch with `if constexpr (HwPwmExpander<T>)`
// instead of value-trait booleans. The legacy `expander_has_*_v` aliases
// remain for older consumers and now resolve via the concepts.

/// Minimum I2C/native GPIO expander contract (port + pin I/O).
template <typename T>
concept GpioExpander = requires(T t, TwoWire& wire,
                                uint8_t address, uint8_t port,
                                uint8_t pin, uint8_t mask, bool high) {
    { T::NUM_PORTS }                       -> std::convertible_to<uint8_t>;
    { T::NUM_PINS  }                       -> std::convertible_to<uint8_t>;
    { t.begin(wire, address) }             -> std::convertible_to<bool>;
    { t.isAvailable() }                    -> std::convertible_to<bool>;
    { t.reset() }                          -> std::same_as<void>;
    { t.setPortDirection(port, mask) }     -> std::convertible_to<bool>;
    { t.writePort(port, mask) }            -> std::convertible_to<bool>;
    { t.readPort(port) }                   -> std::convertible_to<uint8_t>;
    { t.writePin(pin, high) }              -> std::convertible_to<bool>;
    { t.readPin(pin) }                     -> std::convertible_to<bool>;
};

/// True for expanders that advertise `HAS_HW_PWM = true`.
template <typename T>
concept HwPwmExpander = requires {
    { T::HAS_HW_PWM } -> std::convertible_to<bool>;
    requires T::HAS_HW_PWM == true;
};

/// True for expanders exposing `setLedMode` + `setLedBrightness`.
template <typename T>
concept LedBrightnessExpander = requires(T t, uint8_t pin, bool ledMode, uint8_t val) {
    { t.setLedMode(pin, ledMode) }      -> std::convertible_to<bool>;
    { t.setLedBrightness(pin, val) }    -> std::convertible_to<bool>;
};

// ----------------------------------------------------------------------------
// Legacy boolean aliases — kept so existing `if constexpr (expander_has_*_v)`
// call sites (bam_led_drv.h, led_control.h, led_manager.h) compile unchanged.
// New code should prefer `HwPwmExpander<T>` / `LedBrightnessExpander<T>`.
// ----------------------------------------------------------------------------

template <typename T>
constexpr bool expander_has_hw_pwm_v = HwPwmExpander<T>;

template <typename T>
constexpr bool expander_has_led_methods_v = LedBrightnessExpander<T>;

#endif // GPIO_EXPANDER_H
