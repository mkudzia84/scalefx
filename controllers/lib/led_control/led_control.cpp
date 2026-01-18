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
    _brightness = 255;
    if (_usePwm) {
        writePwm(255);
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
    _brightness = _state ? 255 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(_state);
    }
}

void LedControl::set(bool state) {
    if (!_attached) return;
    _state = state;
    _brightness = state ? 255 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(state);
    }
}

void LedControl::setBrightness(uint8_t brightness) {
    if (!_attached) return;
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

void LedControl::writePwm(uint8_t value) {
    if (_activeLow) {
        analogWrite(_pin, 255 - value);
    } else {
        analogWrite(_pin, value);
    }
}
