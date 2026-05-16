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

template <typename TGpio>
LedControlT<TGpio>::~LedControlT() {
    end();
}

// ============================================================================
// Initialization
// ============================================================================

template <typename TGpio>
bool LedControlT<TGpio>::begin(TGpio* gpio, uint8_t pin, bool activeLow, bool usePwm) {
    // End any previous configuration
    end();

    if (!gpio || !gpio->isAvailable()) return false;

    _gpio = gpio;
    _pin = pin;
    _activeLow = activeLow;
    _usePwm = usePwm;
    _state = false;
    _brightness = 0;
    _attached = true;

    // Configure pin as output. Backends with HW PWM (PCA9685, NativeGpio
    // via analogWrite) accept the brightness writes directly — no
    // separate "enable PWM mode" step is needed.
    _gpio->setPinDirection(pin, false);   // Output

    // Start in off state
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }

    return true;
}

template <typename TGpio>
bool LedControlT<TGpio>::configure(bool activeLow, bool usePwm) {
    if (!_attached) return false;

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

template <typename TGpio>
void LedControlT<TGpio>::end() {
    if (_attached && _gpio) {
        off();
    }
    _gpio = nullptr;
    _pin = 0;
    _attached = false;
    _state = false;
    _brightness = 0;
}

// ============================================================================
// Basic Control
// ============================================================================

template <typename TGpio>
void LedControlT<TGpio>::on() {
    if (!_attached) return;
    _state = true;
    _brightness = 100;
    if (_usePwm) {
        writePwm(100);
    } else {
        writePin(true);
    }
}

template <typename TGpio>
void LedControlT<TGpio>::off() {
    if (!_attached) return;
    _state = false;
    _brightness = 0;
    if (_usePwm) {
        writePwm(0);
    } else {
        writePin(false);
    }
}

template <typename TGpio>
void LedControlT<TGpio>::toggle() {
    if (!_attached) return;
    _state = !_state;
    _brightness = _state ? 100 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(_state);
    }
}

template <typename TGpio>
void LedControlT<TGpio>::set(bool state) {
    if (!_attached) return;
    _state = state;
    _brightness = state ? 100 : 0;
    if (_usePwm) {
        writePwm(_brightness);
    } else {
        writePin(state);
    }
}

template <typename TGpio>
void LedControlT<TGpio>::setBrightness(uint8_t brightness) {
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

template <typename TGpio>
void LedControlT<TGpio>::setMasterBrightness_pct(uint8_t pct) {
    _masterBrightness_pct = (pct > 100) ? 100 : pct;
    // Re-apply current brightness with new master scaling
    if (_attached && _usePwm) {
        writePwm(_brightness);
    } else if (_attached && !_usePwm) {
        writePin(_state && _masterBrightness_pct > 0);
    }
}

// ============================================================================
// Private Methods
// ============================================================================

template <typename TGpio>
void LedControlT<TGpio>::writePin(bool state) {
    if (_activeLow) state = !state;
    _gpio->writePin(_pin, state);
}

template <typename TGpio>
void LedControlT<TGpio>::writePwm(uint8_t value) {
    // value is 0-100 brightness, scale by master brightness and convert to 0-255 PWM
    uint8_t pwm = (uint8_t)((uint32_t)value * _masterBrightness_pct * 255 / 10000);

    if (_activeLow) {
        _gpio->setLedBrightness(_pin, 255 - pwm);
    } else {
        _gpio->setLedBrightness(_pin, pwm);
    }
}

#endif // LED_CONTROL_IPP
