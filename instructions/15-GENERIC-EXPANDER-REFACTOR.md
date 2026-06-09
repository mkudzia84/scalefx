# Generic Expander Refactor — Pivot to Component Collections

> **Status (2026-06-09):** historical planning record. The pivot LANDED, but
> in a **different final shape** than the "three collections" /
> `ExpanderServer<TServos, TPwms, TLeds>` design sketched below. What
> actually shipped is the **Ports/Roles system**: an expander declares static
> ports via `BoardOf<...>` + `PortServicePolicy`, and roles (servo, LED,
> heater, BiDcMotor, …) attach at runtime into a `PortRegistry` `std::variant`
> slot via `RoleServicePolicy` (`ROLE_ATTACH` / `ROLE_BULK_ATTACH`). The hub
> drives them transparently by opaque `PortRef{guid, kind, idx}` (Rule 58) —
> there is no `ExpanderServer`, no `ServoCollection`/`PwmCollection`/
> `LedCollection`, and no `ExpanderPacket 0x01..0x7F` range. For the current
> "build a new expander" contract see
> [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md); for the data-flow
> diagrams see [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md) §3.
> The narrative below is kept for history.
>
> **STATUS: planning + library skeleton landed 2026-05-06.** Per-board
> migration is staged across multiple sessions (one board at a time)
> with the test harness as the verification surface — no expander is
> migrated until its replacement passes the existing protocol tests.

> **Terminology note (2026-05-16):** what used to be called *slave
> boards* (GunFX / LightFX / GearControl) are now consistently called
> **expander boards** in design documents.  The rename is purely
> conceptual and lines up with the `GpioExpander` / `HwPwmExpander`
> concepts already used at the peripheral layer.  Live code (the
> uncommitted `controllers/lib/sfx_slave/`, the `BoardState::SLAVE`
> constant, the `CoreCapability::SLAVE_BUS` bit, the in-flight
> `serial/slave/slave.h` header) keeps the old name until this refactor
> actually lands — at which point the rename happens atomically with the
> behavioural change.  This doc, [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md),
> and the CLAUDE.md / copilot-instructions.md sections that describe
> *future* architecture all use "expander" today.

## Pivot summary

Boards (GunFX, LightFX, GearControl) were originally domain-specific:
each owned a high-level firmware (gun-trigger state machine, landing
light groups, gear-deploy choreography) and a board-specific protocol
(`GunFxPacket`, `LightFxPacket`, `GearControlPacket`).  Boards could
also run **standalone** — config-driven, autonomous behaviour with no
master attached.

The new architecture inverts that: **expander boards are dumb peripheral
muxes**.  Every expander exposes the same generic protocol and is just
a runtime-discoverable bag of three collections:

```
   Expander board
   ├── ServoCollection<N>            ← N hobby servos
   ├── PwmCollection<M>              ← M PWM channels — mode-mutable
   │      └─ each channel can be PwmGeneric / PwmLed / PwmMotor / PwmHeater
   │      └─ optional voltage/current sensing per channel
   └── LedCollection<K, TGpio>       ← K LEDs with event-sequence runtime
```

All "what does this board do" semantics now live in the master (HubFX)
or the host PC client orchestrating components.  The expander just
answers queries and toggles outputs.

### Concrete consequences

1. **No more STANDALONE state.**  Boards boot to `IDLE` with all
   peripherals detached (no PWM, no LED current).  They sit there until
   `INIT(EXPANDER)` arrives from the master, or `INIT(DIRECT)` arrives
   from a PC client (the test path).  `BoardState::STANDALONE` (0x01) is
   removed; the value is reserved for back-compat decoding only.

2. **One protocol for all expanders.**  Defined in
   [`controllers/lib/sfx_serial/serial/expander/expander.h`](../controllers/lib/sfx_serial/serial/expander/expander.h)
   in the new packet range **0x10–0x3F**.  The legacy per-board ranges
   (GunFX 0x01-0x2F, LightFX 0x40-0x5F, GearControl 0x60-0x7F) along
   with all per-board protocol headers, Go packages, and Studio APIs
   are **deleted in the same PR as the expander migration** — there is
   no cross-version compatibility window.  All boards flip to the new
   protocol together; old firmware in the field is wiped at first
   re-flash.

3. **Component fingerprint via IDENTIFY.**  `COMPONENT_LIST_REQ` (0x10)
   returns a packed `ComponentInfo[]` describing the board's servo
   count, PWM channel modes + capability flags, and LED count.  The Go
   SDK stops shipping `GunFxApi` / `LightFxApi` / `GearControlApi`
   classes — there's just an `ExpanderApi` that takes the discovered
   fingerprint and exposes the available actions.

4. **Persistent board identity (GUID + optional friendly name).**  Every
   expander board has a deterministic 128-bit UUID derived from its
   silicon serial at first boot, persisted to `/.system/board.guid` on
   flash, and reported in the IDENTIFY payload.  The master matches
   expanders by `(ExpanderType, BoardGuid)` rather than enumeration
   order — so two LightFX boards plugged into the same HubFX are
   unambiguous regardless of which USB port enumerates first.  Users
   can layer a friendly name (e.g. `"left-wing"`, `"front-axle"`) on
   top via `IDENT_SET`, persisted in `/board.yaml`.  See
   [17-SYSTEM-SERVICES.md §3](17-SYSTEM-SERVICES.md) for the GUID
   derivation, namespace UUID, and provisioning flow.

## Expander-side responsibilities — timing-sensitive runtimes only

The "thin expander / thick master" pivot has two narrow exceptions
where the expander still runs intelligence locally.  Both exist because
USB jitter (~ms-scale) would corrupt their output if they were ticked
from the master:

1. **LED event-sequence runtime** (`LedEventSeq` in
   [`lib/sfx_peripherals/led/`](../controllers/lib/sfx_peripherals/led/)).
   Master loads a program once via `LED_PROGRAM_LOAD`, then triggers
   it via `LED_PROGRAM_RUN`.  Per-tick brightness emission, fade
   interpolation, and BAM/hardware-PWM output happen entirely
   expander-side — predictable timing, low bandwidth.

2. **Servo motion profile** (`ServoControl` trapezoidal accel/decel).
   Master sends a target position via `SERVO_SET`; the expander's
   profiler ramps the servo from current → target according to the
   calibrated accel / decel / max-speed profile.  Master never writes
   intermediate pulse widths.

Everything else — gun firing patterns, LED program selection logic,
gear-with-door sequencing, RC-channel-to-effect mapping, config
persistence beyond the board identifier — lives master-side.  Expanders
do **deterministic local execution**, not **decision-making**.

The boundary is concrete: any logic that requires deterministic
sub-50ms timing relative to a hardware output stays expander-side; any
logic whose timing is set by user actions, RC-input edges, or
high-level effect choreography lives master-side.

## Runtime port reconfiguration

Every PWM channel that carries the `MODE_MUTABLE` capability flag can
have its **function** swapped at runtime via the master:

| Master command | Effect |
|---|---|
| `PWM_SET_MODE  [idx][mode]` | switch ComponentKind only — keeps current frequency / cfgFlags / maxDuty |
| `PWM_SET_FREQ  [idx][freq_Hz]` | switch frequency only — keeps mode |
| `PWM_RECONFIGURE  [idx][mode][freq][cfgFlags][maxDuty]` | atomic full-config swap — channel never observes a half-applied state |
| `PWM_GET_CONFIG  [idx]` | query → returns full runtime config + hw flags + sense + pair info |

Two layers of per-channel descriptors keep the model clean:

- **`PwmSpec`** — compile-time hardware truth (pin, expander binding,
  hw capability flags, default mode at boot, sensing wiring, H-bridge
  pair).  Never changes after the firmware ships.
- **`PwmRuntimeConfig`** — runtime mutable state (mode, freq_Hz,
  cfgFlags, maxDuty).  Master writes via the commands above; expander
  persists in RAM only (boots back to `PwmSpec.defaultMode`).

`PwmConfigFlags` (runtime, distinct from `PwmFlags` which are
hardware capability bits):

| Flag | Effect |
|---|---|
| `INVERT_OUTPUT` | invert the PWM signal at the pin (active-low MOSFET driver topologies) |
| `DC_BRAKE_ON_STOP` | PwmMotor: on speed=0 brake (both H-bridge halves LOW); default = coast |
| `HEATER_BANG_BANG` | PwmHeater: closed-loop bang-bang using the thermistor; default = open-loop duty |

### Mode-transition side effects

Switching modes is not "just change the kind byte" — there's a
deterministic cleanup sequence the expander runs every time:

| Transition | Side effects |
|---|---|
| `*` → `PwmGeneric` | duty 0; no motor / heater state retained |
| `*` → `PwmLed` | duty 0; LedCollection adopts as extension output; **no** running program (master must `LED_PROGRAM_RUN` to start) |
| `*` → `PwmMotor` | duty 0 (motor stopped); paired-channel constraint validated |
| `*` → `PwmHeater` | duty 0 (heater off); thermistor channel referenced if `HEATER_BANG_BANG` is set |
| `PwmLed` → `*` | running program **stopped** without firing `LED_PROGRAM_DONE` (master initiated, not natural completion); LedCollection drops extension ref |
| `PwmMotor` → `*` | motor coasts to stop (or brakes if `DC_BRAKE_ON_STOP`) before mode bit flips |

Master code orchestrating high-level effects can rely on these
guarantees — no need to manually issue zero-out commands before a
reconfigure.

### Why two layers (Spec + RuntimeConfig)

- **Hardware capability vs runtime policy.**  `PwmSpec.hwFlags` says
  what the silicon CAN do (`HAS_HARDWARE_PWM`, `MODE_MUTABLE`,
  `IS_BIDIR_PAIR`, sensing presence).  `PwmRuntimeConfig.cfgFlags` says
  what the firmware IS doing right now (output inversion, brake
  policy, closed-loop vs open-loop heater).
- **Wire format symmetry.**  `PWM_GET_CONFIG_RESP` exposes both — the
  master reads `hwFlags` once at COMPONENT_LIST time then watches the
  cheaper runtime fields via `PWM_QUERY` for live state.
- **Compile-time safety.**  Any attempt to set a runtime mode outside
  what `hwFlags` permits is rejected with `MODE_NOT_SUPPORTED`.

## Intelligent servo control surface

Master code never writes raw PWM pulse widths.  Every servo command
goes through the expander's local motion-profile engine, which applies
calibrated min/max clamps, max-speed limits, and trapezoidal accel/
decel before the pin actually moves.  Master concerns: where to go,
how aggressively, when to add a transient kick.

| Command | Effect |
|---|---|
| `SERVO_SET [idx][pos_us]` | command target — expander runs the profile to it; emits `SERVO_TARGET_REACHED` on convergence |
| `SERVO_CONFIG [idx][min][max][center][maxSpeed][accel][decel]` | full calibration write — range + motion profile in one packet |
| `SERVO_SET_MOTION [idx][maxSpeed][accel][decel]` | tune the dynamics only — keeps current range |
| `SERVO_APPLY_JERK [idx][offset_us:i16][duration_ms]` | transient signed offset added to running target; decays through the profile.  Use for recoil, idle wobble, brief mechanical reactions.  Multiple in-flight jerks compose additively |
| `SERVO_HOLD [idx][hold]` | soft-disable PWM — internal target/profile state preserved, pin idles.  Re-attach with `hold=0`; servo runs to held target through the profile.  Power-saving hold + bench hand-positioning |
| `SERVO_QUERY [idx]` | live readback: current pos, target, velocity |

The "jerk" message is the showcase async-effect tool.  A trigger
pull = one `SERVO_SET` (recoil position) + several queued
`SERVO_APPLY_JERK`s stacked on top for vibration: master fires
discrete events, expander's profile turns them into smooth motion.

```
   master                                expander
   ──────                                ────────
   SERVO_SET     idx=0 pos=2000   ───►
   SERVO_APPLY_JERK idx=0 off=+150 dur=80 ───►
   SERVO_APPLY_JERK idx=0 off=-100 dur=120 ───► (composes additively)
                                  ◄───  SERVO_TARGET_REACHED idx=0 pos=2000  (after jerks decay)
```

## LED event vocabulary — programs ARE sequences of events

`LED_PROGRAM_LOAD` carries a sequence of 8-byte events; each event is
typed (one of `LedEventType::*`) and the expander's `LedEventSeq`
runtime walks them in order, executing each before advancing.  The
wire format mirrors the existing animation classes in
`sfx_peripherals/led/led_events.h`:

```
   [type:u8][p1:u16LE][p2:u16LE][p3:u8][p4:u8][p5:u8]   = 8 bytes
```

| Type | Mnemonic | Wire payload |
|---|---|---|
| 0 | `ON` (solid hold) | p1\|p2 = duration_ms (32-bit), p3 = brightness 0..100, p4 = power-saving flag, p5 = power-saving PWM duty |
| 1 | `OFF` (solid off) | p1\|p2 = duration_ms |
| 2 | `FLASHING` (square wave) | p1 = period_ms, p2 = duration_ms (0 = forever), p3 = brightness |
| 3 | `FADE_IN` | p1\|p2 = duration_ms, p3 = target brightness |
| 4 | `FADE_OUT` | p1\|p2 = duration_ms |
| 5 | `FADING` (sinusoidal breath) | p1 = period_ms, p2 = duration_ms, p3 = peak %, p4 = trough % |
| 6 | `BEACON` (rotating-beacon flash + long-off) | p1 = flash_ms, p2 = off_ms, p3 = brightness, p4 = repeat count (0 = forever) |

### One-shot vs looped programs

Two layers of repeat control:

1. **Per-event durations** — each event has its own `duration_ms`
   (or repeat-count, for `BEACON`).  Setting an event's duration to 0
   makes it run **forever** for types where that's meaningful (`ON`,
   `OFF`, `FLASHING`, `FADING`, `BEACON`).  An indefinite event blocks
   sequence advance — useful as a terminal "hold this state" event.

2. **Program-level loop** — `LED_PROGRAM_RUN` flags byte:

   - `LedProgramFlags::REPEAT` (0x01) clear → run the sequence once.
     When the last event finishes naturally, the expander emits
     `LED_PROGRAM_DONE` and leaves the channel at the final brightness.
   - `LedProgramFlags::REPEAT` set → at end of sequence, restart at
     event 0.  Loops forever.  No `LED_PROGRAM_DONE` event fires; only
     `LED_PROGRAM_STOP` ends it.
   - `LedProgramFlags::SYNC_START` (0x02) → defer start until the
     next `LED_PROGRAM_RUN` triggers — used when the master wants to
     load programs on multiple channels then start them all at once
     in lock-step.

So a "fire muzzle flash once" effect is:

```
   LED_PROGRAM_LOAD addr=0 progId=1 events=[
       FADE_IN  (dur=20, br=100),
       ON       (dur=40, br=100),
       FADE_OUT (dur=80),
       OFF      (dur=0)              // terminal — never completes
   ]
   LED_PROGRAM_RUN  addr=0 progId=1 flags=0      // no REPEAT → one-shot
                                  ◄───  LED_PROGRAM_DONE addr=0 progId=1
```

…and a "rotating beacon" forever:

```
   LED_PROGRAM_LOAD addr=2 progId=1 events=[
       BEACON   (flash=80ms, off=920ms, br=100, repeats=0)
   ]
   LED_PROGRAM_RUN  addr=2 progId=1 flags=REPEAT
   ...                                         (no LED_PROGRAM_DONE; runs forever)
   LED_PROGRAM_STOP addr=2                     // master ends it on demand
```

## Motor primitives — promote from gearcontrol, don't reinvent

Audit (2026-05-06) of the existing GearControl firmware found three
already-generic motor primitives that the new generic-expander layer
should delegate to rather than duplicate:

| Primitive | Existing source | Target home | Move scope |
|---|---|---|---|
| `StallDetector` | [`controllers/gearcontrol/pico/src/stall_detector.h`](../controllers/gearcontrol/pico/src/stall_detector.h) | `sfx_peripherals/motor/stall_detector.h` | move as-is — pure state machine, DI-clean (`update(uint16_t current_mA)`) |
| `StallCalibrator` | [`controllers/gearcontrol/pico/src/stall_calibrator.h`](../controllers/gearcontrol/pico/src/stall_calibrator.h) | `sfx_peripherals/motor/stall_calibrator.h` | move; replace `DoorSequencer*` dep with a generic `std::function` callback so the calibrator works on any board |
| `DcMotor` | (new — extract H-bridge logic from `LandingGear::setMotor()`) | `sfx_peripherals/motor/dc_motor.h` | new — wraps single-pin / dual-GPIO H-bridge / PWM+dir topologies |

`StallDetector` already encodes the production-grade detail the
inline placeholder in `PwmCollection::update()` lacks:

- **Startup-inrush ignore** (default 500 ms) — skips the brief
  high-current burst when a motor first energises
- **Motor-presence detection** (`NO_MOTOR` result) — distinguishes
  "stalled" from "not connected"
- **Absolute timeout** (default 10 s max run) — fires `TIMEOUT_STALL`
  / `TIMEOUT_ERROR` on either-end protection
- **Sustained-confirmation** (default 200 ms over threshold) —
  prevents transient drag spikes from tripping

`StallCalibrator` adds the multi-phase calibration sequencer:

- CLEAR_RUN — drive motor briefly, measure free-running baseline
- DEPLOY_RUN — drive forward to stall, record peak
- RETRACT_RUN — drive reverse to stall, record peak
- Compute threshold = 80 % of (peak − baseline) for safety margin

The 80 % safety factor + baseline-averaging window are encoded
constants (`CalibConfig` namespace) tuned from real GearControl
deployments.

### Migration status (2026-05-06)

| Primitive | Status |
|---|---|
| `DcMotor` | **Landed** — `sfx_peripherals/motor/dc_motor.h`, three topologies (SinglePinPwm / HBridgeDualGpio / HBridgePwmDir) |
| `StallDetector` | **Landed** — promoted to `sfx_peripherals/motor/stall_detector.h` under `sfx_peripherals` namespace.  PwmCollection now owns one per channel (`std::array<StallDetector, N>`) and delegates the state machine.  Original at `controllers/gearcontrol/pico/src/stall_detector.{h,cpp}` will be deleted during the GearControl Step-2 PR |
| `StallCalibrator` | **Deferred** — placeholder header in place.  Promotion happens during GearControl Step-2 because two deps need generalising: `CalibPhase`/`GearControlCalibStatus` (replace with board-agnostic POD) and `DoorSequencer*` (replace with `std::function<bool(Phase)>` callback).  Until then: master-side calibration uses `PWM_SET_STALL_GUARD` / `PWM_CLEAR_STALL` / `PWM_STALL` packets directly |

The master-side internal effects orchestrator drives calibration via
the wire packets — calibration **state** lives master-side per the
pivot.  `StallCalibrator`, when promoted, becomes a hub-side helper
rather than an expander-side one.

See [`controllers/lib/sfx_peripherals/motor/README.md`](../controllers/lib/sfx_peripherals/motor/README.md)
for the API surfaces of the three primitives.

## Motor stall detection + endpoint calibration

DC motors driven through `PwmMotor`-mode channels with current
sensing get a stall guard managed expander-side.  This is the
primitive the master uses for **open-loop endpoint calibration** (no
encoder / limit switch required) and **runtime safety** (catch a
jammed gear retract before the heater chars).

### Stall guard primitive

| Wire | Purpose |
|---|---|
| `PWM_SET_STALL_GUARD [idx][threshold_mA:u16][debounce_ms:u8][stallFlags:u8]` | configure threshold + behaviour |
| `PWM_CLEAR_STALL [idx]` | re-arm a latched channel |
| `PWM_STALL [idx][peak_mA:u16][duration_ms:u16]` | async (TAG_ASYNC) — emitted when guard trips |

`stallFlags` (`ExpanderPacket::StallFlags::*`):

- `AUTO_STOP` (0x01) — expander commands the channel to duty 0 the
  moment the guard trips, before emitting the async event
- `BRAKE_ON_STOP` (0x02) — brake (both H-bridge halves LOW) instead of
  coast on AUTO_STOP
- `LATCH` (0x04) — channel rejects subsequent setDuty/setMotor (other
  than `0`) until the master sends `PWM_CLEAR_STALL`

The expander's update tick reads the channel's current via the bound
`SensePolicy`, holds an over-threshold timer, and trips after
`debounce_ms` of sustained excess.  No master-side polling.

### Endpoint calibration via stall (master-side state machine)

A linear-actuator DC motor (gear retract, smoke pump, gun bolt) has
two physical endpoints — fully extended and fully retracted.  Without
limit switches or encoders, the master finds them by **driving until
stall in each direction** and measuring travel time:

```
   master                                     expander
   ──────                                     ────────
   PWM_SET_STALL_GUARD idx=0
       threshold=1500 mA, debounce=80 ms
       flags=AUTO_STOP | LATCH                 ───►   ACK
   PWM_SET_MOTOR idx=0 speed=+700              ───►   ACK
   t_start = now
                                              ◄───   PWM_STALL idx=0 peak=1620 mA dur=92 ms
   t_forward = now − t_start                          // forward travel time
   PWM_CLEAR_STALL idx=0                       ───►   ACK
   PWM_SET_MOTOR idx=0 speed=−700              ───►   ACK
   t_start = now
                                              ◄───   PWM_STALL idx=0 peak=1580 mA dur=88 ms
   t_reverse = now − t_start                          // reverse travel time
   PWM_CLEAR_STALL idx=0                       ───►   ACK
```

Now the master knows: at `speed=±700`, the actuator traverses
end-to-end in `t_forward` / `t_reverse` ms.  Subsequent positional
moves (e.g. "extend to 60 % travel") become timed PWM_SET_MOTOR
commands followed by PWM_SET_MOTOR(0).

Calibration state lives entirely **master-side** — expander just
provides the stall primitive.  Re-running calibration after a
mechanical change is a master-side command sequence.

### Runtime safety

Outside calibration, the master leaves a stall guard armed at a
slightly looser threshold (e.g. 2× the steady-state current measured
during calibration) so unexpected obstructions trip an `AUTO_STOP`
before damaging the actuator.  `LATCH` is recommended in this mode so
the master sees the stall and decides explicitly when it's safe to
move again.

### Driving back and forth

`PWM_SET_MOTOR [idx][speed:i16]` already covers it — signed speed,
-1000 = full reverse, +1000 = full forward, 0 = stop.  Behaviour on 0:

- `cfgFlags::DC_BRAKE_ON_STOP` clear (default) → coast
- `cfgFlags::DC_BRAKE_ON_STOP` set → brake (both H-bridge halves LOW)

H-bridge wiring (when paired channels carry `IS_BIDIR_PAIR` in their
PwmFlags) is handled by the expander from a single `PWM_SET_MOTOR`
call on the primary channel — master doesn't drive the pair directly.

## Async event taxonomy — expander → master

| Packet | ID | Cardinality | Carried data | When emitted |
|---|---|---|---|---|
| `SERVO_TARGET_REACHED` | `0x14` | one per `SERVO_SET` | `[idx][position_us:u16]` | trapezoidal profile converges (velocity ≈ 0 within tolerance) |
| `SERVO_MOTION_UPDATE` | `0x18` | periodic (configurable rate) | `[idx][pos:u16][target:u16][vel:i16]` | while ANY servo is actively profiling, until target reached.  Master enables via `SERVO_MOTION_UPDATES` |
| `PWM_STALL` | `0x3C` | one per stall trip | `[idx][peak_mA:u16][duration_ms:u16]` | StallDetector state machine reaches a terminal result (CONFIRMED / TIMEOUT_*) |
| `LED_PROGRAM_DONE` | `0x56` | one per non-repeating program | `[addr][progId]` | program's last event finishes (REPEAT clear); never fires for looping programs |
| `EXPANDER_STATUS_BROADCAST` | `0x06` | periodic (1..10 Hz) | header + servo + PWM + LED snapshots | configured via `EXPANDER_STATUS_RATE`; default disabled |

### Calibration progress

Calibration is a **master-side** orchestration — the expander doesn't
run a calibration state machine.  The master's internal effects layer
drives `PWM_SET_MOTOR(±speed)` / `PWM_SET_STALL_GUARD` / etc. and
reacts to `PWM_STALL` events to advance through phases (CLEAR_RUN →
DEPLOY_RUN → RETRACT_RUN → COMPUTE).  Progress updates flow
**upstream** from the master orchestrator to its consumers (Studio
observers, CLI), not from the expander — they're a master-side
observable, not a wire packet.

The wire-level primitives the master combines for calibration:

```
   PWM_SET_STALL_GUARD idx=0  thresh=high  debounce  flags=AUTO_STOP|LATCH ───►  ACK
   PWM_SET_MOTOR       idx=0  speed=+700                                   ───►  ACK
   t_start = now
                                                                          ◄───  PWM_STALL idx=0 peak=… dur=…
   master records t_forward = now − t_start
   PWM_CLEAR_STALL                                                         ───►  ACK
   PWM_SET_MOTOR       idx=0  speed=−700                                   ───►  ACK
   t_start = now
                                                                          ◄───  PWM_STALL idx=0 peak=… dur=…
   master records t_reverse, computes endpoints + safety threshold
```

The orchestrator emits its own typed events (`CalibrationStarted`,
`CalibrationPhaseTransition`, `CalibrationComplete`) on its outbound
observer chain — those are master-side concepts that Studio + CLI
observe, not expander wire packets.

## Async completion events — expander → master

Because the master orchestrates higher-level effects (gun firing
patterns, gear-with-door cycles, LED sequence chains), it needs to
know when an expander-side timing engine reaches a milestone.  Two
async-class events handle this — both emitted by the expander with
`TAG_ASYNC`, both unsolicited and unacknowledged:

### LED programs — one-shot vs looped

`LED_PROGRAM_RUN` carries a flags byte; bit 0 is the **repeat** flag:

| Flag | Meaning |
|---|---|
| `REPEAT` (bit 0) | Loop the program forever until `LED_PROGRAM_STOP` |
| `SYNC_START` (bit 1) | Defer start until the next `LED_PROGRAM_RUN` triggers; lets the master start multiple channels in lock-step |

When `REPEAT` is **clear**, the program runs **once**: the LED runtime
consumes events until the sequence ends, drives the final brightness,
then emits `LED_PROGRAM_DONE` as an expander→master async packet:

```
LED_PROGRAM_DONE  (0x2E)  [addr:u8][progId:u8]
```

The master uses this to chain effects — e.g. "play muzzle-flash
program, on done schedule the recoil servo move, on servo target
reached re-arm the trigger."  No master-side polling, no busy-loop.

When `REPEAT` is **set**, no completion event is ever emitted; the
program just keeps cycling.  An explicit `LED_PROGRAM_STOP` from the
master is the only way to end it.  This matches the existing
`LedEventSeq` semantics; the new wire-format detail is the flag bit
and the `LED_PROGRAM_DONE` packet.

### Servo motion — target-reached event

Each `SERVO_SET` arms the expander's trapezoidal profile to ramp
toward the new target.  When the profile converges (velocity ≈ 0 and
the current position is within `POSITION_TOLERANCE` of the target),
the expander emits:

```
SERVO_TARGET_REACHED  (0x1C)  [idx:u8][position_us:u16LE]
```

Exactly **one** event per `SERVO_SET` command — re-issuing
`SERVO_SET` mid-motion arms a new target and a new event will fire
when *that* one is reached.  The position field is the actual
post-profile position (may differ slightly from the requested target
if the profile hit a clamp at the configured min/max).

Master-side use: gear cycle becomes a state machine that awaits
`SERVO_TARGET_REACHED` from the door servos before commanding the
gear-extend servo, then awaits another from the gear servo before
re-commanding the door close — all without master-side timing math.

### Routing through HubFX

These async events arrive on whichever transport the expander is on
(USB CDC for detachable expanders, UART for on-board co-processors).
HubFX's existing type-range routing forwards them upstream with
`TAG_ASYNC` unchanged (per [13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md))
so a CLI client / Studio observer sees them with the same shape they
arrived on the expander.  Studio's per-board tab renders them via the
generic `ExpanderApi` observer chain.

### What does NOT emit completion events

- `PWM_SET_DUTY` / `PWM_SET_MOTOR` / `PWM_SET_HEATER` — outputs change
  immediately; no profile, no completion.  Master gets the regular
  ACK and that's the whole story.
- `LED_SET_BRIGHTNESS` — instant; no event.
- `SERVO_CONFIG` — calibration write, no motion implied.

## Safe-state on keepalive timeout

Expander outputs MUST fall back to a known-safe state if the master
disappears mid-effect.  The existing keepalive infrastructure
(expander expects periodic master traffic in `BoardState::EXPANDER`;
timeout default 2 seconds — see `core/core.h`) is wired to a new
`ExpanderServer::enterSafeState()` path that:

| Component        | Safe-state action                                              |
|------------------|----------------------------------------------------------------|
| Each servo       | command target = calibrated **centre** position; profile runs to neutral |
| Each PWM channel | duty = 0 regardless of mode (motor stops, heater off, generic PWM 0%) |
| Each LED         | stop running program; brightness = 0                           |
| BoardState       | back to `IDLE` — expander waits for a fresh `INIT` from the master |

The transition is **idempotent and atomic** from the protocol's view:
`enterSafeState()` runs the same code path whether triggered by
keepalive timeout, an explicit `SHUTDOWN` (0xF1) packet, or
`INIT(EXPANDER)` arriving on an expander that's already in EXPANDER
mode (which implies the master rebooted and is rejoining).

**`DIRECT` mode behaviour:** keepalive is *not* enforced when an
expander is in `INIT(DIRECT)` — that's the PC-test path where a human
is typing commands sporadically.  An explicit `SHUTDOWN` still
triggers safe state.  Closing the serial port does NOT trigger safe
state; the expander sits in DIRECT mode until power-cycled or
commanded to shut down.  This matches the existing semantic and is the
right policy for bench testing (you don't want lights cutting out
every time you Ctrl+C out of `pio device monitor`).

**Centre-position calibration:** for safe-state to mean "physically
neutral", every servo's `defaultCenter_us` must be set correctly in
the board's `ServoSpec` array.  If a particular servo doesn't have a
true neutral (e.g. a one-way trigger pull), the spec carries an
override flag so safe-state-on-that-channel commands a position that
makes physical sense (e.g. fully retracted for a trigger pulley).

## Expander-owned LED runtime — full feature set

The expander runs the **complete** LED event-sequence runtime locally
— not a stripped-down version.  Every feature in the existing
`sfx_peripherals/led/` library is exposed in the wire protocol so the
master can drive everything that the runtime can do, without
bypassing the timing-sensitive engine that has to live close to the
hardware.

| Concept | Library | Wire packet |
|---|---|---|
| Per-channel brightness 0..255 | `LedManager::ledSet` | `LED_SET_BRIGHTNESS` |
| Event-sequence load (load N typed events into a slot) | `seqClear` + `seqAdd` × N | `LED_PROGRAM_LOAD` |
| Event-sequence start | `seqStart` | `LED_PROGRAM_RUN` |
| Event-sequence stop | `seqStop` | `LED_PROGRAM_STOP` |
| Restart from event 0 | `seqRestart` | `LED_PROGRAM_RESTART` |
| Reset channel (stop + clear + off + re-enable) | `resetChannel` | `LED_RESET_CHANNEL` |
| Channel enable / disable (gate output) | `enableChannel` | `LED_ENABLE_CHANNEL` |
| Master brightness scale 0..100% | `setMasterBrightness` | `LED_SET_MASTER_BRIGHTNESS` |
| Per-channel runtime status (current event idx, time-remaining, repeat count) | `getSeqStatus → LightFxSeqStatus` | `LED_SEQ_STATUS_REQ` / `_RESP` |
| Channel snapshot (brightness + flags) | `getChannelStatus` | `LED_QUERY` / `_RESP` |
| Async program-finished notification | `LedEventSeq::onComplete` callback | `LED_PROGRAM_DONE` (TAG_ASYNC) |

The wire format for the events themselves (the bytes inside
`LED_PROGRAM_LOAD`) matches the existing `LedEventSeq` event layout
1:1 — see `LedEventType` namespace in expander.h.  Master code that
builds a program is just constructing the same byte sequence the
existing expander-side firmware already executes; we're not inventing
a new event language.

### One runtime, mixed outputs (dedicated + PWM-borrowed)

Both dedicated `LedDigital` channels and PWM-borrowed channels
(PWM in `PwmLed` mode) flow through the **same** runtime — same
event-sequence engine, same gamma curve, same fade interpolation,
same master-brightness scaling.  The difference is purely the output
sink:

```
   +─── LED_PROGRAM_RUN addr=0    ──► dedicated channel 0 ─► LedControl<TGpio>      ─► gpio.setLedBrightness()
   │
   +─── LED_PROGRAM_RUN addr=0x80 ──► extension slot 0     ─► LedControl<PwmChannelLedOutput>
   │                                                          where PwmChannelLedOutput
   │                                                          implements ILedOutput by
   │                                                          calling PwmCollection::writeDuty()
   │
   └── one shared LedEventSeq runtime ticking from `update()` per loop
```

The adapter that makes this work is
[`PwmChannelLedOutput`](../controllers/lib/sfx_peripherals/collections/pwm_channel_led_output.h)
— it implements the existing `ILedOutput` interface (`setBrightness` /
`on` / `off`) by writing a duty value into a `PwmCollection` channel
through the `IPwmLedSink` interface.  Once a `LedControl` is
attached to a `PwmChannelLedOutput` instead of a `LedControl<TGpio>`,
the same `LedEventSeq` machinery drives it.

This is what "reassign a port to act as an LED" means — not a
special simplified path, but **the same control logic** (gamma
curves, fade math, event sequencing, master brightness, channel
enable, async completion callbacks) running through a different
output adapter.

### Address bit-7 selects the pool

| Address byte | Pool |
|---|---|
| `bit 7 = 0` | dedicated `LedCollection<K, TGpio>` channels — idx 0..(K−1) |
| `bit 7 = 1` | PWM-borrowed extension slots — idx 0..(M−1) within `PwmCollection` (channel must be in PwmLed mode) |
| `addr = 0xFF` | broadcast — `LED_RESET_CHANNEL`, `LED_ENABLE_CHANNEL` apply to every channel in both pools |

Helpers in `ExpanderPacket::LedAddr` (`dedicated` / `pwmBorrowed` /
`isPwmBorrowed` / `indexOf`) keep master code from manipulating the
bit by hand.

### Lifecycle

- `PWM_SET_MODE → PwmLed` claims the channel.  `LedCollection::onPwmEnteredLedMode(idx)`
  fires; the LED runtime allocates an extension slot, instantiates
  a `PwmChannelLedOutput` against the channel, and attaches a fresh
  `LedControl` + `LedEventSeq`.  No program is loaded yet —
  master must `LED_PROGRAM_LOAD` to put events on the slot.
- `PWM_SET_MODE → not PwmLed` releases the channel.
  `LedCollection::onPwmLeftLedMode(idx)` fires; the LED runtime
  stops the slot's program (no `LED_PROGRAM_DONE` event — release is
  master-initiated, not natural completion), zeroes the output, and
  drops the slot.

### Why this matters for master orchestrators

The master's internal LightFX-effects layer treats "the muzzle-flash
LED is on a PWM channel borrowed for LED mode" and "the navigation
LEDs are on dedicated AW9523B-driven outputs" as the same problem.  It
loads programs, runs them, watches for `LED_PROGRAM_DONE` events, and
never has to know which physical pool a channel sits in.

## Reuse rule — collections are facades, not re-implementations

The `sfx_peripherals::*Collection` types (servo / PWM / LED) and
`sfx_expander` ExpanderServer are deliberately **thin** wrappers over
the existing peripheral drivers.  They handle multi-channel
templating, lifecycle alignment, protocol-format glue, and async-event
bridging — and nothing else.

| Concern | Where it lives |
|---|---|
| Servo motion profile (trapezoidal accel/decel/jerk decay, position clamping) | `ServoControl` in `sfx_peripherals/servo/` — **not duplicated** in collection |
| LED event-sequence runtime (timing, fade interp, BAM, gamma curve) | `LedManager` / `LedEventSeq` / `LedControl` in `sfx_peripherals/led/` |
| Voltage / current sensing | `ISensor` interface in `sfx_peripherals/power/sensor.h` (implemented by INA226 etc.) |
| Board identifier persistence (GUID + friendly name) | `BoardIdentity` in `sfx_expander/expander/` |
| Channel-count templating + protocol-format glue | the collections in `sfx_peripherals/collections/` |
| Wire-level dispatch + lifecycle + safe-state + status broadcast | `ExpanderServer` in `sfx_expander/` |

Collections take the underlying peripheral type as a template
parameter so substitutions are free without forking the collection
header:

```cpp
ServoCollection<N, TServoCtrl = ServoControl>
LedCollection<K, TGpio>           // delegates to LedManager<K, TGpio>
PwmCollection<N>                   // takes ISensor* arrays for sensing
```

## Unified status — broadcast + sync request

Three packets cover every status-fetch use case:

| Packet | ID | Use |
|---|---|---|
| `EXPANDER_STATUS_BROADCAST` | `0x06` | expander → master/PC, `TAG_ASYNC`; emitted periodically per `EXPANDER_STATUS_RATE` |
| `EXPANDER_STATUS_RATE` | `0x07` | `[hz:u8][kindsBitmask:u8]` — set rate (0..10 Hz; 0 disables) + filter |
| `EXPANDER_STATUS_REQ` | `0x08` | `[kindsBitmask:u8]` → returns the same payload synchronously, tagged with the master's current command tag |

`COMPONENT_LIST_REQ` (0x01) is the canonical **re-enumeration**
mechanism — its response carries the **live** runtime modes of
every component (`describe()` reads `_runtime[i].mode` per call),
so re-querying after a `PWM_SET_MODE` returns the new fingerprint
immediately.  No separate "re-enumerate" packet needed.

### Payload layout (port-id-tagged entries)

```
   header:        boardState:u8, mode:u8, uptime_ms:u32, freeRam:u32                 = 10 bytes
   servoCount:u8 + per-servo  [port_id][pos:u16][target:u16][vel:i16][flags:u8]      = 9 bytes/entry
   pwmCount:u8   + per-pwm    [port_id][mode][duty:u16][V_mV:i16][I_mA:i16]
                              [stallFlags][peak_mA:u16]                              = 11 bytes/entry
   ledCount:u8   + per-led    [port_id][brightness][progState][progId]               = 4 bytes/entry
```

Each per-component entry is prefixed with a **`PortId`** byte:
`(kind:3 << 5) | (idx:5)`.  Master code can match a status field
to its source port from a single byte without tracking which
section it appeared in.  Limits: 8 component kinds, 32 channels
per kind.

`ExpanderPacket::PortId` namespace provides `make(kind, idx)` /
`kind(pid)` / `index(pid)` constexpr helpers + the kind enum
(`Servo`, `Pwm`, `LedDed`, `LedPwm`).

### Per-kind filtering

`ExpanderPacket::StatusKinds::*` bitmask byte controls which sections
appear in both periodic broadcasts and on-demand requests:

| Flag | Effect |
|---|---|
| `ALL` (0x00)        | include all kinds (default) |
| `SERVO` (0x01)      | include servo section only |
| `PWM` (0x02)        | include PWM section only |
| `LED` (0x04)        | include LED section only |
| `HEADER_ONLY` (0x80)| skip every per-component section |

Filters compose: e.g. `0x03` = servo + PWM only (no LED block).
Cleared sections still emit a `count = 0` byte so the parser shape
is uniform.

### Sizing

Worst-case at 32 servos + 32 PWMs + 32 LEDs:
`10 + (1 + 32×9) + (1 + 32×11) + (1 + 32×4) = 778 B` — above the
512 B COBS payload limit.  Practical boards (≤ 8 channels per kind)
come in well under: 6 servos + 8 PWMs + 8 LEDs ≈ 187 B.  For
extreme channel counts the master uses `EXPANDER_STATUS_REQ` with a
filter to fetch one section at a time.

## Library skeleton (landed)

```
controllers/lib/sfx_peripherals/collections/
├── components/
│   ├── component_kind.h        ← ComponentKind enum, ComponentInfo wire struct, *Flags namespaces
│   ├── servo_collection.h      ← ServoCollection<N> wrapping ServoControl
│   ├── pwm_collection.h        ← PwmCollection<M> with mode-mutable channels + ISenseProvider
│   └── led_collection.h        ← LedCollection<K, TGpio> wrapping LedManager
└── identity/
    └── board_identity.h        ← BoardIdentity — GUID + /board.yaml friendly name

controllers/lib/sfx_serial/serial/expander/
└── expander.h                  ← ExpanderPacket::*, ExpanderError::* — generic protocol IDs

controllers/lib/sfx_expander/
└── expander/
    └── expander_server.h       ← templated ExpanderServer<TServos, TPwms, TLeds>
```

> **Note:** these paths describe the *target* state.  The
> uncommitted skeleton currently on disk uses the older `sfx_slave/`
> / `serial/slave/` / `board_identifier.h` names; those rename in the
> first PR of this refactor.

What's still TODO at the lib level (subsequent sessions):

- `servo_collection.ipp` — implementation against `ServoControl`
- `pwm_collection.ipp`   — implementation; needs a small new `PwmOutput` driver in `sfx_peripherals/pwm/` because `pwm_control.h` is currently RC PWM **input** measurement, not output
- `led_collection.ipp`   — straightforward facade over `LedManager`
- `expander_server.ipp`  — packet dispatch tables
- Go-side: `app/go/protocol/expander/`, `app/go/api/expander.go`, generic engine handler

## Where the high-level effect logic lives — master-side, internal

All high-level orchestration that used to live in expander firmware
(or in `controllers/lib/sfx_boards/<board>/`) moves master-side, but
as **internal HubFX functionality** rather than a protocol surface.
Concretely:

- The HubFX firmware owns plain C++ classes that consume the generic
  `ExpanderApi` and translate user/RC events into component-level
  commands.  These are not policies of `CoreCommandServer<...>` and
  do not expose their own packet range.
- Studio + CLI observe master-side typed events (calibration progress,
  effect-state changes) through the existing observer chain — they
  are not over-the-wire protocol packets.
- The HubFX has no notion of GunFX / LightFX / GearControl *protocols*
  any more; only of GunFX / LightFX / GearControl *effect roles*
  driven over the generic expander wire.

The directory shape inside HubFX (e.g. `controllers/hubfx/esp32s3/src/effects/<role>/`)
is an implementation detail of the HubFX firmware, not a protocol
contract.  Per [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md), effects
are explicitly **out of scope** for the policy-based composition of
`CoreCommandServer` — they remain internal.

`controllers/lib/sfx_boards/` (the per-board wire-wrapper library) is
**deleted entirely** during the migration — all per-expander wire
access goes through the generic `ExpanderApi` that replaces it.

## Per-board migration plan

Each migration is a self-contained pull-request-sized unit of work.
Order: smallest blast radius first, so the toolchain catches mistakes
on simpler boards before tackling the LED-heavy LightFX.

### Step 0 — preconditions

Before migrating any board:

1. Library implementations (`*.ipp` for the three collections + expander
   server) must be complete and unit-tested against a virtual board.
2. Go SDK has a generic `ExpanderApi` matching the new wire format.
3. Studio has at least a placeholder "Generic Expander" tab that
   renders the component fingerprint.
4. Board GUID / port-GUID surfaces (per
   [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md)) are wired into
   IDENTIFY and `COMPONENT_LIST_RESP` so the master can disambiguate
   multiples and target individual ports stably.

### Step 1 — GunFX (smallest surface)

**Why first:** smallest peripheral set (1 servo, 1 trigger PWM, smoke +
fan PWM, INA226 readout, RC trigger input).  Existing high-level logic
(rate-of-fire state machine, smoke automation) is the smallest of the
three boards.

**Component layout (proposed):**

```cpp
ServoCollection<1>      servos;   // gun servo (recoil)
PwmCollection<3>        pwms;     // [0] trigger motor (PwmMotor)
                                   // [1] smoke heater  (PwmHeater, voltage+current sense via INA226 idx 0)
                                   // [2] smoke fan     (PwmGeneric)
LedCollection<0, …>     leds;     // none
```

**Migration steps:**

1. Rewrite `controllers/gunfx/pico/src/gunfx_pico.ino` to instantiate
   the collections + `ExpanderServer` and nothing else.  Delete the
   gun-trigger state machine, rate-of-fire scheduling, smoke-automation
   logic — all that lives in the master from now on.
2. Delete `controllers/lib/sfx_boards/gunfx/`.
3. Delete `controllers/lib/sfx_serial/serial/gunfx/gunfx.h` (mark the
   0x01-0x2F packet range deprecated in `instructions/README.md`).
4. Delete `app/go/protocol/gunfx/`, `app/go/api/gunfx.go`,
   `app/go/engine/handlers/gunfx/`.
5. Rebuild the high-level "fire the gun" semantics in the **master**:
   the HubFX firmware (or the PC's CLI/Studio) drives PwmMotor for the
   trigger, schedules smoke-heater + fan via PwmHeater/PwmGeneric, and
   reads RC input itself (RC inputs stay master-side per `inputs/`).
6. Tests: existing CLI commands (`gun:trigger`, `gun:smoke`, etc.)
   become composite commands on the master that orchestrate the
   expander's component-level actions.

**Packet-range handling:**  HubFX type-range routing
([13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md)) currently
routes 0x01-0x2F → GunFX expander.  After migration the GunFX
expander listens on the generic 0x10-0x3F range; we'll need a small
router update to recognise that the expander at `ExpanderType::GunFx`
now answers on the new range.  Done in the same PR as the migration.

**Effort estimate:** 1-2 days of expander-side firmware + 2-3 days of
master-side reconstruction.

### Step 2 — GearControl (mechanical bias, similar PWM count)

**Why second:** more components than GunFX (multiple servos for gear
deploy/retract + steering yaw, plus optional motor channels), but the
high-level state machine — gear sequencing, brake interlocks — is
simpler than LightFX's program runtime.

**Component layout (proposed):**

```cpp
ServoCollection<6>      servos;   // gear bay doors (×4) + steering yaw + spare
PwmCollection<2>        pwms;     // [0] retract motor (PwmMotor, current sense)
                                   // [1] brake / aux  (PwmGeneric)
LedCollection<0, …>     leds;     // none (gear status via master's indicators)
```

**Migration steps:** same shape as GunFX.  Deletions: `sfx_boards/gearcontrol/`,
`serial/gearcontrol/gearcontrol.h`, the Go-side gearcontrol package,
`engine/handlers/gearcontrol/`.  Hub-side gear sequencing (the
deploy/retract choreography, brake interlocks, position monitoring) is
rebuilt as a composition of ServoCollection + PwmCollection commands.

**Battery monitoring:**  GearControl currently has a
`BatteryServerT<Ina226Battery>` for cutoff logic.  In the new model,
battery cutoff is a **master-side** policy: the master reads the
expander's voltage/current via PWM channel sensing
(PwmFlags::HAS_VOLTAGE_SENSE) and decides when to assert cutoff,
sending PWM_SET_DUTY=0 to the relevant channels.  Removes board-side
state machines from the expander.

**Effort estimate:** 2-3 days expander + 3-4 days master.

### Step 3 — LightFX (LED-heavy, biggest peripheral count)

**Why last:** the LED light-program runtime is the most complex
expander feature, and it stays with the expander (the master shouldn't
have to beat-track 8 channels' worth of event sequences over USB).
The event-sequence runtime is preserved 1:1 in `LedCollection`; the
only expander-side high-level logic that goes away is the **landing-light
group binding** — that's master orchestration in the new world.

**Component layout (proposed):**

```cpp
ServoCollection<2>             servos;  // landing-gear servo + nav-light servo
PwmCollection<0>               pwms;    // none
LedCollection<8, NativeGpio>   leds;    // 8 channels, native GPIO PWM
```

**Migration steps:**

1. Delete `landing-light groups` from the expander firmware
   (`LANDING_LIGHT_BIND` packet, group state machine).  The master
   sends per-channel `LED_SET_BRIGHTNESS` / `LED_PROGRAM_RUN` commands
   in the new model — the master already has the program graph in
   `/lightfx.yaml`.
2. Replace the entire expander firmware with the ExpanderServer
   template bound to ServoCollection<2> + LedCollection<8, NativeGpio>.
3. Move the `pushLightFxConfigToSlave()` translation in HubFX (rename
   to `pushLightFxConfigToExpander()`) to emit expander-protocol
   packets instead of the legacy `LIGHTFX_*` ones.
4. Delete `sfx_boards/lightfx/`, `serial/lightfx/lightfx.h`,
   `app/go/protocol/lightfx/`, `app/go/api/lightfx.go`,
   `engine/handlers/lightfx/`.

**Effort estimate:** 3-4 days expander (LedCollection wiring + program
loading shape change) + 4-5 days master (`pushLightFxConfigToExpander`
rewrite + Studio LightFX-tab plumbing).

### Step 4 — HubFX (master) cleanup

After all three expanders migrate, HubFX gains:

- New per-role orchestrators (replacing `pushLightFxConfigToSlave`
  with `pushLightFxComponentsToExpander`, etc.) that translate the
  master's high-level config into low-level component commands.
- `ExpanderRegistry` (renamed from `SlaveManager`) simplified — the
  IDENTIFY exchange now also pulls the ComponentList AND the
  per-port-GUID list, so an expander's capabilities and stable port
  identities are known the moment it attaches.  The legacy
  `pushXxxConfigToSlave` per-board functions collapse into one
  templated `pushBoardComponents(boardCfg, expander)`.
- Type-range routing (0x10-0x3F is the only expander range) becomes
  trivial: every inbound packet in that range goes to whichever
  expander matches the requested `(ExpanderType, BoardGuid)` tuple
  (see [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md)).

**Effort estimate:** 2-3 days of master cleanup, depends on what gets
moved master-side during the expander migrations above.

## Testing strategy

The existing `tests/virtual_board/` simulator is the verification
surface.  Add a `tests/virtual_board/boards/generic/` board kind that
implements the new expander protocol; subsume the existing
`boards/lightfx/`, `boards/gunfx/`, `boards/gearcontrol/` simulators
into one parameterised generic-board simulator with different
ComponentInfo fingerprints.  Existing event-timing tests
(`go test ./boards/lightfx/`) get ported once per expander migration.

A `tests/hw/generic_expander_hwtest/` firmware (parallel to the
existing `hubfx_led_hwtest/`) exercises real hardware with
manual-brightness line-based UX so the expander can be brought up
before the master is ready.

## Cutover — no compatibility window

The migration is a hard cutover, not a rolling deployment.  In a
single PR per board (and a final PR for HubFX cleanup) we:

1. Delete the per-board protocol header
   (`controllers/lib/sfx_serial/serial/<board>/<board>.h`).
2. Delete the per-board library directory
   (`controllers/lib/sfx_boards/<board>/`).
3. Delete the per-board Go package
   (`app/go/protocol/<board>/`, `app/go/api/<board>.go`,
   `app/go/engine/handlers/<board>/`).
4. Replace the expander firmware in `controllers/<board>/pico/` with
   the `ExpanderServer` template.
5. Update HubFX's per-role push function to emit the new packet IDs.
6. Update Studio's board tab to bind to the generic `ExpanderApi` with
   the appropriate fingerprint.

No `ExpanderCapability::GENERIC_PROTOCOL` flag, no per-expander
fallback in the master, no old packet ID emitted anywhere after the
migration PR lands.  Any deployed expander running the legacy firmware
**must** be re-flashed before it can talk to the post-migration master
— this is a breaking change at the wire level (MAJOR version bump on
master and each expander).

Legacy packet ranges (0x01-0x2F, 0x40-0x5F, 0x60-0x7F) become
**permanently retired** rather than reserved — the next architectural
need that wants more wire-range space can claim them.

## Pivot non-goals

What this refactor explicitly does NOT do:

- Change the wire-level transport (still COBS / CRC-8 over USB CDC).
- Change the master-side firmware architecture (HubFX stays as is).
  In particular, the master-side internal effects layer is *not* a
  protocol — see [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md).
- Change the YAML config schema for the master (`/lightfx.yaml`,
  `/gunfx.yaml`, etc., remain).  Only the expander-side `/board.yaml`
  is new (and trivially small — just the friendly name).
- Break Studio compatibility — board tabs continue to exist; their
  back-end just shifts from per-expander APIs to a single generic API
  with a fingerprint-driven view layer.

## Cross-references

- [01-ARCHITECTURE.md](01-ARCHITECTURE.md) — system overview (refresh
  needed once the migration completes to remove STANDALONE language).
- [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) — old 7-file pattern
  (will be superseded by "instantiate ExpanderServer with your
  collection layout" once the migration completes).
- [13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md) — type-range
  routing on HubFX; needs a small update for the new 0x10-0x3F range
  and the `(ExpanderType, BoardGuid)` keying.
- [14-ONBOARD-COPROCESSOR.md](14-ONBOARD-COPROCESSOR.md) — the C3
  on-board co-processor pattern uses the same generic ExpanderServer
  with a different transport.  Excellent fit: a co-processor is just
  another generic expander that happens to be on UART instead of USB
  CDC.
- [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md) — anatomy
  of a single expander board's firmware (core + expander protocol
  surface, persistence rules, design contract).
- [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md) — `CoreCommandServer`
  composition, deterministic board GUID, per-port GUIDs, storage
  backends as policies; this doc explains the *protocol* of the
  expander, doc 17 explains how *both master and expander* compose
  their shared system services.

## Rename map (for future code-level migration)

When the refactor PRs actually land, the live code changes from the
column on the left to the column on the right.  Until then, the code
on disk still uses the left column; this and other forward-looking
docs use the right column.

| Pre-refactor (current code) | Post-refactor (docs use this) |
|---|---|
| `BoardState::SLAVE` | `BoardState::EXPANDER` |
| `CoreCapability::SLAVE_BUS` | `CoreCapability::EXPANDER_BUS` |
| `INIT(SLAVE)` | `INIT(EXPANDER)` |
| `controllers/lib/sfx_slave/` | `controllers/lib/sfx_expander/` |
| `controllers/lib/sfx_serial/serial/slave/` | `controllers/lib/sfx_serial/serial/expander/` |
| `SlaveServer<...>` | `ExpanderServer<...>` |
| `SlavePacket::*` | `ExpanderPacket::*` |
| `SlaveError::*` | `ExpanderError::*` |
| `SlaveType::{GunFx, LightFx, GearControl}` | `ExpanderType::{GunFx, LightFx, GearControl}` |
| `SlaveApi` (Go) | `ExpanderApi` (Go) |
| `SLAVE_STATUS_BROADCAST` / `_REQ` / `_RATE` packets | `EXPANDER_STATUS_BROADCAST` / `_REQ` / `_RATE` |
| `SlaveManager` / `UsbRegistry` of slaves | `ExpanderRegistry` keyed by `(ExpanderType, BoardGuid)` |
| `BoardIdentifier` (lib) | `BoardIdentity` (lib) — adds GUID alongside friendly name |
| `pushXxxConfigToSlave(cfg, client)` | `pushXxxConfigToExpander(cfg, client)` |
| `Engine.SetControllerType(SlaveType::…)` | `Engine.SetControllerType(ExpanderType::…)` |
| `slave-side` prose | `expander-side` prose |
| `slave board` prose | `expander board` prose |

Board names themselves (GunFX / LightFX / GearControl / HubFX) do
**not** change — they remain the role names of specific expander
boards.  The only thing renaming is the abstract category and the
infrastructure that supports it.
