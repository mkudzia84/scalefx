# GearControl Pico — generic-expander board

> Thin port + role expander for retractable landing gear (RP2040).

## Overview

GearControl is a **dumb expander** in the ScaleFX generic-expander model:
it exposes its physical ports over USB CDC and lets the **HubFX master**
attach roles and drive them.  All gear / door sequencing, stall-based
endpoint detection, calibration, status LEDs, and error handling live in
the hub's `GearControlServicePolicy` — **not on this board**.  (This
replaced ~4 000 lines of on-board sequencer code; see
[instructions/15-GENERIC-EXPANDER-REFACTOR.md](../../../instructions/15-GENERIC-EXPANDER-REFACTOR.md).)

**Hardware:** Raspberry Pi Pico (RP2040), earlephilhower core
**Protocol:** Binary COBS / CRC-8 over USB CDC (6 Mbps)
**Framework:** `BoardOf<GearControlBoard>` — auto Board + Indicator +
Port + Role service policies; zero user policies.

## Ports exposed

| Kind | Count | GPIO | Role (attached by hub) |
|---|---|---|---|
| Servo   | 7 | GP1,2,3,6,7,8,9 | `ServoActuator` (door + yaw) |
| HBridge | 3 | GP15/16, 17/18, 19/20 | `BiDcMotor` (gear motors, fwd/rev) + INA226 current sense → `MOTOR_STALL_EVENT` |

The 6 small per-motor status LEDs (GP21..26) are **not** ports — they're
plain GPIO indicators driven locally (see below).  Indicator LEDs (GP13
blue = connection, GP14 yellow = error) are driven by the auto
`IndicatorServicePolicy`.  **No input port** — the legacy RC PWM
deploy/retract input on GP0 is retired (the hub commands gear state).

`/gearcontrol.yaml` on the **hub** maps each gear to this board's ports
by GUID — see [media/presets/gearcontrol/](../../../media/presets/gearcontrol/).
Cross-referenced against [instructions/schematics/gearcontrol.tel](../../../instructions/schematics/gearcontrol.tel).

## Status-LED behaviour (local — hardwired to H-bridge state)

The 6 small per-motor status LEDs are driven entirely on-board by
`GearStatusLeds`, purely from the local H-bridge drive state — the hub
does **not** command them.  Each motor has a CW (forward/deploy) and CCW
(reverse/retract) LED:

| H-bridge state | CW LED | CCW LED |
|---|---|---|
| forward (signed duty > 0) | blink @250 ms | off |
| reverse (signed duty < 0) | off | blink @250 ms |
| idle (duty == 0)          | solid if last drove forward | solid if last drove reverse |
| fault | blink fast @100 ms | blink fast @100 ms |

"Fault" = an over-current event (|I| > 9 A, a short/jam) **or** the
BiDcMotor role's latched endstop-seek *timeout* (read from the registry).
Both are detected on-board, so the error indication is local — no hub
push needed.  Position ("deployed/retracted") shows as the last-direction
LED held solid; an active move blinks the matching direction LED.

## Endstop seek (autonomous, on-board)

Gear motion uses the BiDcMotor role's `BIMOTOR_SEEK_ENDSTOP` primitive:
the hub sends one seek per leg (`signed_duty`, optional `timeout_ms`)
and the **expander** drives → detects the stall (= endstop) → **brakes
locally**, then reports `BIMOTOR_ENDSTOP_RESULT` (reached / timeout /
aborted).  The drive→sense→stop loop never crosses the wire, so the
motor never hammers the endstop waiting for a round-trip.  A seek with
no stall before `timeout_ms` brakes and latches a timeout fault (LEDs
blink); `timeout_ms == 0` means no timeout.  Any brake/coast/set-signed
aborts an in-progress seek.  The hub's gear FSM maps results: deploy/
retract → deployed/retracted (or ERROR on timeout); calibration sweeps
retract→deploy→home, one seek per leg.

## Wire surface (hub-side `GearControlService`)

The board itself only speaks the generic-expander port/role surface
(`0x10..0x7F`).  The gear EFFECT commands below are handled by the hub:

| Packet | Payload | Effect |
|---|---|---|
| `GEAR_DEPLOY` 0xBE | `[id]` | lower gear |
| `GEAR_RETRACT` 0xBF | `[id]` | raise gear |
| `GEAR_STOP` 0xC0 | `[id]` | brake (does NOT clear error) |
| `GEAR_ALL` 0xC1 | `[action]` | stop/deploy/retract all |
| `GEAR_STATUS_REQ` 0xC2 | — | → phase per gear (idle/moving/calibrating/error) |
| `GEAR_LIST_REQ` 0xC5 | — | → configured gears |
| `GEAR_RESET` 0xC7 | `[id]` | clear ERROR → retracted |
| `GEAR_CALIBRATE` 0xC8 | `[id]` | stall-endpoint sweep (retract→deploy→home) |
| `GEAR_CALIB_CANCEL` 0xC9 | `[id]` | abort calibration |

Phases: `unconfigured, retracted, deploying, deployed, retracting, error,
calibrating`.  Errors: `MOTOR_UNAVAILABLE, IN_ERROR_STATE, TIMEOUT,
NO_STALL_DETECTED`.

CLI: `gear-list`, `gear-status`, `gear-deploy <id>`, `gear-retract <id>`,
`gear-stop <id>`, `gear-all <…>`, `gear-reset <id>`,
`gear-calibrate <id>`, `gear-calib-cancel <id>`.

## Calibration

`GEAR_CALIBRATE` runs a stall-confirmed endpoint sweep on the hub: drive
to the retract stop, then the deploy stop, then home — each leg ends on a
`MOTOR_STALL_EVENT` from the board's INA226.  A leg that never stalls
within the gear's travel timeout faults the gear (`NO_STALL_DETECTED`).
Status LEDs blink at 150 ms throughout.  (The legacy current-threshold
profiling — drag headroom, baseline current — is deferred; it needs
continuous current streaming the BiDcMotor role doesn't emit yet.)

## Build / flash

```
app/go/scalefx-flash.exe build gearcontrol --no-clean
app/go/scalefx-flash.exe flash gearcontrol --no-clean
```
