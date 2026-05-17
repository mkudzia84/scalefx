# sfx_peripherals/motor — DC motor primitives

> **Status:** `dc_motor.h` and `stall_detector.h` landed 2026-05-06.
> `stall_calibrator.h` is a placeholder; the production calibrator
> stays at `controllers/gearcontrol/pico/src/stall_calibrator.{h,cpp}`
> until the GearControl slave migration (Step 2 of
> [`instructions/15-GENERIC-SLAVE-REFACTOR.md`](../../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)),
> when the gear-specific `CalibPhase` / `GearControlCalibStatus` /
> `DoorSequencer*` couplings can be cleanly replaced.

## Files

| File | Source | Status |
|---|---|---|
| `dc_motor.h`            | new — extracted from `LandingGear::setMotor()` | **landed** |
| `stall_detector.h`      | promoted from `controllers/gearcontrol/pico/src/stall_detector.h` | **landed** |
| `stall_calibrator.h`    | placeholder; original stays in gearcontrol until Step 2 of the slave migration | **deferred** |

## DcMotor — the missing primitive (now landed)

Generic H-bridge / single-pin DC motor driver in
[`dc_motor.h`](dc_motor.h).  Three topologies under one `setSpeed()`
API:

```cpp
namespace sfx_peripherals {

enum class MotorDir : int8_t { Reverse = -1, Stop = 0, Forward = 1 };

/// Single-pin (uni-directional, PWM-driven) and dual-pin (H-bridge,
/// digital direction) topologies via a tagged config.
struct DcMotorConfig {
    enum class Topology : uint8_t {
        SinglePinPwm,      // pwm pin only — direction immutable
        HBridgeDualGpio,   // cw_pin + ccw_pin — digital, no PWM
        HBridgePwmDir,     // pwm_pin + dir_pin — PWM + digital direction
    };
    Topology topology;
    uint8_t  pwm_pin;
    uint8_t  cw_pin;       // HBridgeDualGpio only
    uint8_t  ccw_pin;      // HBridgeDualGpio / HBridgePwmDir
    bool     brake_capable;
};

class DcMotor {
public:
    bool begin(const DcMotorConfig& cfg);
    void end();

    /// Drive the motor.  speed: -1000..+1000 (signed thousandths).
    /// Sign chooses direction (where supported); magnitude → PWM duty.
    /// On supported topologies, speed=0 is "coast" by default; pass
    /// brake=true to short-brake the H-bridge instead.
    void setSpeed(int16_t speed_thousandths, bool brake = false);

    void   stop()              { setSpeed(0, false); }
    void   brake()              { setSpeed(0, true);  }
    int16_t currentSpeed() const { return _speed; }

private:
    DcMotorConfig _cfg{};
    int16_t       _speed = 0;
};

}  // namespace sfx_peripherals
```

This is the unit `PwmCollection` adopts when a channel goes into
`PwmMotor` mode — instead of `PwmCollection` knowing about H-bridges.

## StallDetector — promoted (landed)

Now in [`stall_detector.h`](stall_detector.h) under the
`sfx_peripherals` namespace.  The original at
`controllers/gearcontrol/pico/src/stall_detector.{h,cpp}` keeps
working unchanged for the brief overlap until the GearControl slave
migration (Step 2) deletes it; the new copy is the canonical one
going forward.

PwmCollection (in `sfx_peripherals/collections/`) now owns one `StallDetector`
per channel and delegates the entire detection state machine to it
— see `pwm_collection.ipp::update()`.



`controllers/gearcontrol/pico/src/stall_detector.h` is a pure state
machine — no GPIOs, no driver dependency, just `update(uint16_t
current_mA)` calls.  It exposes:

```cpp
struct StallDetector::Config {
    uint32_t startupIgnore_ms        = 500;
    uint16_t motorDetectThreshold_mA = 100;
    uint16_t stallThreshold_mA       = 1500;
    uint32_t stallConfirm_ms         = 200;
    uint32_t timeout_ms              = 10000;
};

enum class StallDetector::Result {
    RUNNING, STALL_CONFIRMED, NO_MOTOR, TIMEOUT_STALL, TIMEOUT_ERROR
};
```

Configurable startup-inrush ignore, sustained-confirmation, motor-
presence detection, timeout — all the production-grade detail
GearControl needed.  PwmCollection's stall guard is now wired to
**delegate to one of these per channel** rather than re-implement
the comparison + debounce loop.

## StallCalibrator — already generic

`controllers/gearcontrol/pico/src/stall_calibrator.h` runs the
multi-phase calibration sequence (CLEAR_RUN → DEPLOY_RUN →
RETRACT_RUN → compute threshold with safety margin).  Output:

```cpp
struct StallCalibrator::Result {
    uint16_t stallThreshold_mA;
    uint16_t baseline_mA;
    uint16_t peakDeploy_mA;
    uint16_t peakRetract_mA;
    uint8_t  errorReason;
};
```

The 80%-of-peak safety margin, baseline averaging window, and
inrush-ignore phase are encoded constants (`CalibConfig` namespace) —
already production-tuned for ESP32-S3 / Pico timing.

The only gear-specific dependency is a `DoorSequencer*` pointer used
to coordinate door open/close around the calibration motor runs.
That gets generalised to a `std::function` callback (`onPhaseTransition(Phase)`)
during the move so the calibrator works on any board, not just one
with doors.

## When the move happens

- **GearControl slave migration PR** (Step 2 in the refactor plan)
  is the natural carrier — the old `LandingGear` aggregate dissolves
  into the master-side orchestrator, and the three generic motor
  primitives surface here in `sfx_peripherals/motor/`.
- The new `PwmCollection` (in `sfx_peripherals/collections/`, soon to be
  `sfx_peripherals/collections/`) gets re-templatized on these
  primitives at the same time.
- The master-side `effects/gearcontrol/` orchestrator drives the
  calibration via the existing `PWM_SET_STALL_GUARD` /
  `PWM_CLEAR_STALL` / `PWM_STALL` packets — calibration **state**
  lives master-side per the architectural pivot.

## Cross-references

- [`instructions/15-GENERIC-SLAVE-REFACTOR.md`](../../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)
  § "Motor stall detection + endpoint calibration" — the master-
  side calibration workflow + protocol packets
- [`controllers/gearcontrol/pico/src/stall_detector.h`](../../../../gearcontrol/pico/src/stall_detector.h)
  — the production-tested state machine source
- [`controllers/gearcontrol/pico/src/stall_calibrator.h`](../../../../gearcontrol/pico/src/stall_calibrator.h)
  — the calibration sequencer source
