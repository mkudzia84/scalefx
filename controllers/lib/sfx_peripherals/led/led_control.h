/*
 * LED Control Library - Header
 * 
 * Templatized LED control with pluggable hardware drivers.
 * 
 * Features:
 *   - On/Off control
 *   - Active-high or active-low configuration
 *   - PWM brightness control (0-100%)
 *   - Master brightness scaling
 *   - State tracking
 *   - Pluggable driver backends (GPIO, expander, future PWM IC)
 * 
 * Driver backends (see gpio_led_drv.h, expander_led_drv.h):
 *   - GpioLedDriver:     Native GPIO (analogWrite/digitalWrite)
 *   - ExpanderLedDriver: PCAL6416A I2C expander with software BAM
 *   - HwPwmLedDriver:    AW9523B / NativeGpio hardware PWM (hw_pwm_led_drv.h)
 *   - (future) PCA9685:  Hardware PWM I2C expander
 * 
 * Template pattern:
 *   LedControlT<TDriver> is templatized on the driver policy.
 *   `LedControl` is the default alias for GpioLedDriver — fully backward
 *   compatible with all existing code.
 * 
 * Usage (GPIO — unchanged from original API):
 *   LedControl led;                       // = LedControlT<GpioLedDriver>
 *   led.begin(13, false, true);           // GPIO 13, active-high, PWM enabled
 *   led.on();
 *   led.setBrightness(50);
 * 
 * Usage (Expander):
 *   LedControlT<ExpanderLedDriver> led;
 *   led.driver().begin(&bamEngine, 0);    // bind to BAM engine pin 0
 *   led.configure(false, true);           // active-high, PWM enabled
 *   led.setBrightness(75);
 * 
 * For event-based animations, use LedEventSeq with any LedControlT variant
 * via the ILedOutput interface.
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>
#include "gpio_led_drv.h"

// ============================================================================
// ILedOutput — Abstract interface for LED output consumers
// ============================================================================

/**
 * @brief Minimal interface for LED output, used by LedEventSeq and other
 *        animation systems that don't need to know the driver type.
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
// LedControlT<TDriver> — Templatized LED controller
// ============================================================================

/**
 * @brief LED control with pluggable hardware driver backend.
 *
 * @tparam TDriver Hardware driver policy (e.g., GpioLedDriver, ExpanderLedDriver).
 *   Must provide: begin(...), end(), isAttached(), pin(), writeDigital(bool),
 *   writePwm(uint8_t), supportsPwm(), update()
 */
template <typename TDriver>
class LedControlT : public ILedOutput {
public:
    LedControlT() = default;
    ~LedControlT() override;
    
    // ========================================================================
    // Driver Access
    // ========================================================================

    /// Access the underlying hardware driver for driver-specific configuration
    TDriver&       driver()       { return _driver; }
    const TDriver& driver() const { return _driver; }

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize LED on a GPIO pin (GpioLedDriver convenience — backward compatible)
     *
     * Calls driver().begin(pin) internally. Only valid when TDriver accepts
     * begin(int) — compile error otherwise (e.g., ExpanderLedDriver needs engine+pin).
     *
     * @param pin GPIO pin number
     * @param activeLow true if LED is on when pin is LOW (default: false)
     * @param usePwm true to enable PWM for brightness control (default: false)
     * @return true if initialization succeeded
     */
    bool begin(int pin, bool activeLow = false, bool usePwm = false);

    /**
     * @brief Configure LED behavior for an already-initialized driver.
     *
     * Call driver().begin(...) first with driver-specific args, then call
     * configure() to set the LED logic mode.
     *
     * @param activeLow true if LED is on when pin is LOW (default: false)
     * @param usePwm true to enable PWM for brightness control (default: driver capability)
     * @return true if driver is attached
     */
    bool configure(bool activeLow = false, bool usePwm = false);

    /**
     * @brief Release the underlying driver
     */
    void end();

    /**
     * @brief Check if LED is initialized
     */
    bool isAttached() const { return _driver.isAttached(); }

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
     * @brief Get the logical pin/channel ID from the driver
     */
    int pin() const { return _driver.pin(); }

    /**
     * @brief Check if active-low mode
     */
    bool isActiveLow() const { return _activeLow; }

    /**
     * @brief Check if PWM is enabled
     */
    bool isPwmEnabled() const { return _usePwm; }

    // ========================================================================
    // Driver Tick (for time-sliced drivers like BAM)
    // ========================================================================

    /**
     * @brief Forward update() to the driver. No-op for immediate drivers (GPIO).
     * Must be called in loop() for BAM-based expander drivers.
     */
    void update() { _driver.update(); }

private:
    void writePin(bool state);
    void writePwm(uint8_t value);

    TDriver _driver;
    bool _activeLow = false;
    bool _usePwm = false;
    bool _state = false;
    uint8_t _brightness = 0;
    uint8_t _masterBrightness_pct = 100;
};

// ============================================================================
// Default alias — backward compatible with all existing code
// ============================================================================

/// LedControl = LedControlT<GpioLedDriver> (native GPIO, original API)
using LedControl = LedControlT<GpioLedDriver>;

// ============================================================================
// Template implementation
// ============================================================================

#include "led_control.ipp"

#endif // LED_CONTROL_H
