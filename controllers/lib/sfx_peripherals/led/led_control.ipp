/*
 * LED Control Library - Template Implementation
 *
 * Included by led_control.h — do not include directly.
 */

#ifndef LED_CONTROL_IPP
#define LED_CONTROL_IPP

// ============================================================================
// Destructor
// ============================================================================

template <typename TDriver>
LedControlT<TDriver>::~LedControlT() {
    end();
}

// ============================================================================
// Initialization
// ============================================================================

template <typename TDriver>
bool LedControlT<TDriver>::begin(int pin, bool activeLow, bool usePwm) {
    // End any previous configuration
    end();

    if (!_driver.begin(pin)) return false;

    _activeLow = activeLow;
    _usePwm = usePwm;
    _state = false;
    _brightness = 0;

    // Start in off state
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }

    return true;
}

template <typename TDriver>
bool LedControlT<TDriver>::configure(bool activeLow, bool usePwm) {
    if (!_driver.isAttached()) return false;

    _activeLow = activeLow;
    _usePwm = usePwm;
    _state = false;
    _brightness = 0;

    // Start in off state
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }

    return true;
}

template <typename TDriver>
void LedControlT<TDriver>::end() {
    if (_driver.isAttached()) {
        off();
        _driver.end();
    }
    _state = false;
    _brightness = 0;
}

// ============================================================================
// Basic Control
// ============================================================================

template <typename TDriver>
void LedControlT<TDriver>::on() {
    if (!_driver.isAttached()) return;
    _state = true;
    _brightness = 100;
    if (_usePwm) {
        writePwm(100);
    } else {
        writePin(true);
    }
}

template <typename TDriver>
void LedControlT<TDriver>::off() {
    if (!_driver.isAttached()) return;
    _state = false;
    _brightness = 0;
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }
}

template <typename TDriver>
void LedControlT<TDriver>::toggle() {
    if (!_driver.isAttached()) return;
    _state = !_state;
    _brightness = _state ? 100 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(_state);
    }
}

template <typename TDriver>
void LedControlT<TDriver>::set(bool state) {
    if (!_driver.isAttached()) return;
    _state = state;
    _brightness = state ? 100 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(state);
    }
}

template <typename TDriver>
void LedControlT<TDriver>::setBrightness(uint8_t brightness) {
    if (!_driver.isAttached()) return;
    if (brightness > 100) brightness = 100;
    _brightness = brightness;
    _state = (brightness > 0);
    if (_usePwm) {
        writePwm(brightness);
    } else {
        writePin(_state);
    }
}

template <typename TDriver>
void LedControlT<TDriver>::setMasterBrightness_pct(uint8_t pct) {
    _masterBrightness_pct = (pct > 100) ? 100 : pct;
    // Re-apply current brightness with new master scaling
    if (_driver.isAttached() && _usePwm) {
        writePwm(_brightness);
    } else if (_driver.isAttached() && !_usePwm) {
        writePin(_state && _masterBrightness_pct > 0);
    }
}

// ============================================================================
// Private Methods
// ============================================================================

template <typename TDriver>
void LedControlT<TDriver>::writePin(bool state) {
    if (_activeLow) {
        _driver.writeDigital(!state);
    } else {
        _driver.writeDigital(state);
    }
}

template <typename TDriver>
void LedControlT<TDriver>::writePwm(uint8_t value) {
    // value is 0-100 brightness, scale by master brightness and convert to 0-255 PWM
    uint8_t pwm = (uint8_t)((uint32_t)value * _masterBrightness_pct * 255 / 10000);

    if (_activeLow) {
        _driver.writePwm(255 - pwm);
    } else {
        _driver.writePwm(pwm);
    }
}

#endif // LED_CONTROL_IPP
