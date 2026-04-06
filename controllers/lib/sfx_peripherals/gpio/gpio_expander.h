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
 * Template consumers:
 *   - ExpanderBamT<TExpander>       — Software BAM engine (any GPIO expander)
 *   - ExpanderLedDriverT<TExpander> — Per-pin BAM driver (software PWM)
 *   - HwPwmLedDriverT<TExpander>    — Per-pin hardware PWM driver (AW9523B)
 *   - LedControlT<TDriver>          — LED controller (any driver policy)
 *   - LedManager<N, TDriver>        — Multi-channel LED manager
 */

#ifndef GPIO_EXPANDER_H
#define GPIO_EXPANDER_H

#include <stdint.h>

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
// Compile-time Concept Check (C++17 SFINAE helper)
// ============================================================================

namespace expander_detail {

/// Check if T has HAS_HW_PWM = true (false by default)
template <typename T, typename = void>
struct has_hw_pwm : std::false_type {};

template <typename T>
struct has_hw_pwm<T, std::enable_if_t<T::HAS_HW_PWM>> : std::true_type {};

/// Check if T has setLedMode and setLedBrightness
template <typename T, typename = void>
struct has_led_methods : std::false_type {};

template <typename T>
struct has_led_methods<T, std::void_t<
    decltype(std::declval<T>().setLedMode(uint8_t{}, bool{})),
    decltype(std::declval<T>().setLedBrightness(uint8_t{}, uint8_t{}))
>> : std::true_type {};

} // namespace expander_detail

/// True if TExpander supports hardware PWM (has HAS_HW_PWM = true)
template <typename T>
constexpr bool expander_has_hw_pwm_v = expander_detail::has_hw_pwm<T>::value;

/// True if TExpander has setLedMode/setLedBrightness methods
template <typename T>
constexpr bool expander_has_led_methods_v = expander_detail::has_led_methods<T>::value;

#endif // GPIO_EXPANDER_H
