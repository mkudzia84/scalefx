# GearControl effect orchestrator (master-side)

> **Status: descriptive scaffolding.** Implementation lands during
> Step 2 of the slave migration (see
> [`instructions/15-GENERIC-SLAVE-REFACTOR.md`](../../../../../instructions/15-GENERIC-SLAVE-REFACTOR.md)).
> Headers in this directory describe the master-side responsibilities
> for landing-gear sequencing.  No working code yet.

In the post-pivot architecture, a "GearControl board" is a generic
slave exposing 6 servos (gear bay doors + main gear leg + steering
yaw + spare) + 2 PWM channels (retract motor with current sense,
brake / aux) + INA226 sensing.  The slave does the per-axis motion
profile; this orchestrator decides **which axis moves when**.

## Effects covered

### Gear cycle — the showcase async-event use case

Deploying the gear is a multi-step sequence with per-step waits for
slave-side completion.  This is exactly the case the
`SERVO_TARGET_REACHED` async event was added for.

```
   master                              slave
   ──────                              ─────
   1. SERVO_SET door_left  open  ───►
   2. SERVO_SET door_right open  ───►
                                  ◄─── SERVO_TARGET_REACHED door_left
                                  ◄─── SERVO_TARGET_REACHED door_right
   3. SERVO_SET gear extend     ───►
                                  ◄─── SERVO_TARGET_REACHED gear
   4. SERVO_SET door_left  close ───►
   5. SERVO_SET door_right close ───►
                                  ◄─── (target-reached events)
   6. (optional) PWM_SET_DUTY brake → 0  // release brake
```

No master-side timers.  No `delay()` calls.  Each step waits for the
hardware to actually arrive before issuing the next.  A retract is
the same sequence in reverse.

### Steering yaw

Direct servo control bound to an RC channel — the master maps RC
input to `SERVO_SET` calls on the yaw servo.  No state machine
required; the servo's local profile handles the smooth motion.

### Brake interlocks

The brake is a PWM channel; the orchestrator drives its duty:

| State | Brake duty |
|---|---|
| Gear stowed | 1000 (full lock) |
| Gear deploying / retracting | 0 |
| Gear deployed + RC-brake-toggle high | 1000 |
| Gear deployed + RC-brake-toggle low | 0 |

The master is also responsible for **never extending gear while the
brake is fully engaged** — interlock validated before issuing the
sequence.

### Battery cutoff (moved master-side)

Pre-pivot, GearControl had a `BatteryServerT<Ina226Battery>` running
its own cutoff state machine.  Post-pivot, the master:

1. Reads battery voltage / current via `PWM_QUERY` against the
   sensing-enabled PWM channel (or via a dedicated ADC channel
   exposed by the slave's COMPONENT_LIST).
2. Compares against the configured chemistry / cell-count cutoff
   thresholds (per-cell mV table for LiPo / LiFe / NiMH).
3. On low-voltage detection, commands the slave to safe-state any
   high-current actuators (retract motor → `PWM_SET_DUTY 0`, brake
   → engaged).
4. Surfaces an alarm in the HubFX STATUS broadcast so Studio /
   CLI can render it.

The slave's keepalive watchdog still acts as a fail-safe — if the
master crashes mid-deploy, the slave parks every servo at neutral
and zeroes every PWM after the timeout.

### Diagnostics + state surface

| Feature | Source | Wire |
|---|---|---|
| Gear cycle phase | master state machine | broadcast in HubFX STATUS payload |
| Per-servo target / position | `SERVO_QUERY` cached + `SERVO_TARGET_REACHED` async | STATUS field |
| Retract-motor current | `PWM_QUERY` (sense-enabled channel) | STATUS field |
| Battery voltage / cutoff state | INA226 readback + master cutoff policy | STATUS field |

## What's gone vs the legacy GearControl firmware

- **Slave-side gear sequencer** (open-doors → extend → close
  choreography) — replaced by master orchestrator using
  `SERVO_TARGET_REACHED` chaining.
- **Slave-side battery cutoff** — replaced by master-side INA226
  polling + cutoff policy + safe-state command.
- **`GearControlClient` / `GearControlServer` wire wrappers** in
  `lib/sfx_boards/gearcontrol/` — deleted; replaced by generic
  `SlaveApi`.
- **`GearControlPacket` / `GearControlError`** in
  `lib/sfx_serial/serial/gearcontrol/gearcontrol.h` — deleted.

## What stayed slave-side (and why)

- **Per-servo trapezoidal motion profile** — same reason as LightFX;
  smooth motion needs predictable per-tick advance.
- **Keepalive safe-state watchdog** — no master, no decisions, no
  motion: this single piece of "intelligence" stays slave-side as
  the safety floor.

## State machines (master-side, ported from the legacy slave)

The two slave-side sequencers from `controllers/gearcontrol/pico/src/`
have been promoted to master-side helpers under this directory.  They
keep the exact mode set + op-queue layout of the originals — only the
emission / completion paths differ:

| Concern | Slave-side (legacy) | Master-side (here) |
|---|---|---|
| Servo command emission | direct `ServoControl::setTarget()` | `ServoCommandFn` stub → SlaveApi (deferred) |
| Servo completion | poll `ServoControl::atTarget()` from `update()` | `notifyTargetReached()` driven by `SERVO_TARGET_REACHED` async |
| Motor command emission | direct `analogWrite` / DcMotor | `MotorFn` stub → SlaveApi PWM (deferred) |
| Stall detection | local `StallDetector` ticked from `update()` | `StallStateFn` stub → latched from `PWM_STALL` async (deferred) |

### `door_sequencer.{h,cpp}`

Mode-aware door servo sequencing with the same five modes as the
slave-side original:

| Mode | Behaviour |
|---|---|
| `NONE`        | No door servos; sequence completes immediately |
| `SINGLE`      | One door servo (door 0 only) |
| `DUAL_SYNC`   | Two doors, commanded simultaneously (default) |
| `DUAL_DELAY`  | Two doors, door 1 commanded after `delay_ms` |
| `DUAL_SEQ`    | Two doors, door 1 commanded after door 0 reaches target |

Each gear has a `doorPreDeployMode` (open before deploy motor / close
after retract motor) and a `doorPostDeployMode` (close after deploy /
open before retract).  `postDeployMode == NONE` is the "doors stay
open after deploy, retract skips the pre-retract open" shortcut.

For `DUAL_DELAY` / `DUAL_SEQ`, doors **open 0→1 and close 1→0** —
inner door opens first, closes last (mimics real aircraft).

### `gear_sequencer.{h,cpp}`

Op-queue based deploy/retract sequencing, identical layout to the
slave version:

```
OPEN_DOORS → [SYNC_BARRIER] → RUN_MOTOR → [SYNC_BARRIER] → CLOSE_DOORS
```

- Preemption: deploy during retract (or vice versa) rebuilds the
  queue mid-flight and handles the in-flight op safely (motor stop,
  door reverse, doors-already-open shortcut).
- Sync mode: a coordinator can hold the sequence at either barrier
  and release with `advanceSyncPhase()`.
- Phase callback fires on every op transition + on completion
  (success/error) so the orchestrator can update broadcast state.

### Multi-gear coordination — `GearControlEffect::coordMode`

The orchestrator owns one DoorSequencer + one GearSequencer per
gear (currently 2 — front + rear).  `GearCoordMode` selects how the
two gears advance through the cycle:

| Mode | Doors | Motor | Doors close |
|---|---|---|---|
| `Independent` | each gear runs solo | independent | independent |
| `DoorSync`    | both gears wait for both door-open ops | independent motors | both gears wait for each other to close |
| `FullSync`    | both gears wait for both door-open ops | both gears wait at the post-motor barrier | both gears close together |
| `Sequenced`   | front cycles fully, then rear cycles | n/a | n/a |

Sync barriers are released by the orchestrator's
`releaseSyncBarriersIfReady()`, which checks every gear's `step()`
and calls `advanceSyncPhase()` on each once they all line up.

## Scaffolding files in this directory

- [`door_sequencer.{h,cpp}`](door_sequencer.h) — door servo sequencing
  state machine (master-side port).
- [`gear_sequencer.{h,cpp}`](gear_sequencer.h) — op-queue-driven
  deploy/retract state machine.
- [`gearcontrol_effect.{h,cpp}`](gearcontrol_effect.h) — orchestrator:
  per-gear sequencer pair, multi-gear coordinator, RC + battery
  scaffolding, async-event hooks.

> **Outbound signal emission is stubbed.** All three files use
> `std::function` callbacks for the actual SlaveApi wiring.  Until
> the integration step lands those bind to local lambdas that no-op
> the wire emission — the state machines are otherwise fully live.
