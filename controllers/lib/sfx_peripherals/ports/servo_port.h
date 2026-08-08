/*
 * servo_port.h — abstract servo port + native pin driver.
 *
 * A `ServoPort` is a hobby-servo pulse train: 50 Hz frame, 500-2500 µs
 * pulse width.  **Output only** (Rule 31 — direction is fixed at
 * declaration; `InputPort` is the input-side kind).  The port is the
 * raw pulse output; motion profiling, end-stop limits, REV flag, and
 * the "moving / target-reached" state machine all live in
 * `ServoActuatorRole` one layer above.
 *
 * Driver shipped here:
 *   - `MicroservoPort` — native servo pulse train on one MCU pin via
 *                        `ServoDriver` (ESP32: MCPWM; Pico: Arduino-Pico PIO),
 *                        selected in sfx_servo.h.
 *
 * For pulse-stream inputs (RC PWM, PPM, SBUS, Jeti EX), use
 * `InputPort` from `input_port.h` instead.
 */

#ifndef SFX_PERIPHERAL_SERVO_PORT_H
#define SFX_PERIPHERAL_SERVO_PORT_H

#include <cstdint>
#include <type_traits>

#include "sfx_servo.h"   // sfx_peripherals::ServoDriver (MCPWM / PIO)

namespace sfx_peripherals {

// Driver capability probe: a backend that declares `kQuietAttach = true`
// (EspServo) emits NO pulse train until the first writeMicroseconds(), so
// begin() must not push the initial centre.  Backends without the marker
// (Arduino-Pico Servo) start pulsing on attach — begin() writes the initial
// value as before so the pulse at least matches the declared centre.
template <typename T, typename = void>
struct servo_quiet_attach : std::false_type {};
template <typename T>
struct servo_quiet_attach<T, std::void_t<decltype(T::kQuietAttach)>>
    : std::bool_constant<T::kQuietAttach> {};

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
};

// ============================================================================
// MicroservoPort — Arduino Servo on one MCU pin
// ============================================================================

class MicroservoPort final : public ServoPort {
public:
    /// @param gpioPin   Arduino pin number.
    /// @param minUs     Driver-level lower clamp (default 500 µs).
    /// @param maxUs     Driver-level upper clamp (default 2500 µs).
    /// @param initialUs Intended centre (default 1500 µs).  On a quiet-attach
    ///                  driver (ESP32) this is only the `microseconds()` seed —
    ///                  NO pulse is emitted until the first command, so a
    ///                  reboot can't drive an undercarriage mid-travel.  On
    ///                  drivers that pulse from attach (Pico) it is written
    ///                  on `begin()` as before.
    MicroservoPort(int gpioPin,
                   uint16_t minUs = 500,
                   uint16_t maxUs = 2500,
                   uint16_t initialUs = 1500)
        : _pin(gpioPin), _minUs(minUs), _maxUs(maxUs), _us(initialUs) {}

    bool begin() override {
        if (_pin < 0) return false;
        _servo.attach(_pin, _minUs, _maxUs);
        if constexpr (!servo_quiet_attach<ServoDriver>::value) {
            writeMicroseconds(_us);
        }
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
    int          _pin;
    uint16_t     _minUs;
    uint16_t     _maxUs;
    uint16_t     _us;
    ServoDriver  _servo;
};

}  // namespace sfx_peripherals

#endif  // SFX_PERIPHERAL_SERVO_PORT_H
