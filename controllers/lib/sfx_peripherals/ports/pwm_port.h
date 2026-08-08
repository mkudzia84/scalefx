/*
 * pwm_port.h — abstract PWM port + the two stock drivers.
 *
 * A `PwmPort` is a write-only PWM output: caller asks for a duty value
 * in [0, maxDuty()] and the port translates to whatever the underlying
 * hardware needs (analogWrite on a GPIO, PCA9685 register write, …).
 *
 * The port doesn't know what's running on top.  An LED animator, a
 * heater bang-bang loop, or a DC-motor open-loop ramp all just call
 * `setDuty()` against this abstract surface.
 *
 * Two stock drivers ship in this header:
 *   - `NativePwmPort`    — Arduino `analogWrite()` on a single MCU pin
 *   - `Pca9685PwmPort`   — one channel of a NXP PCA9685 I²C PWM chip
 *
 * Additional drivers (DRV8847 dual-PWM driver, TLC5947 24-ch LED driver,
 * dedicated PWM expander chips, …) implement the same interface in
 * their own header.
 */

#ifndef SFX_PERIPHERAL_PWM_PORT_H
#define SFX_PERIPHERAL_PWM_PORT_H

#include <cstdint>

#include <gpio/native_gpio.h>   // NativeGpio — native MCU PWM (P6)
#include "pwm/pca9685.h"        // pre-existing chip driver

namespace sfx_peripherals {

// ============================================================================
// PwmPort — abstract write-only PWM output
// ============================================================================

class PwmPort {
public:
    virtual ~PwmPort() = default;

    /// One-shot hardware init (called once by the port registry / board).
    /// Returns true on success; a failed begin() leaves the port unusable
    /// but does not crash the firmware.
    virtual bool begin() = 0;

    /// Write a duty value in [0, maxDuty()].  Values above maxDuty() are
    /// clamped silently.
    virtual void setDuty(uint16_t duty) = 0;

    /// Read back the last commanded duty (no hardware round-trip).
    virtual uint16_t duty() const = 0;

    /// Inclusive upper bound on `setDuty()` values.  8-bit GPIO PWM
    /// returns 255; PCA9685 returns 4095; etc.  Roles can use this to
    /// scale brightness / target values into the port's native range.
    virtual uint16_t maxDuty() const = 0;

    /// Optional: set the PWM frequency in Hz.  Backends that don't
    /// support runtime frequency changes return false.  Default returns
    /// false (no-op).
    virtual bool setFrequencyHz(uint16_t /*hz*/) { return false; }

    /// Current PWM frequency (0 = unknown / not applicable).
    virtual uint16_t frequencyHz() const { return 0; }
};

// ============================================================================
// NativePwmPort — MCU pin via native PWM (NativeGpio: ESP32 LEDC / Pico PWM)
// ============================================================================

class NativePwmPort final : public PwmPort {
public:
    /// @param gpioPin       GPIO number.
    /// @param inverted      If true, hardware is wired such that
    ///                      logic-high = off (e.g. P-channel MOSFET side
    ///                      driver).  The duty is inverted on write.
    /// @param resolutionBits Role-facing duty resolution (default 8 = 0..255).
    ///                      Reported via maxDuty(); the value is scaled to the
    ///                      native 8-bit LEDC/PWM channel on write.
    explicit NativePwmPort(int gpioPin, bool inverted = false, uint8_t resolutionBits = 8)
        : _pin(gpioPin), _inverted(inverted), _resBits(resolutionBits) {
        _maxDuty = (resolutionBits >= 16) ? 0xFFFF : ((1u << resolutionBits) - 1u);
    }

    bool begin() override {
        if (_pin < 0) return false;
        NativeGpio::instance().setPinDirection(_pin, /*isInput=*/false);
        setDuty(0);
        return true;
    }

    void setDuty(uint16_t duty) override {
        if (_pin < 0) return;
        if (duty > _maxDuty) duty = _maxDuty;
        _duty = duty;
        const uint16_t out = _inverted ? (uint16_t)(_maxDuty - duty) : duty;
        // Scale the role-facing duty into the native 8-bit PWM channel.
        const uint8_t b = _maxDuty ? (uint8_t)(((uint32_t)out * 255u) / _maxDuty) : 0;
        NativeGpio::instance().setLedBrightness(_pin, b);
    }

    uint16_t duty() const     override { return _duty; }
    uint16_t maxDuty() const  override { return _maxDuty; }

    /// Carrier frequency for this pin's PWM (platform-dependent; the Pico
    /// implements it per slice, ESP32 LEDC returns false).  Motor drivers
    /// MUST set an in-spec carrier (~20 kHz): the platform default is an
    /// LED-oriented clk_sys/256 ≈ 490 kHz free-run that H-bridge ICs cannot
    /// switch cleanly at partial duty (whine + mushy torque).
    bool setFrequencyHz(uint16_t hz) override {
        if (!NativeGpio::instance().setPinPwmFrequencyHz((uint8_t)_pin, hz)) return false;
        _freqHz = hz;
        return true;
    }
    uint16_t frequencyHz() const override { return _freqHz; }

private:
    int      _pin;
    bool     _inverted;
    uint8_t  _resBits;
    uint16_t _maxDuty = 255;
    uint16_t _duty    = 0;
    uint16_t _freqHz  = 0;
};

// ============================================================================
// Pca9685PwmPort — one channel of a PCA9685 I²C PWM chip
// ============================================================================
//
// The shared PCA9685 driver instance is initialised at the board level
// (`chip.begin(wire, addr, freqHz)`).  Each Pca9685PwmPort just claims
// one channel on that already-initialised chip.

class Pca9685PwmPort final : public PwmPort {
public:
    /// @param chip     PCA9685 driver instance (shared across channels;
    ///                 caller is responsible for `chip.begin(...)` once).
    /// @param channel  Channel 0..15 on the chip.
    /// @param inverted If true, output is inverted (active-low LED, …).
    Pca9685PwmPort(PCA9685& chip, uint8_t channel, bool inverted = false)
        : _chip(&chip), _channel(channel), _inverted(inverted) {}

    bool begin() override {
        if (!_chip) return false;
        setDuty(0);
        return _chip->isAvailable();
    }

    void setDuty(uint16_t duty) override {
        if (!_chip) return;
        if (duty > kMaxDuty) duty = kMaxDuty;
        _duty = duty;
        const uint16_t out = _inverted ? (uint16_t)(kMaxDuty - duty) : duty;
        _chip->setChannel(_channel, out);
    }

    uint16_t duty()    const override { return _duty; }
    uint16_t maxDuty() const override { return kMaxDuty; }

    bool setFrequencyHz(uint16_t hz) override {
        if (!_chip) return false;
        _chip->setFrequency(hz);   // chip-wide — affects every channel
        return true;
    }
    uint16_t frequencyHz() const override {
        return _chip ? _chip->frequency() : 0;
    }

private:
    static constexpr uint16_t kMaxDuty = PCA9685::DUTY_MAX;   ///< 12-bit
    PCA9685* _chip;
    uint8_t  _channel;
    bool     _inverted;
    uint16_t _duty = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_PERIPHERAL_PWM_PORT_H
