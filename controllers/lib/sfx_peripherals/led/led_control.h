/*
 * LED Control Library — Header
 *
 * Templatized single-channel LED control with pluggable PWM-output
 * backends.  One C++ template, two backends today:
 *
 *   - NativeGpio  — MCU pin + analogWrite                 (gpio/native_gpio.h)
 *   - PCA9685     — NXP 16-channel 12-bit I²C PWM chip    (pwm/pca9685.h)
 *
 * Backends must satisfy the PwmOutput concept (pwm/pwm_output.h):
 *   - bool isAvailable() const
 *   - bool setPinDirection(uint8_t pin, bool isInput)
 *   - bool writePin(uint8_t pin, bool high)
 *   - bool setLedBrightness(uint8_t pin, uint8_t brightness)  // 0-255
 *
 * Features:
 *   - On / off / toggle
 *   - Active-high or active-low configuration
 *   - PWM brightness 0-100% with master-brightness scaling
 *   - State tracking
 *   - Direct backend binding (no intermediate driver layer)
 *
 * Template pattern:
 *   LedControlT<TPwm> stores {TPwm* _gpio, uint8_t _pin} and calls the
 *   backend's methods directly — no virtual dispatch.  `LedControl` is
 *   the default alias for NativeGpio — fully backward compatible with
 *   existing call sites that use MCU pins.
 *
 * Usage (native pin):
 *   LedControl led;                          // = LedControlT<NativeGpio>
 *   led.begin(13, false, true);              // GPIO 13, active-high, PWM
 *   led.on();
 *   led.setBrightness(50);
 *
 * Usage (PCA9685 channel):
 *   PCA9685 pwm;
 *   pwm.begin(Wire, 0x70);
 *   LedControlT<PCA9685> led;
 *   led.begin(&pwm, 3, false, true);         // channel 3, active-high, PWM
 *   led.setBrightness(75);
 *
 * For event-based animations, use LedEventSeq with any LedControlT
 * variant via the ILedOutput interface.
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>
#include <type_traits>
#include "../gpio/native_gpio.h"
#include "../pwm/pwm_output.h"

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
 * @brief LED control with direct PWM-output backend binding.
 *
 * @tparam TGpio PWM-output backend type (NativeGpio or PCA9685).
 *   Must satisfy the PwmOutput concept (pwm/pwm_output.h): isAvailable(),
 *   setPinDirection(), writePin(), setLedBrightness().
 *
 * Each instance stores a pointer to the backend and a pin/channel
 * number.  Multiple LedControlT instances can share the same backend
 * (e.g. 8 LEDs on one PCA9685 chip, or several channels on the
 * NativeGpio singleton).
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
