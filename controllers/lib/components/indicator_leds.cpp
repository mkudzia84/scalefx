/*
 * Indicator LED Manager - Implementation
 *
 * Standardized indicator LED control for ScaleFX Pico controllers.
 * Eliminates duplicated updateIndicatorLEDs() across controllers.
 */

#include "indicator_leds.h"

void IndicatorLedManager::begin(uint8_t connectionPin, uint8_t errorPin) {
    _leds[0].begin(connectionPin);
    _leds[1].begin(errorPin);
}

void IndicatorLedManager::update() {
    // LED 0: Connection status
    if (_watchdogTriggered) {
        _leds[0].off();
    } else if (!_connected) {
        _leds[0].set((millis() / BLINK_WAITING_ms) % 2);  // Slow blink: waiting for INIT
    } else {
        _leds[0].on();  // Solid: connected
    }

    // LED 1: Error/warning status (three-tier priority)
    if (_errorCondition) {
        _leds[1].set((millis() / BLINK_ERROR_ms) % 2);    // Fast blink: error
    } else if (_warningCondition) {
        _leds[1].set((millis() / BLINK_WARNING_ms) % 2);  // Slow blink: warning
    } else {
        _leds[1].off();                                     // Off: normal
    }
}
