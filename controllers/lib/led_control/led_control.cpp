/*
 * LED Control Library - Implementation
 * 
 * Simple LED control on GPIO pins.
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

bool LedControl::begin(int pin, bool activeLow) {
    if (pin < 0) {
        return false;
    }
    
    // End any previous configuration
    end();
    
    _pin = pin;
    _activeLow = activeLow;
    _state = false;
    
    // Configure pin as output
    pinMode(_pin, OUTPUT);
    
    // Start in off state
    writePin(false);
    
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
}

// ============================================================================
// Control
// ============================================================================

void LedControl::on() {
    if (!_attached) return;
    _state = true;
    writePin(true);
}

void LedControl::off() {
    if (!_attached) return;
    _state = false;
    writePin(false);
}

void LedControl::toggle() {
    if (!_attached) return;
    _state = !_state;
    writePin(_state);
}

void LedControl::set(bool state) {
    if (!_attached) return;
    _state = state;
    writePin(state);
}

// ============================================================================
// Private
// ============================================================================

void LedControl::writePin(bool state) {
    // Handle active-low logic
    bool pinLevel = _activeLow ? !state : state;
    digitalWrite(_pin, pinLevel ? HIGH : LOW);
}
