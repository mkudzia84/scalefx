# 29 — GearControl: retractable-undercarriage door sequencing

> **Status:** how-to / workflow &middot; **Read when:** working on GearControl door servo sequencing, multi-channel gear coordination, or the Studio Gear tab.
> **TL;DR:** Adds door-servo sequencing + multi-channel coordination + the Studio Gear tab on top of the shipped motor-only gear FSM; §6a decisions #1–5 are landed in firmware (build #788) with `SERVO_MOTION_DONE` as the door-completion signal and `/gearcontrol.yaml` v2 carrying the coord/door config.

Plan for the full retractable-landing-gear feature on the **HubFX master** +
its **ScaleFX Studio** panel. The motor-only gear FSM already ships; this adds
**door servo sequencing**, **multi-channel coordination**, and the **Studio Gear
tab**. Door sequencing was prototyped in the old codebase — port it, don't
reinvent (see §7 archive map).

> **Status (firmware landed 2026-06-07):** §6a LOCKED decisions #1–5 are
> implemented in firmware (build #788). SERVO_MOTION_DONE (`0x56`) is the
> monitored door-completion signal; gear calibration (`0xD8/0xD9`) is removed;
> `GearDef` carries `doors[2]`/`openMode`/`doorDelayMs`/`closePolicy`;
> `/gearcontrol.yaml` v2 parses `coord` + per-gear `doors`/`door_mode`/
> `door_delay_ms`/`close_policy`; the op-queue brackets the motor seek with
> door legs (`DoorSequencer`); all 4 coord modes (`independent`/`door_sync`/
> `full_sync`/`sequenced`) + the multi-gear barrier coordinator ship; `GEAR_ALL`
> drives every channel; `GEAR_STATUS_RESP` + `GEAR_PHASE_EVENT` carry the
> trailing `subPhase` (Rule 11).
>
> **Studio panel landed 2026-06-07:** `GearPanel.svelte` + `gear.ts`
> (`gearConfigSource`, Rule 46) + `app_gear.go` (LoadGearConfig/SaveGearConfig v2
> YAML; GearDeploy/Retract/Stop/All/Reset/Status; `gear:phase` live stream). Tab
> gated on hubfx + gearcontrol domain. Multi-channel editor: motor picker (R49) +
> ≤2 door servos (ServoWidget+calib) + door-pairing/close-policy radios + coord
> selector + fleet & per-channel Deploy/Retract action-toggles + live phase·
> subphase pill. wails build + vitest green.
>
> **Motor calibration popup landed 2026-06-13:** `MotorCalibrationDialog.svelte`
> + `MotorWidget.svelte` + `motor_calibration.ts` / `motor_status.ts` +
> `app_gearmotor.go` (GUID-aware `GearMotor*` Wails methods routing via
> `c.Role(guid)`, Rule 58 — reuse the `Diag*` structs + `awaitEndstop`). The
> strut motor card now shows a **live current-mA / duty / position / stall**
> readout (polled per configured motor on the status timer) + a **⚙ Calibrate
> motor…** button → modal with live status, A→B calibrate sweep (travel-time +
> peak current), To-End-A/B, hold-jog, and LiveRatio/Fixed **stall-guard**
> tuning. `Save to strut` commits the working duty (deploy +, retract −) +
> suggested travel timeout (full-stroke × 1.5) into the channel draft. NO new
> wire packets — it drives the EXISTING BiDcMotor role packets directly (no
> `GEAR_MANUAL_HOLD` needed: the operator calibrates while the strut is idle, so
> the FSM isn't driving the motor). Stall-guard is a LIVE push (not yet a YAML
> field). wails build + vitest + svelte-check (no new errors) green.
>
> **Remaining:** (a) **hardware bench validation** of the full sequence + the
> calibration sweep on a wired hub + expander + motor + servos; (b) persist the
> stall-guard config to `/hubfx.yaml ports[]` (today it's re-applied per session);
> (c) a `GEAR_MANUAL_HOLD` interlock IF calibration is ever wanted while the FSM
> is mid-cycle (not needed for the idle-strut bench flow today).

> Scope note: the gear MOTOR lives on a GearControl **expander** (BiDcMotor role
> on an H-bridge); the door SERVOS are ServoActuator roles (expander or hub). All
> sequencing logic runs on the **hub** (Rule 17 effect-service); the hub forwards
> role commands to the GUID'd ports via the Topology service. Effects never touch
> raw `millis()` — use `sfx_core::EffectClock` (Rule 40).

---

## 0. Current state (audit)

**Already on the hub** (`controllers/hubfx/esp32s3/src/effects/gearcontrol/`):
- `GearControlServicePolicyT` — array of per-gear FSMs, claims the BiDcMotor role,
  drives deploy/retract by seeking to the endstop (stall-confirmed), 3-leg
  `GEAR_CALIBRATE`, `GEAR_RESET`, timeout backstop, and forwards DEPLOYED/RETRACTED
  to `LandingLightService` for gears that own a light group.
- Phases: `Unconfigured/Retracted/Deploying/Deployed/Retracting/Error/Calibrating`.
- Wire `0xBE–0xC6` + `0xD7–0xD9` (DEPLOY/RETRACT/STOP/ALL/STATUS/LIST/RESET/
  CALIBRATE/CALIB_CANCEL) — full Go mirror in `app/go/protocol/gear/gear.go`.
- Config `gearcontrol_config.h`: `GearDef { id, name, motor:PortRef, deployDuty,
  retractDuty, timeoutMs }`, `/gearcontrol.yaml`, `applyGearControlConfig`.

**Missing (this plan):**
- Door servos per gear (≤2) + the open→motor→close bracket sequencing.
- Door-pair modes (sync / delay / one-after-another) + post-deploy close policy
  (both / one / none).
- Cross-channel coordination (sync the door-phase + motor-phase across all gears,
  or run independent).
- Studio Gear tab (multi-channel editor + motor calibration popup + door servo
  widgets + live deploy/retract).

**Archive to port** (`controllers/archive/hubfx-esp32s3/src/effects/gearcontrol/`
+ `sfx_*_legacy/`): `door_sequencer.{h,cpp}`, `gear_sequencer.{h,cpp}` (op-queue),
`gearcontrol_effect.{h,cpp}` (multi-gear coordinator), `stall_detector.h`. See §7.

---

## 1. Architecture decisions (decide before coding)

1. **Extend, don't fork.** Add door sequencing INTO the existing
   `GearControlServicePolicyT` + `Gear` FSM. The motor leg already works; bracket
   it with door open/close legs.

2. **Op-queue per gear** (from the archive): a deploy is the ordered list
   `OPEN_DOORS → [SYNC] → RUN_MOTOR(deploy) → [SYNC] → CLOSE_DOORS(policy)`. Retract
   is the reverse: `OPEN_DOORS → [SYNC] → RUN_MOTOR(retract) → [SYNC] → CLOSE_DOORS`.
   Each leg must **finish before the next starts** (the user's hard requirement).

3. **Completion signals (no new round-trips):**
   - **Servo leg done** = deterministic travel time computed from the servo's motion
     profile (the hub holds it: `(|Δus| / maxSpeed) + accel/decel ramps`) + a settle
     margin, ticked by `EffectClock`. Optional early-exit via a servo status poll is
     a later refinement; the timer alone is robust and matches the archive's
     `DOOR_TRAVEL_TIME_ms` + `SETTLE_TIME_ms`.
   - **Motor leg done** = the existing `BIMOTOR_ENDSTOP_RESULT` (reached / timeout)
     the gear FSM already consumes — no change.

4. **Door modes** (port the archive enum): `NONE / SINGLE / DUAL_SYNC /
   DUAL_DELAY(delayMs) / DUAL_SEQ`. `DUAL_SEQ` = door-1 after door-0 *completes*;
   `DUAL_DELAY` = door-1 after a fixed delay. The user's two pairing options map to
   `DUAL_SEQ` and `DUAL_DELAY`.

5. **Post-deploy close policy** = the user's "both close / one closes / none
   close". Model as a per-gear `closePolicy ∈ {both, first_only, none}` applied to
   the CLOSE_DOORS leg after a deploy (retract re-opens whatever was closed). The
   archive expressed this as `preDeployMode` (always open before motor) +
   `postDeployMode` (NONE = stay open). Keep both: `openMode` (the pairing for the
   open leg) + `closePolicy` (which doors close after deploy).

6. **Cross-channel coordination** (top-level toggle the user asked for):
   `coordMode ∈ {independent, sync}`. `sync` inserts SYNC barriers so **all** gears
   open their doors together, then **all** run motors, then **all** close — via the
   archive's barrier-release (advance every gear's phase only once *all* gears sit
   at the barrier). `independent` = no barriers (today's behaviour). (Archive also
   had `DoorSync`/`FullSync`/`Sequenced`; ship `independent` + `sync`=FullSync first,
   leave the others as future enum values.)

7. **Servo direction (normal/reverse)** is a property of the door servo's ROLE
   (Rule 42/44 — the ServoActuator's `reversed` profile flag), NOT a gear config
   field. The gear sequencer commands an INTENT (`open`/`closed` as a normalized
   position via `SERVO_SET_POS_NORM`, Rule "servo intent is normalised"); the role
   honours `reversed`. So "reverse" in the Gear panel just toggles the servo
   profile's `reversed` (same control as everywhere else).

8. **Motor calibration popup** drives the motor through the **hub→expander role
   forwarding** (Topology `sendRoleCommand` to the motor's GUID'd port), reusing the
   BiMotor role commands the Diagnostics tab uses (`move-to-end`, `seek`,
   `set-guard` LiveRatio+ceiling, `status`). While the popup is open the gear must
   **yield the motor** (a new `GEAR_MANUAL_HOLD`/`GEAR_MANUAL_RELEASE`, or reuse the
   `Calibrating` phase) so the role layer can drive it without the FSM fighting.
   New Studio Wails bindings wrap the topology-forwarded BiMotor commands (the
   existing `DiagBiMotor*` are direct-to-expander; the hub path needs GUID'd
   variants — see §3.4).

---

## 2. Firmware plan (HubFX)

### 2.1 Config schema (`gearcontrol_config.h` + `/gearcontrol.yaml`)
Extend `GearDef` (append-only, Rule 11):
```cpp
struct DoorDef {
    PortRef  servo;            // ServoActuator role port (hub or expander); empty = none
    uint16_t openNorm  = 0;        // normalized [0..10000] open position  (role honours reversed)
    uint16_t closeNorm = 10000;    // normalized closed position
    uint16_t travelMs  = 0;        // 0 = derive from the servo profile + settle
};
struct GearDef {              // existing fields kept …
    uint8_t  id; char name[16]; PortRef motor; int16_t deployDuty, retractDuty; uint32_t timeoutMs;
    // NEW:
    DoorDef  doors[2];        // ≤2 door servos
    uint8_t  numDoors   = 0;
    uint8_t  openMode   = DUAL_SYNC;   // door-pair sequencing for the OPEN leg
    uint16_t doorDelayMs= 500;         // DUAL_DELAY only
    uint8_t  closePolicy= BOTH;        // post-deploy: BOTH | FIRST_ONLY | NONE
};
struct GearControlConfig { bool enabled; uint8_t coordMode; GearDef gears[kMaxGears]; uint8_t numGears; };
```
`/gearcontrol.yaml` gains `coord: independent|sync` at top level and per-gear
`doors: [ { port:{guid,kind:servo,idx}, open: 0, close: 10000 }, … ]`,
`door_mode: sync|delay|sequence`, `door_delay_ms`, `close_policy: both|first|none`.
Parser: direct `YamlNode` traversal like the existing gears[] block; `portRefFromNode`
for each door port; door open/close positions are normalized (Rule servo-intent).

### 2.2 Door sub-sequencer (port `door_sequencer.{h,cpp}` from archive)
Per gear: `DoorSequencer` with `open()/close()/update(nowMs)/isComplete()`. Drives
≤2 servos by `SERVO_SET_POS_NORM` (role-forwarded), implements the 5 modes via a
small phase counter + the computed `travelMs` per door. `isComplete()` when all
commanded doors have passed `travelMs + settle` (Rule 40 clock).

### 2.3 Gear op-queue (port `gear_sequencer.{h,cpp}`)
Per gear: build the op list on deploy/retract; `update(nowMs)` advances one leg at
a time, calling `DoorSequencer` for door legs and the existing motor-seek for the
RUN_MOTOR leg, waiting on `isComplete()` / `BIMOTOR_ENDSTOP_RESULT` respectively.
SYNC_BARRIER legs park at `SYNC_DOORS_OPEN` / `SYNC_MOTOR_DONE` until the
coordinator releases them. Preemption (deploy during retract) rebuilds the queue
safely (archive logic). Phases reported on the wire extend `GearPhase` with door
sub-states (or keep the 7 phases + a separate door-phase byte — see §2.5).

### 2.4 Multi-gear coordinator (in `GearControlServicePolicyT::update`)
When `coordMode == sync`: after ticking every gear, run `releaseSyncBarriersIfReady`
— if **all** gears sit at `SYNC_DOORS_OPEN`, advance all; same for
`SYNC_MOTOR_DONE`. `independent` skips this.

### 2.5 Wire protocol (additive, Rule 11/2)
- **Config** flows via `/gearcontrol.yaml` (no new command packets needed).
- **Status**: extend `GEAR_STATUS_RESP` per-entry with a trailing `doorPhase:u8`
  (Rule 11 append — old clients read the first 2 bytes). Door phase enum:
  `idle / opening / open / motor / closing / closed`.
- **Manual hold for calibration** (§1.8): `GEAR_MANUAL_HOLD 0x?? [id]` /
  `GEAR_MANUAL_RELEASE 0x?? [id]` — free bytes in the gear range (`0xEB–0xED` per
  the dispatch map, or the next free in `0xD7–0xD9`'s neighbourhood; **grep
  `ownsType` before allocating**, CLAUDE.md). While held, the gear yields the motor
  role and the hub forwards role-layer BiMotor commands to it.
- **Calibration drive** reuses the EXISTING role-layer packets (`BIMOTOR_MOVE_TO_END
  0x5F`, `SEEK_ENDSTOP 0x6E`, `SET_GUARD 0x77`, `GET_STATUS 0x6B`) forwarded by the
  Topology service to the motor's GUID'd port — no new packets.

### 2.6 Go mirror (`app/go/protocol/gear/gear.go` + `app/go/console/cmd_gear.go` + `app/go/client/gear.go`)
Add the `doorPhase` to `DecodeStatus`, the `MANUAL_HOLD/RELEASE` builders, and the
GUID'd BiMotor-forward helpers (drive via `client.Role(guid)` — the `RoleTarget`
transport, Rule 58). Update the gear client + CLI command file (`engine/handlers/gear/`
is archived). CLI: `gear-door-*`? (optional — the Studio tab is the primary surface;
CLI can stay at the existing flat-hyphenated `gear-*` commands).

### 2.7 Apply translator (`apply_hubfx_config.h`)
`applyGearControlConfig` already attaches the motor role; extend to (a) attach the
door servo roles (or rely on the IO/`/hubfx.yaml` ports block already attaching
ServoActuator), (b) push each door's computed `travelMs`, (c) set `coordMode`.
Door servo `reversed` comes from the servo profile (Rule 44), not the gear config.

---

## 3. Studio GUI plan

### 3.1 Tab gating (`devicemodel.ts studioTabs`)
Show the **Gear** tab only when `connected && controllerType === 'hubfx'` AND the
`gearcontrol` capability domain is present (GEARCTRL advertised) — mirror how
enginefx/gunfx gate. The `/gearcontrol.yaml` loads on connect (HubFX-only loader,
already gated per the recent fix); absent file → empty defaults. Replace the
generic `DomainTab` in `GearLandingTab.svelte` with the new `GearPanel.svelte`.

### 3.2 Config source (Rule 46)
New `gear.ts` module: stores (config/draft/dirty), `loadGearConfig`/`applyGearConfig`,
`gearHasErrors` derived (cross-file: door/motor ports must resolve in the device
model), and `gearConfigSource: DirtySource`. Register in `App.svelte` onMount
(after hubconfig). Loader gated on `controllerType === 'hubfx'`.

### 3.3 Panel layout (`GearPanel.svelte`)
```
┌ Gear / Undercarriage ───────────────────────── [✓ Enabled]  [Apply→toolbar] ┐
│ Coordination:  ( ) Independent   (•) Synchronized   ⓘ all gears move together │
│                                                                               │
│ + Add undercarriage channel                                                   │
│ ┌ Channel: "Nose" ───────────────── [phase pill]  [▶ Deploy/■ Retract] [×] ┐ │
│ │ Name        [Nose                ]                                         │ │
│ │ Gear motor  [HBridge pool ▾ (BiDcMotor, unclaimed)]  [⚙ Calibrate…]       │ │
│ │ Doors (≤2)  pairing: (•) sequence ( ) delay [500 ms] ( ) both-together     │ │
│ │   Door 1   [Servo pool ▾]  [↔ Normal/Reversed]  [⚙ Calibrate…]  [live bar] │ │
│ │   Door 2   [Servo pool ▾]  [↔ Normal/Reversed]  [⚙ Calibrate…]  [live bar] │ │
│ │   After deploy:  doors ( ) both close  (•) one closes  ( ) none close      │ │
│ │   ⚠ per-row / section validation (Rule 35 red, Rule 39 yellow)             │ │
│ └───────────────────────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────────────────┘
```
Reuse:
- **Multi-channel list** = LandingPanel add/remove/per-item-card pattern
  (`LandingPanel.svelte:311–589`).
- **Motor picker** = `freePortPool(ports, claims, 'hbridge', RoleKind.BiDcMotor, exempt)`
  (Rule 49) + empty-pool yellow `section-warn` (Rule 39).
- **Door servo picker** = `freePortPool(..., 'servo', RoleKind.ServoActuator, …)`; the
  `↔ Normal/Reversed` toggle + `⚙ Calibrate…` + the **live position bar** = the
  shared `ServoWidget.svelte` (Rule 24) + `ServoCalibrationDialog.svelte` (Rule 28),
  exactly as LandingPanel/GunFx use them.
- **Enable** = Rule 45 button; **Deploy/Retract** = Rule 48 action-toggle gated on
  `busy || dirty || hasErrors` (ON→OFF i.e. retract always allowed), with a live
  **phase pill** beside it fed by `GEAR_PHASE_EVENT` / status poll.
- (Optional) an **RC channel** to trigger deploy/retract per channel → Rule 43
  `collectChannels` + `ChannelToggleCluster` (defer to phase 2 unless wanted now).

### 3.4 Motor calibration popup
A modal (ServoCalibrationDialog pattern) that reuses the Diagnostics-tab BiMotor
controls — **manual jog / move-to-end / seek / live status + the stall-guard config
(LiveRatio ratio + ceiling)** — but driven through the **hub** (GUID'd, topology-
forwarded) instead of direct-to-expander. New Wails bindings:
`GearMotorHold(gearId)` / `GearMotorRelease(gearId)` + `GearMotorMoveEnd/Seek/Status/
SetGuard(guid, …)` wrapping `client.Topology.SendRoleCommand`. On open → HOLD; on
close → RELEASE + re-claim. Extract the Diagnostics BiMotor logic into a shared
`bimotor_controls.ts` so both tabs share one implementation (Rule 7/22).

### 3.5 Validation (Rule 35/39)
- Motor port unset → red section error (channel can't deploy).
- Door servo selected but role not ServoActuator / port claimed elsewhere → red.
- Empty motor/servo pool → yellow `section-warn` (fix on IO tab).
- `DUAL_DELAY` delay 0, or `close_policy=first` with <2 doors → row warning.
- Deploy/Retract + Calibrate gate on `busy || dirty || hasErrors`.

---

## 4. Config YAML shape (`/gearcontrol.yaml`)
```yaml
schema_version: 2
enabled: true
coord: sync            # independent | door_sync | full_sync | sequenced
input:                 # OPTIONAL (2026-06-11) — one RC up/down channel drives ALL gears
  name: gear_updown    # named channel from /hubfx.yaml inputs[] (Rule 43)
  threshold_us: 1500
  hysteresis_us: 50
  invert: false        # false: above threshold = RETRACT (gear up)
sounds:                # OPTIONAL (2026-06-11) — transit loops on HubFxLayout::Gear (slot 5)
  deploy:  /sounds/gear/deploy.wav    # looped while any gear deploys; empty = silent
  retract: /sounds/gear/retract.wav
  output_mask: 3       # 1 = left, 2 = right, 3 = both
gears:
  - id: 0
    name: nose
    motor: { kind: hbridge, idx: 0 }       # guid omitted = hub-local; expander → guid:"3225"
    deploy_duty: 600
    retract_duty: -600
    timeout_ms: 20000
    doors:
      - { port: { kind: servo, idx: 0 }, open: 10000, close: 0 }
      - { port: { kind: servo, idx: 1 }, open: 10000, close: 0 }
    door_mode: sequence          # sync | delay | sequence
    door_delay_ms: 500           # delay mode only
    close_policy: first          # both | first | none
```
Door `reversed` is NOT here — it lives on the servo's profile in `/hubfx.yaml`
`ports[]` (Rule 44).

`input:` is wired by the standalone `GearActivationDriver`
(`config/gear_activation.h`, the gear twin of `LandingActivationDriver`) so the
service keeps its two template params; it fires the same
`GearControlService::commandAll()` the wire `GEAR_ALL` takes, honouring the
coord mode.  Failsafe is firmware-fixed to the DEPLOY side (RC loss lowers the
gear).  `sounds:` plays through an `AudioCmdFn` trampoline the sketch binds
(`bindAudio`, GunFx pattern): the matching WAV loops on the dedicated `Gear`
mixer channel while any gear is mid-transit and stops when the set settles
(a Sequenced chain counts as still-moving across the inter-gear handoff).
Both blocks are append-only optional — a pre-2026-06 v2 file parses unchanged.

## 5. Phasing / PR sequence
1. **Firmware sequencing** — port `door_sequencer` + `gear_sequencer`, extend
   `GearDef`/`/gearcontrol.yaml` parse, op-queue + per-gear door bracket (no sync).
   Verify one gear end-to-end on the bench (open→motor→close, completion timing).
2. **Multi-gear sync** — coordinator + SYNC barriers + `coord` config; verify two
   gears move together vs independent.
3. **Status + Go mirror** — `doorPhase` in status, gear handler/parsers, manual-hold
   packets + topology-forwarded BiMotor bindings.
4. **Studio GearPanel** — config source, multi-channel editor, port pickers, servo
   widgets, enable/deploy toggles, validation.
5. **Motor calibration popup** — hub-forwarded BiMotor controls + manual-hold.
6. **Docs** (Rule 0): controller README + this file + dispatch-map/error-range
   updates; mark door-sequencing landed.

## 6a. Operator decisions (LOCKED 2026-06-06)
1. **Servo completion is MONITORED, not timed.** `ServoActuatorRole` emits a
   `SERVO_MOTION_DONE` async event when its `MotionProfile1D` reaches target
   (`atTarget()` rising edge after a commanded move); the hub routes it to the gear
   service exactly like `BIMOTOR_ENDSTOP_RESULT`. The gear door-leg waits on that
   event (per-door), never a timer.
2. **All sync modes ship:** `independent`, `door_sync` (barrier at door phases),
   `full_sync` (barriers at door AND motor phases), `sequenced` (one channel's full
   cycle, then the next). Phase order per channel is door-seq → undercarriage →
   door-seq; barriers sit between phases.
3. **Gear calibration REMOVED.** Delete `GEAR_CALIBRATE`/`GEAR_CALIB_CANCEL` + the
   `Calibrating` phase + the 3-leg sweep. Endstop calibration now lives entirely on
   the BiDcMotor role (LiveRatio + ceiling guard). The Studio motor popup tunes the
   role guard + jogs; it does NOT call a gear-level calibrate.
4. **One global trigger drives ALL channels.** Deploy/Retract is fleet-wide
   (`GEAR_ALL`); channels exist to make config atomic + enable cross-channel sync.
   Per-channel deploy stays for bench testing.
5. **Robust state propagation:** every channel broadcasts its overall phase AND its
   sub-phase (door-opening / doors-open / motor-running / door-closing / …) so
   Studio shows live per-channel door+gear state.

## 6. Open questions / decide with the operator
- **Servo completion**: timer-from-profile (recommended, no round-trip) vs a real
  servo-status poll for early-exit? Start with the timer.
- **Sync granularity**: ship `independent` + full `sync` (doors+motor) first; do we
  also want door-only sync (`DoorSync`) and front-then-rear `Sequenced`? Enum has
  room.
- **Calibration ownership**: `GEAR_MANUAL_HOLD` new packet vs reuse `Calibrating`
  phase to yield the motor — lean to a dedicated HOLD so calibration ≠ the 3-leg
  sweep.
- **RC trigger per channel** now (Rule 43 ChannelToggleCluster) or phase 2?

## 7. Archive reference map (port these)
- `controllers/archive/hubfx-esp32s3/src/effects/gearcontrol/gear_sequencer.{h,cpp}`
  — op-queue (`OPEN_DOORS→SYNC→RUN_MOTOR→SYNC→CLOSE_DOORS`), preemption,
  `isWaitingSyncDoorsOpen/MotorDone`.
- `…/door_sequencer.{h,cpp}` — 5 door modes, `DOOR_TRAVEL_TIME_ms`/`SETTLE_TIME_ms`,
  `atTarget` early-exit.
- `…/gearcontrol_effect.{h,cpp}` — `GearCoordMode` + `releaseSyncBarriersIfReady`
  + per-gear `GearUnit { doors, cycle, servoDoor0/1, … }`.
- `controllers/archive/sfx_serial_legacy/gearcontrol/gearcontrol.h` — `DoorMode`
  enum (`NONE/SINGLE/DUAL_SYNC/DUAL_DELAY/DUAL_SEQ`), `GearControlDoorModeConfig`
  (`preDeployMode/postDeployMode/delay_ms`), `GearControlServoConfig` (reversed +
  motion profile).
- `controllers/archive/sfx_peripherals_legacy/motor/stall_detector.h` — superseded
  by the current BiDcMotor LiveRatio+ceiling guard; use the live one.

## Manual / maintenance control (2026-06-23)

For setup/checkout the operator can drive a strut's **doors** and **strut** motor
INDEPENDENTLY of the coordinated deploy/retract sequence — per-leg and fleet:

- **Wire.** `GEAR_DOOR` (0x01) `[id][open]`, `GEAR_STRUT` (0x02) `[id][down]`,
  `GEAR_DOOR_ALL` (0x03) `[open]`, `GEAR_STRUT_ALL` (0x04) `[down]` (free 0x00–0x0F
  block). `GEAR_STATUS_RESP`/`GEAR_PHASE_EVENT` grew a Rule-11 append
  `[doorsOpen][strutState]` so the host can gate the controls.
- **Safety interlocks (firmware-enforced, authoritative — the GUI gate mirrors).**
  Close-doors requires the strut **Up**; move-strut (either way) requires the doors
  **Open**. Violations NACK with `GEAR_DOORS_CLOSED` (0x67) / `GEAR_STRUT_NOT_UP`
  (0x68); a manual op while a cycle is in flight NACKs `GEAR_BUSY` (0x69). Fleet
  commands are all-or-nothing (dry-run every leg first).
- **FSM.** The `Gear` gained explicit `_strutState` (Unknown/Up/Out/Moving — set on
  every confirmed endstop reach) + a `_manualLeg` so completion asyncs settle the
  manual op instead of re-entering `pump()`; `doorsOpen()` derives from the
  DoorSequencer's persistent `_doorEnd[]`. A coordinated command (setTarget/
  stepToward) clears `_manualLeg` and takes over — so the connection-loss emergency
  deploy is never blocked. Manual doors honour the configured door-mode.
- **Studio.** A "⚙ Manual / maintenance" section in each strut card (Open/Close
  doors · Strut down/up) + a fleet "Manual (all)" row, gated to mirror the
  interlock. Console: `gear-doors` / `gear-strut` (+ `-all`).
