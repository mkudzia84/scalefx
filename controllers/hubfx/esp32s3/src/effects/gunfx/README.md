# GunFX effect orchestrator (master-side)

> **Status: descriptive scaffolding.** Implementation lands as part of
> the GunFX slave migration (see
> [`instructions/15-GENERIC-SLAVE-REFACTOR.md`](../../../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)
> § "Step 1 — GunFX").  Headers in this directory capture the
> high-level behaviour the orchestrator must reproduce against the
> generic slave's component primitives.  No working code yet.

In the post-pivot architecture, a "GunFX board" is just a slave that
exposes 1 servo (recoil) + 3 PWM channels (trigger motor, smoke
heater, smoke fan) + INA226 current sensing.  The slave does **none**
of the firing logic — it sets a servo target, drives a duty, reads a
current.  All the gun-firing semantics live here, on the master.

## Effects covered

### Trigger

| Feature | Inputs | Slave commands emitted |
|---|---|---|
| Single shot | trigger pull edge (RC input or HUBFX command) | `PWM_SET_DUTY` (trigger motor on for cycle) → wait → `PWM_SET_DUTY 0` |
| Burst (N rounds) | trigger pull + burst-count config | repeat single-shot N times at the configured rate-of-fire |
| Full-auto | trigger HELD | continuous trigger-motor cycling at rate-of-fire while RC channel is above threshold |
| Rate-of-fire scheduling | YAML config: rounds-per-minute table per RC channel band | trigger-cycle period derived from RC input + RoF table |
| Recoil sync | trigger fire event | `SERVO_SET` (recoil position) at fire moment, `SERVO_TARGET_REACHED` async drives reset → `SERVO_SET` (return position) |

### Smoke generator

| Feature | Inputs | Slave commands emitted |
|---|---|---|
| Smoke arm | RC channel high | `PWM_SET_DUTY` heater target |
| Smoke fire | RC channel + heater current threshold | `PWM_SET_DUTY` fan high during firing |
| Heater current limit / cutoff | INA226 current readback exceeds threshold | `PWM_SET_DUTY 0` heater + alarm event |
| Cool-down | trigger release | timed fan-on after heater-off so the chamber clears |

### Diagnostics + state surface

| Feature | Source | Wire |
|---|---|---|
| Live trigger state | master state machine | broadcast in HubFX STATUS payload |
| Smoke heater current | INA226 via `PWM_QUERY` (PwmFlags::HAS_CURRENT_SENSE) | exposed as a status field |
| Recoil servo position | `SERVO_QUERY` cached from last `SERVO_TARGET_REACHED` | status field |

## What's gone vs the legacy slave firmware

- **Slave-side rate-of-fire scheduler** — was a state machine in
  `gunfx_pico.ino`; now a master-side timer.
- **Slave-side smoke automation** (heater warm-up + fan-on/off
  timing) — now master-side.
- **`GunFxClient` / `GunFxServer` wire wrappers** in
  `lib/sfx_boards/gunfx/` — deleted; replaced by generic `SlaveApi`.
- **`GunFxPacket` / `GunFxError` namespaces** in
  `lib/sfx_serial/serial/gunfx/gunfx.h` — deleted; the firing logic
  uses generic `SlavePacket::*` IDs against whichever slave reports
  the right component fingerprint.

## Scaffolding files in this directory

- [`gunfx_effect.h`](gunfx_effect.h) — orchestrator class declaration
  (state machine + RC-input wiring + slave command emitters).  No
  implementation yet.

## Migration order

This directory is created **before** the GunFX slave migration so the
master-side scaffolding is reviewable first.  The orchestrator is then
implemented as part of Step 1 of the refactor plan, using the new
generic `SlaveApi` against a re-flashed GunFX slave.
