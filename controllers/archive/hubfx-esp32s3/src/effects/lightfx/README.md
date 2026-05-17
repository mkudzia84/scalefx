# LightFX effect orchestrator (master-side)

> **Status: descriptive scaffolding.** Implementation lands during
> Step 3 of the slave migration (see
> [`instructions/15-GENERIC-SLAVE-REFACTOR.md`](../../../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)).
> Headers in this directory describe the master-side responsibilities
> for the LightFX effect set.  No working code yet.

In the post-pivot architecture, a "LightFX board" is a generic slave
exposing 2 servos (gear leg / nav-light leg) + 8 dedicated LED
channels with the per-channel event-sequence runtime.  **The LED
runtime stays slave-side** — it's timing-sensitive — but every
*decision* about which program to run on which channel, when to start
it, what its parameters are, lives here on the master.

## Effects covered

### Light program selection + scheduling

| Feature | Inputs | Slave commands emitted |
|---|---|---|
| Pick active program per channel | YAML `light_fx.programs[]` + RC channel band | `LED_PROGRAM_LOAD` (once) → `LED_PROGRAM_RUN` |
| Day / night switch | RC channel + threshold | swap loaded program set on every channel |
| Sync multi-channel start | grouped channels share a trigger | `LED_PROGRAM_RUN` with `SYNC_START` then a triggering RUN |
| One-shot effect (e.g. muzzle flash) | gun-fire event chain | `LED_PROGRAM_RUN` (REPEAT clear) → await `LED_PROGRAM_DONE` |
| Master brightness | YAML + Studio slider | per-channel `LED_SET_BRIGHTNESS` scale |

### Landing-light groups

Pre-pivot, the slave knew about "landing-light groups" — bundles of
channels that activate together.  Post-pivot, the master:

1. Resolves the YAML group definition (`landing_groups[]`) into a list
   of channel addresses.
2. On RC trigger / Studio toggle, emits per-channel `LED_PROGRAM_RUN`
   commands using `SYNC_START` so the channels light in lock-step.
3. On de-trigger, emits `LED_PROGRAM_STOP` for each channel.

Groups can span **dedicated LEDs and PWM-borrowed LEDs uniformly**
because the LED address byte distinguishes them via bit 7 (see
`SlavePacket::LedAddr` in slave.h).  A landing group can pull half its
channels from the LightFX slave's dedicated LedDigital outputs and
half from the HubFX-local AW9523B-driven outputs without the
master-side group code knowing or caring.

### Servo control

Both servos on the slave are driven directly by master-issued
`SERVO_SET` commands.  The orchestrator exposes:

- Gear servo: deploy / retract bound to RC channel
- Nav-light servo: position bound to RC channel band (3 positions
  e.g. nav-off / nav-low / nav-bright)

`SERVO_TARGET_REACHED` events are observed but currently unused for
LightFX (no inter-servo sequencing on this board).

### Diagnostics + state surface

| Feature | Source | Wire |
|---|---|---|
| Per-channel program state | `LED_QUERY` cached + `LED_PROGRAM_DONE` async events | broadcast in HubFX STATUS |
| Active program ID per channel | master-side state | STATUS field |
| RC band → program mapping | YAML config + RC input frontend | logged + Studio diagnostic |

## What's gone vs the legacy LightFX firmware

- **Slave-side `LANDING_LIGHT_BIND` / group state machine** —
  removed; bundles are master-side YAML, fanned out as per-channel
  `LED_PROGRAM_RUN` calls with `SYNC_START`.
- **Slave-side day/night auto-switch** — removed; master watches the
  RC channel and reloads programs.
- **`LightFxClient` / `LightFxServer` wire wrappers** in
  `lib/sfx_boards/lightfx/` — deleted; replaced by generic `SlaveApi`.
- **`LightFxPacket` / `LightFxError`** in
  `lib/sfx_serial/serial/lightfx/lightfx.h` — deleted; LED-runtime
  semantics use `SlavePacket::LED_*` IDs.

## What stayed slave-side (and why)

- **`LedEventSeq` runtime** (event interpolation, fade math, BAM /
  hardware-PWM emission).  Latency-critical at 100+ Hz tick rate;
  USB jitter would corrupt smooth fades if ticked from the master.
- **`ServoControl` motion profiler** (trapezoidal accel/decel).
  Same reason — predictable per-axis timing requires local execution.

## Scaffolding files in this directory

- [`lightfx_effect.h`](lightfx_effect.h) — orchestrator class with
  group resolver + program-set switcher + servo bindings.

## Cross-references to existing infrastructure

- The `/lightfx.yaml` schema (group definitions, program list, RC
  bindings) is already master-side in
  [`controllers/hubfx/esp32s3/src/config/`](../../config/) — only the
  *application* of that config changes.
- The (old) `pushLightFxConfigToSlave()` translator
  (`src/protocol/hub_lightfx_apply.cpp`) collapses into a
  `LightFxEffect::applyConfig()` body that emits the new
  `SlavePacket::LED_*` commands instead of the legacy ones.
