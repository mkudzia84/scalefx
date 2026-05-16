/*
 * DcMotor — generic H-bridge / single-pin DC motor driver.
 *
 * Wraps the three common topologies for driving a brushed DC motor:
 *
 *   1. SinglePinPwm        — one PWM-capable pin; direction is fixed
 *                            by the wiring.  Useful for one-way
 *                            actuators (smoke fan, heater pump).
 *
 *   2. HBridgeDualGpio     — two digital GPIO pins on opposite halves
 *                            of an H-bridge.  Direction is the pair's
 *                            logic state; speed control happens on
 *                            the bridge IC's enable pin (not modelled
 *                            here — set it to a fixed PWM externally,
 *                            or use HBridgePwmDir below).
 *
 *   3. HBridgePwmDir       — one PWM-capable pin (speed) plus one
 *                            digital pin (direction).  The most
 *                            common configuration for a typical
 *                            driver IC like DRV8871 / TB6612 / L298.
 *                            Speed = duty on the PWM pin; direction
 *                            = digital level on the dir pin.
 *
 * Speed is signed thousandths (-1000..+1000).  Sign chooses direction
 * (where supported); magnitude maps to PWM duty via `analogWrite()`
 * for now (placeholder for a future typed PwmOutput driver).
 *
 * Brake support: `HBridgeDualGpio` brakes by driving both halves LOW
 * (motor terminals shorted to GND, dynamic braking).  Other
 * topologies fall back to coast.  Pass `brake=true` to setSpeed(0)
 * to request brake; the driver short-circuits this internally to
 * coast on topologies that can't.
 *
 * The driver is a pure peripheral wrapper — no stall detection, no
 * calibration, no protocol awareness.  Combine it with
 * `StallDetector` (this directory) for current-spike detection and
 * with the slave-protocol layer for higher-level policy.
 */

#ifndef SFX_DC_MOTOR_H
#define SFX_DC_MOTOR_H

#include <Arduino.h>
#include <cstdint>

namespace sfx_peripherals {

enum class MotorDir : int8_t {
    Reverse = -1,
    Stop    =  0,
    Forward =  1,
};

enum class MotorTopology : uint8_t {
    SinglePinPwm     = 0,   ///< one PWM pin; direction fixed by wiring
    HBridgeDualGpio  = 1,   ///< two digital pins; speed via separate enable
    HBridgePwmDir    = 2,   ///< one PWM (speed) + one digital (direction)
};

struct DcMotorConfig {
    MotorTopology topology;
    uint8_t       pwm_pin;       ///< SinglePinPwm + HBridgePwmDir
    uint8_t       cw_pin;        ///< HBridgeDualGpio  (and HBridgePwmDir's `dir_pin`)
    uint8_t       ccw_pin;       ///< HBridgeDualGpio only
    bool          invert_dir;    ///< swap forward / reverse semantically
    bool          brake_capable; ///< if HBridgeDualGpio: support brake-on-stop
};

class DcMotor {
public:
    DcMotor() = default;

    bool begin(const DcMotorConfig& cfg) {
        _cfg = cfg;
        switch (cfg.topology) {
            case MotorTopology::SinglePinPwm:
                pinMode(cfg.pwm_pin, OUTPUT);
                analogWrite(cfg.pwm_pin, 0);
                break;
            case MotorTopology::HBridgeDualGpio:
                pinMode(cfg.cw_pin,  OUTPUT);
                pinMode(cfg.ccw_pin, OUTPUT);
                digitalWrite(cfg.cw_pin,  LOW);
                digitalWrite(cfg.ccw_pin, LOW);
                break;
            case MotorTopology::HBridgePwmDir:
                pinMode(cfg.pwm_pin, OUTPUT);
                pinMode(cfg.cw_pin,  OUTPUT);   // dir pin (we re-use cw_pin slot)
                analogWrite(cfg.pwm_pin, 0);
                digitalWrite(cfg.cw_pin, LOW);
                break;
        }
        _attached = true;
        _speed = 0;
        return true;
    }

    void end() {
        if (!_attached) return;
        setSpeed(0, /*brake=*/false);
        _attached = false;
    }

    /// Drive the motor.  speed: -1000..+1000 (signed thousandths).
    /// Returns the actual speed applied (clamped if out of range).
    int16_t setSpeed(int16_t speed_thousandths, bool brake = false) {
        if (!_attached) return 0;
        if (speed_thousandths >  1000) speed_thousandths =  1000;
        if (speed_thousandths < -1000) speed_thousandths = -1000;
        _speed = speed_thousandths;

        if (_cfg.invert_dir) speed_thousandths = -speed_thousandths;

        const int16_t mag = (speed_thousandths < 0) ? -speed_thousandths : speed_thousandths;
        const uint8_t pwm8 = (uint8_t)((uint32_t)mag * 255u / 1000u);
        const MotorDir dir = (speed_thousandths > 0) ? MotorDir::Forward
                          : (speed_thousandths < 0) ? MotorDir::Reverse
                          : MotorDir::Stop;

        switch (_cfg.topology) {
            case MotorTopology::SinglePinPwm:
                // Direction is fixed by wiring — sign is informational
                // only, magnitude controls duty.
                analogWrite(_cfg.pwm_pin, pwm8);
                break;

            case MotorTopology::HBridgeDualGpio:
                // Brake = both halves LOW (capable bridges only).  Coast
                // = also both halves LOW for non-brake-capable bridges
                // — same gpio result, different mechanical behaviour
                // depending on the bridge IC.
                if (dir == MotorDir::Forward) {
                    digitalWrite(_cfg.ccw_pin, LOW);
                    digitalWrite(_cfg.cw_pin,  HIGH);
                } else if (dir == MotorDir::Reverse) {
                    digitalWrite(_cfg.cw_pin,  LOW);
                    digitalWrite(_cfg.ccw_pin, HIGH);
                } else {
                    // Stop — brake or coast based on capability + caller.
                    if (brake && _cfg.brake_capable) {
                        digitalWrite(_cfg.cw_pin,  LOW);
                        digitalWrite(_cfg.ccw_pin, LOW);
                    } else {
                        digitalWrite(_cfg.cw_pin,  LOW);
                        digitalWrite(_cfg.ccw_pin, LOW);
                    }
                }
                break;

            case MotorTopology::HBridgePwmDir:
                digitalWrite(_cfg.cw_pin, dir == MotorDir::Forward ? HIGH : LOW);
                analogWrite(_cfg.pwm_pin, dir == MotorDir::Stop ? 0 : pwm8);
                break;
        }
        return _speed;
    }

    void   stop()                { setSpeed(0, false); }
    void   brake()               { setSpeed(0, true);  }
    int16_t currentSpeed() const { return _speed; }
    bool    isAttached()   const { return _attached; }

    const DcMotorConfig& config() const { return _cfg; }

private:
    DcMotorConfig _cfg{};
    int16_t       _speed    = 0;
    bool          _attached = false;
};

}  // namespace sfx_peripherals

#endif  // SFX_DC_MOTOR_H
