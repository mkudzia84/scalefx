/*
 * Indicator LED Manager - Header
 *
 * Standardized indicator LED control for ScaleFX Pico controllers.
 * Manages the two-LED status display on GP13 (connection) and GP14 (error)
 * with consistent behavior across all controllers.
 *
 * LED 0 (Connection):
 *   - Blink 500ms: Waiting for INIT (power-on, not yet connected)
 *   - Solid ON:    Connected and initialized
 *   - OFF:         Connection lost (watchdog timeout triggered)
 *
 * LED 1 (Error/Warning, three-tier priority):
 *   - Fast blink 200ms: Error condition (highest priority)
 *   - Slow blink 500ms: Warning condition (e.g. low voltage)
 *   - OFF:              Normal operation
 *
 * Usage:
 *   IndicatorLedManager indicators;
 *   indicators.begin(13, 14);
 *
 *   // State updates:
 *   indicators.setConnected(true);
 *   indicators.setErrorCondition(anyGearError);
 *   indicators.setWarningCondition(lowVoltage);
 *
 *   // In loop():
 *   indicators.update();
 */

#ifndef INDICATOR_LEDS_H
#define INDICATOR_LEDS_H

#include <Arduino.h>
#include "led_control.h"

// ============================================================================
// IndicatorLedManager Class
// ============================================================================

/**
 * @brief Manages standardized indicator LEDs for ScaleFX Pico controllers
 *
 * Encapsulates the connection/error LED pattern used identically across
 * all Pico server controllers (GunFX, LightFX, GearControl).
 *
 * Also tracks the `connected` and `watchdogTriggered` state that was
 * previously duplicated as bare `bool` variables in every controller.
 */
class IndicatorLedManager {
public:
    IndicatorLedManager() = default;
    ~IndicatorLedManager() = default;

    /**
     * @brief Initialize indicator LEDs
     * @param connectionPin GPIO pin for connection status LED (typically GP13)
     * @param errorPin GPIO pin for error/warning LED (typically GP14)
     */
    void begin(uint8_t connectionPin, uint8_t errorPin);

    /**
     * @brief Update indicator LED outputs based on current state
     *
     * Must be called every loop() iteration.
     */
    void update();

    // ========================================================================
    // State Setters
    // ========================================================================

    /** @brief Set connection state (true = INIT received, false = shutdown) */
    void setConnected(bool connected) { _connected = connected; }

    /** @brief Set watchdog triggered state (true = keepalive timeout) */
    void setWatchdogTriggered(bool triggered) { _watchdogTriggered = triggered; }

    /** @brief Set error condition (fast blink, highest priority on error LED) */
    void setErrorCondition(bool error) { _errorCondition = error; }

    /** @brief Set warning condition (slow blink, lower priority than error) */
    void setWarningCondition(bool warning) { _warningCondition = warning; }

    // ========================================================================
    // State Queries
    // ========================================================================

    /** @brief Check if controller is connected (INIT received, not shutdown) */
    bool isConnected() const { return _connected; }

    /** @brief Check if watchdog has triggered (keepalive timeout) */
    bool isWatchdogTriggered() const { return _watchdogTriggered; }

    // ========================================================================
    // LED Access (for building status flags, etc.)
    // ========================================================================

    /** @brief Get const reference to connection LED (for status queries) */
    const LedControl& connectionLed() const { return _leds[0]; }

    /** @brief Get const reference to error LED (for status queries) */
    const LedControl& errorLed() const { return _leds[1]; }

private:
    LedControl _leds[2];
    bool _connected = false;
    bool _watchdogTriggered = false;
    bool _errorCondition = false;
    bool _warningCondition = false;

    static constexpr uint16_t BLINK_WAITING_ms = 500;  // Connection waiting blink rate
    static constexpr uint16_t BLINK_ERROR_ms   = 200;  // Error fast blink rate
    static constexpr uint16_t BLINK_WARNING_ms = 500;  // Warning slow blink rate
};

#endif // INDICATOR_LEDS_H
