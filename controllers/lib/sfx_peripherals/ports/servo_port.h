/*
 * servo_port.h — abstract servo port + native pin driver.
 *
 * A `ServoPort` is a hobby-servo pulse train: 50 Hz frame, 500-2500 µs
 * pulse width.  The port is the raw transport — motion profiling,
 * end-stop limits, REV flag, and the "moving / target-reached" state
 * machine all live in `ServoActuatorRole` (one layer above).
 *
 * Input mode is optional: a port that can also *sample* an incoming
 * pulse train (RC PWM capture) reports `ServoPortFlags::INPUT` in its
 * capability bits and supports `readMicroseconds(out)`.
 *
 * Driver shipped here:
 *   - `MicroservoPort` — Arduino `Servo` library on one MCU pin
 *                        (Pico: PIO; ESP32: LEDC; both via sfx_platform's
 *                        cross-platform `Servo` include).
 *
 * RC input capture and dedicated PWM-expander servos live in their own
 * headers when needed.
 */

#ifndef SFX_PERIPHERAL_SERVO_PORT_H
#define SFX_PERIPHERAL_SERVO_PORT_H

#include <Arduino.h>
#include <cstdint>

#include <platform/sfx_platform.h>   // pulls in <Servo.h> or <ESP32Servo.h>

namespace sfx_peripherals {

// ============================================================================
// ServoPort — abstract pulse output (+ optional input capture)
// ============================================================================

class ServoPort {
public:
    virtual ~ServoPort() = default;

    /// Hardware init.  Called once by the port registry.
    virtual bool begin() = 0;

    /// Write a pulse width in microseconds (typically 500..2500).  The
    /// driver clamps to its own safe range.
    virtual void writeMicroseconds(uint16_t us) = 0;

    /// Last commanded pulse width (no hardware round-trip).
    virtual uint16_t microseconds() const = 0;

    /// Inclusive driver-level pulse-width bounds.  Roles should respect
    /// these as a hard safety envelope; calibration limits sit above.
    virtual uint16_t minMicroseconds() const = 0;
    virtual uint16_t maxMicroseconds() const = 0;

    // ── Optional input mode ──────────────────────────────────────────

    /// True iff this port can sample an external PWM pulse.
    virtual bool supportsInput() const { return false; }

    /// Read the most recent measured pulse width (input mode).
    /// Returns true if a valid sample is available; on success
    /// writes the pulse width to `*outUs`.
    virtual bool readMicroseconds(uint16_t* /*outUs*/) { return false; }
};

// ============================================================================
// MicroservoPort — Arduino Servo on one MCU pin
// ============================================================================

class MicroservoPort final : public ServoPort {
public:
    /// @param gpioPin   Arduino pin number.
    /// @param minUs     Driver-level lower clamp (default 500 µs).
    /// @param maxUs     Driver-level upper clamp (default 2500 µs).
    /// @param initialUs Pulse to write on `begin()` (default 1500 µs centre).
    explicit MicroservoPort(int gpioPin,
                            uint16_t minUs = 500,
                            uint16_t maxUs = 2500,
                            uint16_t initialUs = 1500)
        : _pin(gpioPin), _minUs(minUs), _maxUs(maxUs), _us(initialUs) {}

    bool begin() override {
        if (_pin < 0) return false;
        _servo.attach(_pin, _minUs, _maxUs);
        writeMicroseconds(_us);
        return _servo.attached();
    }

    void writeMicroseconds(uint16_t us) override {
        if (us < _minUs) us = _minUs;
        if (us > _maxUs) us = _maxUs;
        _us = us;
        if (_servo.attached()) _servo.writeMicroseconds(us);
    }

    uint16_t microseconds()    const override { return _us; }
    uint16_t minMicroseconds() const override { return _minUs; }
    uint16_t maxMicroseconds() const override { return _maxUs; }

private:
    int      _pin;
    uint16_t _minUs;
    uint16_t _maxUs;
    uint16_t _us;
    Servo    _servo;
};

}  // namespace sfx_peripherals

#endif  // SFX_PERIPHERAL_SERVO_PORT_H
