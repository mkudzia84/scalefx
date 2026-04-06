/*
 * Hardware PWM LED Driver — Per-pin driver for HW-PWM-capable GPIO providers
 *
 * Provides per-channel LED brightness control using hardware PWM built into
 * the GPIO provider (I2C expander or native MCU GPIO).  Unlike the software
 * BAM driver (ExpanderLedDriverT), this driver delegates PWM generation
 * entirely to the hardware — no time-sliced update loop required.
 *
 * Requirements on TGpio:
 *   - HAS_HW_PWM = true
 *   - bool setPinDirection(uint8_t pin, bool isInput)
 *   - bool setLedMode(uint8_t pin, bool ledMode)
 *   - bool setLedBrightness(uint8_t pin, uint8_t brightness)  // 0-255
 *   - bool writePin(uint8_t pin, bool high)                    // for digital fallback
 *   - bool isAvailable() const
 *
 * Matching GPIO providers:
 *   - AW9523B:    I2C expander with 256-step constant-current LED drivers (~430 Hz)
 *   - NativeGpio: MCU GPIO with analogWrite (RP2040 HW PWM / ESP32 LEDC)
 *
 * Satisfies the LedDriver policy expected by LedControlT<TDriver>:
 *   begin, end, isAttached, pin, writeDigital, writePwm, supportsPwm, update
 *
 * Usage with AW9523B:
 *   AW9523B expander;
 *   expander.begin(Wire, 0x58);
 *
 *   HwPwmLedDriverT<AW9523B> drv;
 *   drv.begin(&expander, 0);      // P0_0 → LED mode, output, full PWM control
 *   drv.writePwm(128);            // → expander.setLedBrightness(0, 128)
 *
 * Usage with NativeGpio:
 *   NativeGpio gpio;
 *   gpio.begin();
 *
 *   HwPwmLedDriverT<NativeGpio> drv;
 *   drv.begin(&gpio, 13);         // GPIO 13 → output, PWM via analogWrite
 *   drv.writePwm(200);            // → analogWrite(13, 200)
 *
 * Default aliases (bottom of file):
 *   HwPwmLedDriver   = HwPwmLedDriverT<AW9523B>     — I2C expander HW PWM
 *   NativePwmDriver  = HwPwmLedDriverT<NativeGpio>   — Native GPIO PWM
 */

#ifndef HW_PWM_LED_DRV_H
#define HW_PWM_LED_DRV_H

#include <Arduino.h>
#include "../gpio/gpio_expander.h"

// ============================================================================
// HwPwmLedDriverT — Per-pin hardware PWM driver
// ============================================================================

/**
 * @brief LED driver for a single pin on a hardware-PWM-capable GPIO provider.
 *
 * @tparam TGpio GPIO provider type. Must satisfy the HW PWM portion of the
 *   GpioExpander concept: isAvailable(), setPinDirection(), setLedMode(),
 *   setLedBrightness(), writePin().
 *
 * Each instance binds to one pin. Multiple drivers can share the same
 * GPIO provider instance (e.g., 6 drivers on one AW9523B).
 *
 * Static assertion ensures the GPIO provider actually supports HW PWM.
 */
template <typename TGpio>
class HwPwmLedDriverT {
    static_assert(TGpio::HAS_HW_PWM,
        "HwPwmLedDriverT requires a GPIO provider with HAS_HW_PWM = true. "
        "For software BAM on GPIO-only expanders, use ExpanderLedDriverT instead.");

public:
    HwPwmLedDriverT() = default;

    /**
     * @brief Bind to a pin on the GPIO provider
     * @param gpio Pointer to initialized GPIO provider (must outlive driver)
     * @param pin Pin number (0-based; meaning depends on provider)
     * @return true on success
     *
     * Configures the pin as: output, LED mode (hardware PWM), brightness = 0.
     */
    bool begin(TGpio* gpio, uint8_t pin) {
        if (!gpio || !gpio->isAvailable()) return false;

        _gpio = gpio;
        _pin = pin;

        // Configure pin: output + LED mode
        _gpio->setPinDirection(pin, false);  // Output
        _gpio->setLedMode(pin, true);        // Enable HW PWM
        _gpio->setLedBrightness(pin, 0);     // Start off

        _attached = true;
        return true;
    }

    /**
     * @brief Release the pin (set brightness to 0)
     */
    void end() {
        if (_attached && _gpio) {
            _gpio->setLedBrightness(_pin, 0);
        }
        _gpio = nullptr;
        _pin = 0;
        _attached = false;
    }

    bool isAttached() const { return _attached; }
    int  pin() const        { return _attached ? static_cast<int>(_pin) : -1; }

    /**
     * @brief Digital on/off (full brightness or off)
     *
     * Uses setLedBrightness for clean transitions rather than switching to
     * GPIO mode. Active-high; caller (LedControlT) handles polarity inversion.
     */
    void writeDigital(bool on) {
        if (!_attached) return;
        _gpio->setLedBrightness(_pin, on ? 255 : 0);
    }

    /**
     * @brief Set PWM duty cycle 0-255
     *
     * Directly passed to the GPIO provider's hardware PWM (no software LUT
     * needed — hardware provides smooth 256-step dimming).
     */
    void writePwm(uint8_t duty) {
        if (!_attached) return;
        _gpio->setLedBrightness(_pin, duty);
    }

    /// Hardware PWM is always available on HAS_HW_PWM providers
    bool supportsPwm() const { return true; }

    /// No-op — hardware PWM runs autonomously, no tick needed
    void update() {}

private:
    TGpio*  _gpio = nullptr;
    uint8_t _pin = 0;
    bool    _attached = false;
};

// ============================================================================
// Default aliases
// ============================================================================

// Forward declarations — include the specific headers to use these aliases
class AW9523B;
class NativeGpio;

/// Hardware PWM driver on AW9523B I2C expander (~430 Hz constant-current)
using HwPwmLedDriver = HwPwmLedDriverT<AW9523B>;

/// Hardware PWM driver on native MCU GPIO (analogWrite)
using NativePwmDriver = HwPwmLedDriverT<NativeGpio>;

#endif // HW_PWM_LED_DRV_H
