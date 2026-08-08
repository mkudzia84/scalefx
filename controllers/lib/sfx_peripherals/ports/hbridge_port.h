/*
 * hbridge_port.h — abstract bi-directional PWM port + two stock drivers.
 *
 * An `HBridgePort` is a "signed duty" output: positive values drive one
 * polarity, negative values the other, zero coasts or brakes (decided
 * by the driver's brake / coast methods).  The port doesn't know if it
 * powers a DC motor, a solenoid, or a heater in alternating polarity —
 * that's the role layer's concern.
 *
 * Two stock drivers ship in this header:
 *   - `DualPwmHBridgePort` — two PwmPort outputs.  Forward = drive
 *     pwmA high, pwmB low.  Reverse = pwmA low, pwmB high.  Brake =
 *     both high (short brake).  Coast = both low.
 *   - `PwmDirHBridgePort`  — one PwmPort (magnitude) + one GPIO
 *     direction pin.  Common for DRV8871-style single-channel drivers
 *     where direction is a logic input.  Brake = unsupported (would
 *     need both pins LOW which contradicts the dir-pin model).
 *
 * Drivers for dedicated H-bridge chips (DRV8833, TB6612FNG) live in
 * their own headers as needed.
 */

#ifndef SFX_PERIPHERAL_HBRIDGE_PORT_H
#define SFX_PERIPHERAL_HBRIDGE_PORT_H

#include <cstdint>
#include <cstdlib>     // abs()

#include "pwm_port.h"   // also pulls <gpio/native_gpio.h> (NativeGpio)

namespace sfx_peripherals {

// ============================================================================
// HBridgePort — abstract signed-duty bidirectional output
// ============================================================================

class HBridgePort {
public:
    virtual ~HBridgePort() = default;

    /// Hardware init.  Called once by the port registry.
    virtual bool begin() = 0;

    /// Set signed duty in [-maxDuty(), +maxDuty()].  Positive = forward,
    /// negative = reverse, zero = coast (default).  Drivers may choose
    /// different zero semantics — call `brake()` / `coast()` explicitly
    /// for unambiguous control.
    virtual void setSigned(int16_t signedDuty) = 0;

    /// Short brake — pulls both ends of the bridge to the same rail.
    /// Backends that can't brake should fall back to `coast()` and
    /// return false.
    virtual bool brake() = 0;

    /// Coast — high-Z / floating output, motor free-spins.
    virtual bool coast() = 0;

    /// Last commanded signed duty.
    virtual int16_t signedDuty() const = 0;

    /// Unsigned magnitude limit (matches the underlying PWM resolution).
    virtual uint16_t maxDuty() const = 0;

    /// Optional: set PWM frequency.  Affects both half-bridges.
    virtual bool setFrequencyHz(uint16_t /*hz*/) { return false; }
    virtual uint16_t frequencyHz() const { return 0; }
};

// ============================================================================
// DualPwmHBridgePort — two PwmPort half-bridges (forward / reverse)
// ============================================================================

class DualPwmHBridgePort final : public HBridgePort {
public:
    /// @param pwmA  PWM output driving the "A" side of the bridge.
    /// @param pwmB  PWM output driving the "B" side.
    DualPwmHBridgePort(PwmPort& pwmA, PwmPort& pwmB)
        : _a(&pwmA), _b(&pwmB) {}

    bool begin() override {
        // Begin both half-bridges.  Idempotent for ports that are ALSO
        // registered separately (e.g. PCA9685 channels declared in
        // kPwmPorts) — begin() just re-zeros them.  Essential for
        // native-GPIO PWM pins that are NOT separately registered (the
        // common motor-driver case): without this their NativeGpio pin
        // setup / first channel bind never runs.
        bool ok = true;
        if (_a) ok = _a->begin() && ok;
        if (_b) ok = _b->begin() && ok;
        coast();
        return ok;
    }

    void setSigned(int16_t signedDuty) override {
        const uint16_t maxAB = effectiveMax();
        if (signedDuty > (int16_t)maxAB)  signedDuty =  (int16_t)maxAB;
        if (signedDuty < -(int16_t)maxAB) signedDuty = -(int16_t)maxAB;
        _signed = signedDuty;
        // SLOW-DECAY (drive/brake) PWM, both directions: the active side
        // stays HIGH and the complementary side chops LOW(drive)↔HIGH
        // (brake) — motor current recirculates through the bridge during
        // the off-phase and stays CONTINUOUS (fast decay's drive/coast
        // collapsed the current every cycle at partial duty: jerky motion,
        // mushy torque; bench 2026-08-08).  Both-high = brake is this
        // hardware's contract — see brake().
        //
        // NOTE (ISSUES.md §7): an IN-LINE INA226 cannot read the leg it
        // sits on while that leg's common-mode rides HIGH (reverse drive
        // above ~8 V rail reads ~0 µV with amps flowing).  That is a
        // SENSOR limitation, not a decay-mode choice — chopping the shunt
        // leg (fast decay) was tried and is equally blind.  Keep the motor
        // supply ≤ ~2S/3S on boards with in-line shunts, or fix in the
        // board rev (low-side shunt / INA240).
        if (signedDuty > 0) {
            _a->setDuty(maxAB);
            _b->setDuty((uint16_t)(maxAB - (uint16_t)signedDuty));
        } else if (signedDuty < 0) {
            _a->setDuty((uint16_t)(maxAB - (uint16_t)(-signedDuty)));
            _b->setDuty(maxAB);
        } else {
            coast();
        }
    }

    bool brake() override {
        const uint16_t m = effectiveMax();
        _a->setDuty(m);
        _b->setDuty(m);
        _signed = 0;
        return true;
    }

    bool coast() override {
        _a->setDuty(0);
        _b->setDuty(0);
        _signed = 0;
        return true;
    }

    int16_t  signedDuty() const override { return _signed; }
    uint16_t maxDuty()    const override { return effectiveMax(); }

    bool setFrequencyHz(uint16_t hz) override {
        const bool ok = _a->setFrequencyHz(hz);
        _b->setFrequencyHz(hz);
        return ok;
    }
    uint16_t frequencyHz() const override { return _a->frequencyHz(); }

private:
    uint16_t effectiveMax() const {
        const uint16_t ma = _a->maxDuty();
        const uint16_t mb = _b->maxDuty();
        return (ma < mb) ? ma : mb;
    }

    PwmPort* _a;
    PwmPort* _b;
    int16_t  _signed = 0;
};

// ============================================================================
// PwmDirHBridgePort — one PwmPort magnitude + one GPIO direction pin
// ============================================================================

class PwmDirHBridgePort final : public HBridgePort {
public:
    /// @param pwm     PWM output controlling magnitude.
    /// @param dirPin  GPIO controlling direction (HIGH = forward, LOW = reverse).
    /// @param invertDir If true, swap the direction-pin polarity.
    PwmDirHBridgePort(PwmPort& pwm, int dirPin, bool invertDir = false)
        : _pwm(&pwm), _dirPin(dirPin), _invertDir(invertDir) {}

    bool begin() override {
        if (_dirPin < 0) return false;
        NativeGpio::instance().setPinDirection(_dirPin, /*isInput=*/false);
        coast();
        return true;
    }

    void setSigned(int16_t signedDuty) override {
        const int16_t maxAB = (int16_t)_pwm->maxDuty();
        if (signedDuty >  maxAB) signedDuty =  maxAB;
        if (signedDuty < -maxAB) signedDuty = -maxAB;
        _signed = signedDuty;
        const bool forward = (signedDuty >= 0);
        const bool pinHigh = forward ^ _invertDir;
        NativeGpio::instance().writePin(_dirPin, pinHigh);
        _pwm->setDuty((uint16_t)std::abs((int)signedDuty));
    }

    bool brake() override {
        // No short brake with this topology — fall back to coast.
        return coast();
    }

    bool coast() override {
        _pwm->setDuty(0);
        _signed = 0;
        return true;
    }

    int16_t  signedDuty() const override { return _signed; }
    uint16_t maxDuty()    const override { return _pwm->maxDuty(); }

    bool setFrequencyHz(uint16_t hz) override { return _pwm->setFrequencyHz(hz); }
    uint16_t frequencyHz() const override     { return _pwm->frequencyHz(); }

private:
    PwmPort* _pwm;
    int      _dirPin;
    bool     _invertDir;
    int16_t  _signed = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_PERIPHERAL_HBRIDGE_PORT_H
