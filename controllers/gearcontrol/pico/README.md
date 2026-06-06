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

## Endstop detection — stall guard + dual-stage soft-start

A BiDcMotor finds its endstops by **current sensing** (each H-bridge has an
INA226 measuring both motor current AND bus voltage). Two orthogonal mechanisms:

**Stall guard (run-phase endstop detection)** — how the seek decides it hit the
stop. Two modes (live-retune via `BIMOTOR_SET_GUARD` / CLI `bimotor-guard`):
- **LiveRatio (recommended default):** averages the running current per stroke
  (after inrush blanking), then trips when `|I| ≥ baseline × ratio` (e.g. 2.5×).
  No per-motor threshold, battery-voltage independent. The right choice when the
  stall current is unknown / varies — a Fixed mA threshold set too high (the old
  2000 mA default) simply never trips on a motor that stalls at a few hundred mA.
- **Fixed:** trip on `|I| ≥ threshold_mA` sustained. Only when you know the value.

**Dual-stage soft-start probe (pre-phase, optional)** — before a seek commits
full power, drive a LOW-power probe toward the target and classify the start
state from the current curve (`BIMOTOR_SET_PROBE` / CLI `bimotor-probe`):
- current **decays** below `dropPct%` of its probe peak → rotor spun up =
  mechanism FREE → ramp to full power and run the seek;
- current **holds high** through the window → locked rotor = already hard against
  this end → brake WITHOUT slamming full power, report Reached.

This protects an unknown mechanism from a full-power grind into an already-engaged
end-stop. Current-only (no encoder); probing toward the target disambiguates
"already there". Stiff gearboxes may need higher probe power (raise probePct).
`probePct 0` disables the probe (default in firmware; the Studio diag tab enables
it at 35% for bench bring-up). Both phases stream a verbose `[bimotor]` trace to
the console (the firmware enables wire log emission at boot).

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

### Role-layer CLI (bench-test an expander — no hub)

When a GearControl (or any generic expander) is plugged STRAIGHT into the PC —
no HubFX in the loop — the CLI / Studio Console can attach, drive, and inspect
its roles directly over the wire.  This is how you bring up the board and prove
a servo or gear motor works before wiring it into a hub config.  These talk the
expander's own **role layer** (`ROLE_ATTACH/DETACH/LIST_REQ`, no GUID) — distinct
from the GUID-addressed `role-attach <guid> …` which routes through a hub's
Topology service and is rejected on a board that only advertises `PORTS|ROLES`.

Lifecycle (gated on the `ROLES` capability):

- `init` — activate the expander (mode=slave).  *(A directly-connected
  expander needs INIT; the hub auto-inits its own.)*
- `role-list-local` — list roles currently attached on the connected board.
- `role-attach-local <portKind> <portIdx> <roleKind> [hexcfg]` — bind a role.
  `portKind` = `servo|pwm|hbridge|input`; `roleKind` = `servo|bi-dc-motor|
  dc-motor|heater|led-animator` (or a raw byte).
- `role-detach-local <portKind> <portIdx>` — unbind.

Drive / inspect once attached (port index is the role's port, not a gear id):

- `bimotor-move-end <portIdx> <a|b> [duty=600] [timeoutMs=5000]` — drive a
  BiDcMotor to logical endstop **A** (`+duty`) or **B** (`-duty`); ACK is
  immediate, the outcome (`reached/timeout/aborted`) arrives async (`subscribe`
  to watch `BIMOTOR_ENDSTOP_RESULT`).
- `bimotor-seek <portIdx> <signedDuty> [timeoutMs=5000]` — position-agnostic
  seek (explicit signed duty); doesn't label which end was reached.
- `bimotor-status <portIdx>` — verbose: duty, voltage_mV, current_mA, stalled,
  position (A/B), guard mode.
- `servo-profile-get <portIdx>` / `servo-profile-set <portIdx> <key=val>…` —
  read / live-push a ServoActuator's motion profile (`min_us`, `max_us`,
  `center_us`, `max_speed`, `max_accel`, `max_jerk`, `reversed`).

Worked example (a gear motor on H-bridge 0, a door servo on servo 0):

```
init
role-attach-local hbridge 0 bi-dc-motor
role-attach-local servo   0 servo-actuator
role-list-local                       # → both rows
bimotor-seek 0 600 1200               # drive toward an endstop
bimotor-status 0                      # current / stall / position
servo-profile-set 0 max_us=1800 max_speed=600
role-detach-local hbridge 0
```

These map to `protocol/roles` (`CmdRoleAttach`/`CmdRoleDetach`/`CmdRoleListReq` +
`CmdBiMotor*` + `CmdServo*`) via `client.Roles.*`.  The `gear-*` commands above
are the effect-layer surface a hub uses to orchestrate the same primitives.

**GUI equivalent:** in ScaleFX Studio, connecting a gearcontrol expander on its
own shows a **Diagnostics** tab (gated to `controllerType === 'gearcontrol'`)
that wraps the same role-layer surface — attach/detach, a servo travel slider,
gear-motor seek/endstop buttons, and live stall-current — plus a raw-command box.
Backed by the `App.Diag*` Wails bindings (`app/go/studio/app_geardiag.go`).
HubFX-only config/topology auto-loads are skipped for an expander (it has no
config of its own; the hub holds it in `/hubfx.yaml`'s `expanders:` block).

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
