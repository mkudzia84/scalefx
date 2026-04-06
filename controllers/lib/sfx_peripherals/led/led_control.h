/*
 * LED Control Library - Header
 *
 * Templatized LED control with pluggable GPIO provider backends.
 *
 * Features:
 *   - On/Off control
 *   - Active-high or active-low configuration
 *   - PWM brightness control (0-100%)
 *   - Master brightness scaling
 *   - State tracking
 *   - Direct GPIO provider binding (no intermediate driver layer)
 *
 * GPIO providers must satisfy the GpioExpander concept (gpio_expander.h):
 *   - bool isAvailable() const
 *   - bool setPinDirection(uint8_t pin, bool isInput)
 *   - bool setLedMode(uint8_t pin, bool ledMode)
 *   - bool setLedBrightness(uint8_t pin, uint8_t brightness)  // 0-255
 *   - bool writePin(uint8_t pin, bool high)
 *
 * Supported providers:
 *   - NativeGpio:        MCU GPIO with analogWrite PWM (native_gpio.h)
 *   - AW9523B:           I2C expander with 256-step HW LED PWM (aw9523b.h)
 *   - ExpanderBamT<T>:   Software BAM over GPIO-only expander (bam_led_drv.h)
 *
 * Template pattern:
 *   LedControlT<TGpio> stores {TGpio* _gpio, uint8_t _pin} and calls the
 *   GPIO provider's methods directly — no intermediate driver layer.
 *   `LedControl` is the default alias for NativeGpio — fully backward
 *   compatible with all existing code.
 *
 * Usage (GPIO — unchanged from original API):
 *   LedControl led;                       // = LedControlT<NativeGpio>
 *   led.begin(13, false, true);           // GPIO 13, active-high, PWM enabled
 *   led.on();
 *   led.setBrightness(50);
 *
 * Usage (I2C Expander with HW PWM):
 *   AW9523B expander;
 *   expander.begin(Wire, 0x58);
 *   LedControlT<AW9523B> led;
 *   led.begin(&expander, 0, false, true); // P0_0, active-high, PWM
 *   led.setBrightness(75);
 *
 * Usage (Software BAM on GPIO-only expander):
 *   ExpanderBamT<PCAL6416A> bamEngine;
 *   bamEngine.begin(&pcal, 0);
 *   LedControlT<ExpanderBamT<PCAL6416A>> led;
 *   led.begin(&bamEngine, 0, false, true);
 *   led.setBrightness(50);
 *   // Must call bamEngine.update() in loop()!
 *
 * For event-based animations, use LedEventSeq with any LedControlT variant
 * via the ILedOutput interface.
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>
#include <type_traits>
#include "../gpio/native_gpio.h"

// ============================================================================
// ILedOutput — Abstract interface for LED output consumers
// ============================================================================

/**
 * @brief Minimal interface for LED output, used by LedEventSeq and other
 *        animation systems that don't need to know the GPIO provider type.
 *
 * Virtual dispatch overhead is negligible at LED animation tick rates (~100 Hz).
 */
class ILedOutput {
public:
    virtual ~ILedOutput() = default;

    /// Set brightness 0-100 (0 = off, 100 = full)
    virtual void setBrightness(uint8_t brightness) = 0;

    /// Turn LED off
    virtual void off() = 0;

    /// Turn LED on (full brightness)
    virtual void on() = 0;

    /// Check if LED is currently on
    virtual bool isOn() const = 0;
};

// ============================================================================
// LedControlT<TGpio> — Templatized LED controller
// ============================================================================

/**
 * @brief LED control with direct GPIO provider binding.
 *
 * @tparam TGpio GPIO provider type (e.g., NativeGpio, AW9523B, ExpanderBamT<T>).
 *   Must satisfy the GpioExpander concept: isAvailable(), setPinDirection(),
 *   setLedMode(), setLedBrightness(), writePin().
 *
 * Each instance stores a pointer to the GPIO provider and a pin number.
 * Multiple LedControlT instances can share the same provider (e.g., 6 LEDs
 * on one AW9523B expander, or 8 channels on one NativeGpio singleton).
 */
template <typename TGpio>
class LedControlT : public ILedOutput {
public:
    LedControlT() = default;
    ~LedControlT() override;

    // ========================================================================
    // GPIO Provider Access
    // ========================================================================

    /// Access the underlying GPIO provider
    TGpio*       gpio()       { return _gpio; }
    const TGpio* gpio() const { return _gpio; }

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize LED on a specific GPIO provider and pin.
     *
     * Configures the pin as output with LED mode enabled, then sets
     * initial state to off.
     *
     * @param gpio Pointer to initialized GPIO provider (must outlive this object)
     * @param pin Pin number (meaning depends on provider)
     * @param activeLow true if LED is on when pin is LOW (default: false)
     * @param usePwm true to enable PWM for brightness control (default: false)
     * @return true if initialization succeeded
     */
    bool begin(TGpio* gpio, uint8_t pin, bool activeLow = false, bool usePwm = false);

    /**
     * @brief Convenience: initialize LED on a native GPIO pin.
     *
     * Uses NativeGpio::instance() as the GPIO provider.
     * Only available when TGpio = NativeGpio (SFINAE).
     *
     * @param pin GPIO pin number
     * @param activeLow true if LED is on when pin is LOW (default: false)
     * @param usePwm true to enable PWM for brightness control (default: false)
     * @return true if initialization succeeded
     */
    template <typename U = TGpio>
    std::enable_if_t<std::is_same<U, NativeGpio>::value, bool>
    begin(int pin, bool activeLow = false, bool usePwm = false) {
        return begin(&NativeGpio::instance(), static_cast<uint8_t>(pin), activeLow, usePwm);
    }

    /**
     * @brief Configure LED behavior for an already-attached GPIO/pin.
     *
     * Call begin(gpio, pin, ...) first, then call configure() if you need
     * to change activeLow/usePwm without re-initializing the pin.
     *
     * @param activeLow true if LED is on when pin is LOW (default: false)
     * @param usePwm true to enable PWM for brightness control (default: false)
     * @return true if attached
     */
    bool configure(bool activeLow = false, bool usePwm = false);

    /**
     * @brief Detach from the GPIO provider
     */
    void end();

    /**
     * @brief Check if LED is initialized
     */
    bool isAttached() const { return _attached; }

    // ========================================================================
    // Basic Control (ILedOutput interface + extras)
    // ========================================================================

    void on() override;
    void off() override;

    /**
     * @brief Toggle LED state
     */
    void toggle();

    /**
     * @brief Set LED state
     * @param state true for on, false for off
     */
    void set(bool state);

    void setBrightness(uint8_t brightness) override;

    /**
     * @brief Set master brightness scaling (0-100%)
     *
     * Scales ALL output from this LED. Default is 100 (no scaling).
     * Immediately re-applies to current output.
     *
     * @param pct Brightness percentage (0 = off, 100 = full, clamped to 100)
     */
    void setMasterBrightness_pct(uint8_t pct);

    /**
     * @brief Get master brightness percentage (0-100)
     */
    uint8_t masterBrightness_pct() const { return _masterBrightness_pct; }

    // ========================================================================
    // State
    // ========================================================================

    bool isOn() const override { return _state; }
    bool isOff() const { return !_state; }

    /**
     * @brief Get current brightness (0-100)
     */
    uint8_t brightness() const { return _brightness; }

    /**
     * @brief Get the pin number (-1 if unattached)
     */
    int pin() const { return _attached ? static_cast<int>(_pin) : -1; }

    /**
     * @brief Check if active-low mode
     */
    bool isActiveLow() const { return _activeLow; }

    /**
     * @brief Check if PWM is enabled
     */
    bool isPwmEnabled() const { return _usePwm; }

private:
    void writePin(bool state);
    void writePwm(uint8_t value);

    TGpio*  _gpio = nullptr;
    uint8_t _pin = 0;
    bool    _attached = false;
    bool    _activeLow = false;
    bool    _usePwm = false;
    bool    _state = false;
    uint8_t _brightness = 0;
    uint8_t _masterBrightness_pct = 100;
};

// ============================================================================
// Default alias — backward compatible with all existing code
// ============================================================================

/// LedControl = LedControlT<NativeGpio> (native GPIO, original API)
using LedControl = LedControlT<NativeGpio>;

// ============================================================================
// Template implementation
// ============================================================================

#include "led_control.ipp"

#endif // LED_CONTROL_H
