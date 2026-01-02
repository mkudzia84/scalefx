/*
 * LED Control Library - Header
 * 
 * Simple LED control on GPIO pins.
 * 
 * Features:
 *   - On/Off control
 *   - Active-high or active-low configuration
 *   - State tracking
 * 
 * Usage:
 *   LedControl led;
 *   led.begin(13);       // GPIO 13
 *   led.on();            // Turn on
 *   led.off();           // Turn off
 *   led.toggle();        // Toggle state
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>

// ============================================================================
// LedControl Class
// ============================================================================

/**
 * @brief Simple LED control on a GPIO pin
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
     * @return true if initialization succeeded
     */
    bool begin(int pin, bool activeLow = false);

    /**
     * @brief Release the GPIO pin
     */
    void end();

    /**
     * @brief Check if LED is initialized
     */
    bool isAttached() const { return _attached; }

    // ========================================================================
    // Control
    // ========================================================================

    /**
     * @brief Turn LED on
     */
    void on();

    /**
     * @brief Turn LED off
     */
    void off();

    /**
     * @brief Toggle LED state
     */
    void toggle();

    /**
     * @brief Set LED state
     * @param state true for on, false for off
     */
    void set(bool state);

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
     * @brief Get the GPIO pin
     */
    int pin() const { return _pin; }

    /**
     * @brief Check if active-low mode
     */
    bool isActiveLow() const { return _activeLow; }

private:
    void writePin(bool state);

    int _pin = -1;
    bool _attached = false;
    bool _activeLow = false;
    bool _state = false;
};

#endif // LED_CONTROL_H
