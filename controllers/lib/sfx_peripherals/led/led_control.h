/*
 * LED Control Library - Header
 * 
 * Simple LED control on GPIO pins with brightness support.
 * 
 * Features:
 *   - On/Off control
 *   - Active-high or active-low configuration
 *   - PWM brightness control
 *   - State tracking
 * 
 * For event-based animations, use LedEventSeq with this class.
 * 
 * Usage:
 *   LedControl led;
 *   led.begin(13, false, true);  // GPIO 13, PWM enabled
 *   led.on();                     // Turn on
 *   led.off();                    // Turn off
 *   led.setBrightness(50);        // Half brightness
 *   
 *   // With event sequence:
 *   LedEventSeq seq;
 *   seq.attachLed(&led);
 *   seq.add(new LedFlashing(500, 2000));
 *   seq.start();
 *   seq.update();  // Call in loop()
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>

// ============================================================================
// LedControl Class
// ============================================================================

/**
 * @brief LED control on a GPIO pin with event-based animations
 */
class LedControl {
public:
    LedControl() = default;
    ~LedControl();
    
    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize LED on a GPIO pin
     * @param pin GPIO pin number
     * @param activeLow true if LED is on when pin is LOW (default: false)
     * @param usePwm true to enable PWM for brightness control (default: false)
     * @return true if initialization succeeded
     */
    bool begin(int pin, bool activeLow = false, bool usePwm = false);

    /**
     * @brief Release the GPIO pin
     */
    void end();

    /**
     * @brief Check if LED is initialized
     */
    bool isAttached() const { return _attached; }

    // ========================================================================
    // Basic Control
    // ========================================================================

    /**
     * @brief Turn LED on (stops any active event)
     */
    void on();

    /**
     * @brief Turn LED off (stops any active event)
     */
    void off();

    /**
     * @brief Toggle LED state (stops any active event)
     */
    void toggle();

    /**
     * @brief Set LED state (stops any active event)
     * @param state true for on, false for off
     */
    void set(bool state);

    /**
     * @brief Set LED brightness (requires PWM enabled)
     * @param brightness 0-100 (0 = off, 100 = full)
     */
    void setBrightness(uint8_t brightness);

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

    /**
     * @brief Check if LED is currently on
     * @return true if LED is on
     */
    bool isOn() const { return _state; }

    /**
     * @brief Check if LED is currently off
     * @return true if LED is off
     */
    bool isOff() const { return !_state; }

    /**
     * @brief Get current brightness (0-100)
     */
    uint8_t brightness() const { return _brightness; }

    /**
     * @brief Get the GPIO pin
     */
    int pin() const { return _pin; }

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

    int _pin = -1;
    bool _attached = false;
    bool _activeLow = false;
    bool _usePwm = false;
    bool _state = false;
    uint8_t _brightness = 0;
    uint8_t _masterBrightness_pct = 100;
};

#endif // LED_CONTROL_H
