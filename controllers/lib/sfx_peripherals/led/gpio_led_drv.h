/*
 * LED Driver Abstraction — Policy Interface
 *
 * Defines the contract for LED hardware backends. Each driver provides
 * low-level pin setup, on/off control, and PWM output.
 *
 * Concrete drivers:
 *   - GpioLedDriver:     Native GPIO (analogWrite / digitalWrite)
 *   - ExpanderLedDriver: PCAL6416A GPIO expander with software BAM
 *   - HwPwmLedDriver:    AW9523B / NativeGpio hardware PWM (hw_pwm_led_drv.h)
 *
 * LedControlT<TDriver> is templatized on the driver policy. Consumers use
 * the `LedControl` alias which resolves to the correct driver per platform
 * or configuration.
 *
 * Driver policy requirements (duck-typed, no virtual):
 *   - bool begin(args...)       — bind to a pin/channel, configure as output
 *   - void end()                — release the pin/channel
 *   - bool isAttached() const   — is the driver bound to a pin?
 *   - int  pin() const          — logical pin/channel ID (-1 if unattached)
 *   - void writeDigital(bool)   — on/off output (active-high; caller handles polarity)
 *   - void writePwm(uint8_t)    — PWM duty 0-255 (caller handles polarity)
 *   - bool supportsPwm() const  — true if the driver supports writePwm()
 *   - void update()             — periodic tick (BAM drivers need this; GPIO is no-op)
 */

#ifndef GPIO_LED_DRV_H
#define GPIO_LED_DRV_H

#include <Arduino.h>

// ============================================================================
// GpioLedDriver — Native GPIO pins with analogWrite PWM
// ============================================================================

/**
 * @brief LED driver using native MCU GPIO pins.
 *
 * Uses Arduino analogWrite() for PWM (8-bit resolution) and
 * digitalWrite() for on/off. Works on any pin that supports
 * analogWrite (all pins on RP2040, most on ESP32).
 */
class GpioLedDriver {
public:
    GpioLedDriver() = default;

    /**
     * @brief Bind to a GPIO pin and configure as output
     * @param pin GPIO pin number
     * @return true on success
     */
    bool begin(int pin) {
        if (pin < 0) return false;
        _pin = pin;
        _attached = true;
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        return true;
    }

    /**
     * @brief Release the GPIO pin
     */
    void end() {
        if (_attached) {
            digitalWrite(_pin, LOW);
        }
        _pin = -1;
        _attached = false;
    }

    bool isAttached() const { return _attached; }
    int  pin() const        { return _pin; }

    /**
     * @brief Digital on/off (active-high; caller inverts for active-low)
     */
    void writeDigital(bool on) {
        if (!_attached) return;
        digitalWrite(_pin, on ? HIGH : LOW);
    }

    /**
     * @brief PWM duty cycle 0-255 (active-high; caller inverts for active-low)
     */
    void writePwm(uint8_t duty) {
        if (!_attached) return;
        analogWrite(_pin, duty);
    }

    /// Native GPIO always supports analogWrite PWM
    bool supportsPwm() const { return true; }

    /// No-op — GPIO output is immediate
    void update() {}

private:
    int  _pin = -1;
    bool _attached = false;
};

#endif // GPIO_LED_DRV_H
