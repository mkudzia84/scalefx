/*
 * IndicatorServicePolicy — state-machine implementation.
 *
 * configure() initialises the LedControl pair on the supplied pins
 * (skipping any pin < 0 — disabled).  update() ticks the two state
 * machines from the boolean flags set by the lifecycle hooks.
 */

#include "indicator_leds.h"

void IndicatorServicePolicy::configure(const Config& cfg) {
    if (cfg.connectionPin >= 0) _leds[0].begin(cfg.connectionPin);
    if (cfg.errorPin      >= 0) _leds[1].begin(cfg.errorPin);
}

void IndicatorServicePolicy::update() {
    // LED 0: Connection status
    if (_watchdogTriggered) {
        _leds[0].off();
    } else if (!_connected) {
        // Slow blink: waiting for INIT
        _leds[0].set((millis() / BLINK_WAITING_ms) % 2);
    } else {
        _leds[0].on();   // Solid: connected
    }

    // LED 1: Error / warning status (three-tier priority)
    if (_errorCondition) {
        _leds[1].set((millis() / BLINK_ERROR_ms) % 2);     // Fast blink: error
    } else if (_warningCondition) {
        _leds[1].set((millis() / BLINK_WARNING_ms) % 2);   // Slow blink: warning
    } else {
        _leds[1].off();                                     // Off: normal
    }
}
