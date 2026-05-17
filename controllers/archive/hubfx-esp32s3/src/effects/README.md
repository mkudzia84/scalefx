# hubfx/effects/ — master-side effect orchestrators

After the slave-as-port-expander pivot
([instructions/15-GENERIC-SLAVE-REFACTOR.md](../../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)),
**all high-level effect logic** lives master-side.  Slaves are
generic component-collection muxes (`SlaveServer<TServos, TPwms,
TLeds>` from
[`controllers/lib/sfx_slave/`](../../../../lib/sfx_slave/)) — they
move servos, drive PWMs, run LED programs.  They make no decisions
about *when* or *why*.

This directory holds the orchestrators that make those decisions, one
subdirectory per effect family:

| Directory | What it orchestrates | Slave fingerprint expected | Migration step |
|---|---|---|---|
| [`engine_fx/`](engine_fx.h) (existing) | engine-sound state machine (RPM → playback crossfade) | (uses HubFX-local audio mixer, no slave) | already master-side |
| [`gunfx/`](gunfx/) | gun firing patterns, smoke generation, recoil sync | 1 servo + 3 PWM (current-sensed heater) | Step 1 |
| [`gearcontrol/`](gearcontrol/) | gear cycle (door choreography), steering yaw, brake interlock, battery cutoff | 6 servos + 2 PWM (current-sensed motor) | Step 2 |
| [`lightfx/`](lightfx/) | light-program selection, landing groups, day/night switch | 2 servos + 8 LedDigital | Step 3 |

Each subdirectory currently contains:

- `README.md` — full description of the high-level functionality the
  orchestrator owns + what was removed from the slave + what stayed
  slave-side and why
- `<name>_effect.h` — orchestrator class declaration (state machine,
  config struct, async-event hooks, command surface)

Implementations land per migration step.  Status today: **scaffolding
only** — declarations and READMEs.  No `.cpp` files yet; the
orchestrators don't compile or run, but the API surface is concrete
enough that the master's effect-graph wiring can be drafted against it.

## Common patterns across all three orchestrators

- **`begin(slave, rc, status)`** — bind a generic `SlaveApi`, the
  RC-input frontend, and the HubFX status builder.  Validates the
  slave's `COMPONENT_LIST` fingerprint at attach time and refuses to
  engage if the components don't match.
- **`applyConfig(cfg)`** — invoked from the matching YAML
  `ConfigStore::onLoaded()` callback.  Reloading config at runtime is
  free — the orchestrator just re-resolves bindings.
- **`update()`** — called once per main loop tick.  Polls RC inputs,
  drives the state machine, polls sensing as needed.
- **Async-event hooks** — `onServoTargetReached(idx, pos)` and
  `onLedProgramDone(addr, progId)` invoked from the SlaveApi observer
  chain.  These are how the orchestrators chain multi-step effects
  without master-side timing math.
- **Manual command surface** — every orchestrator exposes a few
  public methods (e.g. `manualFireOnce()`, `requestDeploy()`,
  `setGroupActive()`) so CLI and Studio can drive effects directly,
  bypassing RC input.

## What goes away when this all lands

- [`controllers/lib/sfx_boards/`](../../../../lib/sfx_boards/) (the
  per-board client/server wire wrappers) is **deleted**.  Generic
  `SlaveApi` replaces all of it.
- The slave `_pico.ino` files for GunFX / LightFX / GearControl are
  **rewritten** as ~30-line files that just instantiate the relevant
  collection sizes and bind a `SlaveServer` — no high-level logic.
- The legacy per-board wire ranges (0x01-0x2F, 0x40-0x5F, 0x60-0x7F)
  are **retired** (per the no-compatibility-window policy in the
  refactor plan).

## See also

- [`../config/`](../config/) — master-side YAML schemas (gunfx,
  enginefx, lightfx, gearcontrol, hubfx).  Unchanged by the pivot.
- [`controllers/lib/sfx_peripherals/collections/`](../../../../lib/sfx_peripherals/collections/)
  — the slave-side component collection library these orchestrators
  speak to.
- [`controllers/lib/sfx_slave/`](../../../../lib/sfx_slave/) —
  generic slave server that owns the wire-level dispatch on the
  slave side.
