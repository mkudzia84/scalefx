/*
 * LED Control Library - Implementation
 * 
 * LED control on GPIO pins with event-based animations.
 */

#include "led_control.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

LedControl::~LedControl() {
    end();
}

// ============================================================================
// Initialization
// ============================================================================

bool LedControl::begin(int pin, bool activeLow, bool usePwm) {
    if (pin < 0) {
        return false;
    }
    
    // End any previous configuration
    end();
    
    _pin = pin;
    _activeLow = activeLow;
    _usePwm = usePwm;
    _state = false;
    _brightness = 0;
    
    // Configure pin as output
    pinMode(_pin, OUTPUT);
    
    // Start in off state
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }
    
    _attached = true;
    return true;
}

void LedControl::end() {
    if (_attached) {
        off();
        _attached = false;
    }
    _pin = -1;
    _state = false;
    _brightness = 0;
}

// ============================================================================
// Basic Control
// ============================================================================

void LedControl::on() {
    if (!_attached) return;
    _state = true;
    _brightness = 100;
    if (_usePwm) {
        writePwm(100);
    } else {
        writePin(true);
    }
}

void LedControl::off() {
    if (!_attached) return;
    _state = false;
    _brightness = 0;
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }
}

void LedControl::toggle() {
    if (!_attached) return;
    _state = !_state;
    _brightness = _state ? 100 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(_state);
    }
}

void LedControl::set(bool state) {
    if (!_attached) return;
    _state = state;
    _brightness = state ? 100 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(state);
    }
}

void LedControl::setBrightness(uint8_t brightness) {
    if (!_attached) return;
    if (brightness > 100) brightness = 100;
    _brightness = brightness;
    _state = (brightness > 0);
    if (_usePwm) {
        writePwm(brightness);
    } else {
        writePin(_state);
    }
}

// ============================================================================
// Private Methods
// ============================================================================

void LedControl::writePin(bool state) {
    if (_activeLow) {
        digitalWrite(_pin, state ? LOW : HIGH);
    } else {
        digitalWrite(_pin, state ? HIGH : LOW);
    }
}

void LedControl::setMasterBrightness_pct(uint8_t pct) {
    _masterBrightness_pct = (pct > 100) ? 100 : pct;
    // Re-apply current brightness with new master scaling
    if (_attached && _usePwm) {
        writePwm(_brightness);
    } else if (_attached && !_usePwm) {
        writePin(_state && _masterBrightness_pct > 0);
    }
}

void LedControl::writePwm(uint8_t value) {
    // value is 0-100 brightness, scale by master brightness and convert to 0-255 PWM
    uint8_t pwm = (uint8_t)((uint32_t)value * _masterBrightness_pct * 255 / 10000);

    if (_activeLow) {
        analogWrite(_pin, 255 - pwm);
    } else {
        analogWrite(_pin, pwm);
    }
}
