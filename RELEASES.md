# ScaleFX Releases

Release notes for the ScaleFX multi-platform effects system. Each firmware
component is versioned and released independently (tag `‹component›-v‹version›`);
the host tools (Studio + CLI) ship together. GitHub releases carry the flashable
firmware binary; ScaleFX Studio's **Firmware** tab can flash a release directly.

---

## 2.45.4 — 2026-08-08 — landing-light direction single-sourced + servo-path audit

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.45.4 | ESP32-S3 | `hubfx-v2.45.4` |

PATCH: audit follow-up to the 2.45.2 REV-reflection restoration — every
firmware servo drive path checked for calibrated-limit compliance.

Audit result: all effect paths respect the calibrated limits — gear
doors, servo struts, landing lights and gun aim all drive via
`SERVO_SET_POS_NORM` (mapped onto the role's LIVE `[min,max]`), gun
recoil is clamped at the role output, and raw `SERVO_SET_TARGET` is
hard-clamped by the role. (Limits only protect once the calibration
actually reaches the device — the Studio save bug remains open.)

### Bug Fixes

- **Landing lights: deploy direction now lives SOLELY in the servo
  profile's `reversed`** — the same single-source rule as gear doors and
  servo struts. The panel's "Deploy direction" toggle wrote swapped
  `open_us`/`close_us` sentinels, a workaround from the era when the
  role's REV reflection was silently broken; with the reflection
  restored (2.45.2) the two mechanisms COMPOUNDED — both set cancels
  out, either alone flips deploy under the operator. The toggle is
  removed (a hint points at ↔ Reversed in ⚙ Calibrate), and the firmware
  ignores the vestigial `open_us`/`close_us` (still parsed for compat).

### Internal

- Dead code removed: `GunUnit::commandServoTargetUs` (no callers — gun
  aim uses the pos-norm path).
- PPM sync-skip pulse diagnostic (from `e06e2d6`) ships in this build:
  the RMT capture logs raw HIGH/LOW µs of non-PPM signals at ~1 Hz — a
  no-scope pulse-width meter for SRV→INP loopbacks.

---

## 2.45.3 — 2026-08-08 — quiet-attach reverted: silent PWM inputs make black-box retracts hunt

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.45.3 | ESP32-S3 | `hubfx-v2.45.3` |

PATCH: reverts 2.45.2's quiet-attach after live falsification on the bench.

### Bug Fixes

- **Servo headers pulse from attach again.** 2.45.2 held every servo pin
  forced-low until its first command, to stop the boot centre-slam. Bench
  result: "all servos randomly moving" with ZERO commands on the wire, no
  resets, RC routing disabled — integrated retract controllers hunt/cycle
  autonomously when their PWM input is silent, and limp door servos flap
  under spring load. A steady pulse is the lesser evil; the boot-slam is
  mitigated by the (retained) integrator-seed fix and by resets being rare.
  The `kQuietAttach` driver capability plumbing stays (all-false) for a
  future opt-in per-port policy.

---

## 2.45.2 — 2026-08-08 — servo REV reflection restored + role-attach position seed

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.45.2 | ESP32-S3 | `hubfx-v2.45.2` |

PATCH: two shared-role-library bugs behind "doors don't hit the calibrated
ends / servo motion profiles feel unreliable". No wire changes.

### Bug Fixes

- **The servo profile's `reversed` flag did NOTHING on the wire path.** The
  REV reflection was silently lost in the Phase 2.9 MotionProfile refactor:
  `setNormalizedTarget` mapped the intent fraction linearly onto
  `[min, max]` with no reflection (its comment claimed `setTarget`
  reflected — it never did), and the only reversed-aware code
  (`openEndpoint()/closeEndpoint()`) had **no callers**. Every door, servo
  strut, landing servo and gun axis ignored ↔ Reversed while the Studio
  dialog and all docs claimed otherwise. The reflection now lives in
  `setNormalizedTarget` (intent space: full = open/deploy → MIN-µs end when
  reversed); raw `SERVO_SET_TARGET` stays unreflected (servo space — the
  calibration jog needs absolute µs).
- **Servo headers are silent until first commanded (quiet attach).** The
  MCPWM driver used to start the 1500 µs pulse train the moment the port
  attached at boot — every reset (and the bench had 25 unexpected
  disconnects/resets in one day) drove all 10 servo headers to centre at
  raw servo speed: mid-travel for a retract, "randomly open and shut at
  high speed" from the operator's chair. The generator now holds the pin
  forced-low from attach; the first real command releases it with the
  correct width already latched. A servo with no pulse simply holds — no
  boot motion at all. (ESP32/hub only; the Pico Arduino-Servo backend
  keeps its attach-time pulse until the P8 native migration.)
- **Role re-attach no longer teleports the integrator to centre.** Every
  Studio Apply (CONFIG_RELOAD → detach + re-attach all hub roles)
  re-emplaced each ServoActuatorRole and snapped its motion integrator to
  1500 µs while the physical servo sat wherever it was — the Studio live
  bar showed centre ("some other value"), and the next command made the
  servo jump unprofiled from its true position before slewing. The role now
  seeds from the port's last-written pulse (fresh boot still starts at the
  port's initial 1500 centre).

**Note:** the fix is in the shared role library
(`controllers/lib/sfx_board/roles/`) — expander boards (GearControl,
PortExpander, LightFX) pick it up on their next build+flash; until then
their locally-hosted servos still ignore `reversed`.

---

## 2.45.1 — 2026-08-08 — servo calibration integrity: envelope leak, auto-expand, double reversal

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.45.1 | ESP32-S3 | `hubfx-v2.45.1` |

PATCH: bug fixes to the servo calibration flow + the day-old servo strut
modes. No wire changes; Studio + HubFX flash recommended together.

Root-caused from a live bench session log ("calibration doesn't stick /
servos get mismatched"): every wire command was provably addressed to the
right `(guid, idx)` — the corruption came from three flow bugs stacking.

### Bug Fixes

- **Studio: the calibration widen-envelope can no longer persist.** Opening
  the Calibrate popup pushes a wide fast-slew envelope (800–2200 µs, 4000 µs/s)
  so jogging can reach the end-stops. It used to go through `SetPortProfile`,
  which ALSO wrote the Studio overlay — so an **Apply or a link drop while a
  dialog was open silently saved the envelope to `/hubfx.yaml`** as that
  servo's profile. The widen (and Cancel's origin restore) now use a new
  live-only push (`ServoSetProfileLive` — role only, no overlay, no dirty
  flag); only Save can put a profile where Apply persists it.
- **Studio: jogging no longer auto-expands the draft limits.** Sweeping the
  range to explore a mechanism (mandatory for a black-box integrated
  retract) used to silently drag min/max to the sweep extents — Save then
  committed 800–2200 instead of the operator's limits (bench log showed a
  strut controller saved at exactly the envelope, twice). Limits now change
  ONLY via ⤓/⤒ Set-as-min/max or the numeric fields.
- **Gear (servo strut modes): direction now lives SOLELY in the servo
  profile's `reversed`.** The per-gear `reverse:` flag (the "Deploy drives
  Min/Max end" toggle) COMPOUNDED with the profile flag — both set cancels
  out, one set in the "wrong" layer inverts a freshly-calibrated servo; in
  `servo_shared` mode per-gear flags on the ONE shared port were
  last-write-wins. The firmware ignores `reverse:` for servo-driven struts
  (hbridge keeps it) and the toggle is gone from the servo-mode UI — same
  single-source rule as doors and landing lights (Rule 42).
- **Studio: an unexpected disconnect closes an open calibration dialog**
  (its Cancel needs the wire; the session cannot survive the link).
- **HubFX: `/hubfx.yaml` port slots are zeroed before each parse.** The
  config data is re-populated in place on every reload and the profile
  parser's field defaults read the current slot — a stale occupant from a
  prior load with a different port order could donate its profile/ESC
  fields to whatever port parses into that slot now. Never observed on the
  wire, but it is exactly the "profiles jump between servos" failure mode;
  closed as hardening.

### Internal

- Calibration dialog + parameter-reference / FAQ / assistant-context copy
  updated to the new capture-only limit model (Rule 64).

---

## 2.45.0 — 2026-08-08 — customizable strut drive: integrated PWM retracts

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.45.0 | ESP32-S3 | `hubfx-v2.45.0` |

MINOR: new config schema fields; NO wire changes (rides the existing
ServoActuator role / SERVO_SET_POS_NORM) — expanders need no reflash.

### New Features
- **The strut stage of the gear sequencer (doors → strut → doors) is now
  customizable** via a single `strut_mode:` selector supporting the three
  undercarriage setups:
  - `hbridge` (default, unchanged) — custom DC motor per strut on an
    H-bridge port (BiDcMotor role), endstop detected by current;
  - `servo` — each strut is an INTEGRATED/3rd-party retract controller
    taking an RC-servo signal on its OWN channel (per-gear
    `strut_servo:` port + `travel_ms`);
  - `servo_shared` — ONE servo channel drives the WHOLE undercarriage
    (top-level `strut_shared: { port, travel_ms }`).
  The servo modes treat the controller as a BLACK BOX: the sequencer
  puts the pulse at the deploy/retract end (deploy → calibrated MAX end
  unless `reverse:`) and holds for the FIXED `travel_ms` before the
  door stage engages — no feedback exists, so the time IS the
  completion.  A mid-travel reversal re-commands the pulse and restarts
  the full timer (conservative).  E-stop cannot brake a black-box
  stroke (logged); manual strut moves use the same timer.  Stall
  guards, calibration, and the voltage cap remain hbridge-only.
- **Studio**: a "Strut drive" segmented selector on the gear tab (with
  the shared channel + travel time inline for `servo_shared`); each
  strut card's left column switches between the motor editor (hbridge)
  and a strut-channel + travel-time editor (servo) or a shared-mode
  note; pools/claims/validation are mode-aware (strut channels join
  `$effectClaims`; doors and struts exclude each other's picks).

---

## 2.44.3 — 2026-08-08 — the real crash: re-entrant role-event dispatch

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.44.3 | ESP32-S3 | `hubfx-v2.44.3` |

PATCH: the root-cause fix behind the whole 2026-08-08 crash family (the
2.44.2 stack growth treated a symptom; a fresh coredump then handed us
the full recursive backtrace).

### Bug Fixes
- **Gear role-event reactions ran INSIDE the event dispatch** — a fault
  reaction (`Gear::enterError` → brake + batch commit) sends a
  synchronous `forwardToExpander`, whose ACK-wait PUMPS the receive
  path, which dispatches the NEXT queued endstop event NESTED inside
  the first handler (same reaction → deeper nesting).  The batch buffer
  and bus-client tag state are single-flight; three simultaneous
  NO-MOTOR faults (unwired motors + full_sync) nested three deep and
  corrupted memory — presenting as three different-looking panics
  (poisoned stack, scheduler ready-list assert, LoadProhibited in the
  subscriber fan-out).  Latent since the gear phase; tonight's
  bench scenario was the first reliable detonator.  Fix: role-event
  ingress is now ENQUEUE-ONLY (spinlock-guarded ring, safe from any
  dispatch context — events arrive on the USB-CDC driver task AND on
  sync-forward pumps); the gear service drains the ring at the top of
  update() on the loop task, so reactions and their synchronous sends
  always run flat.  Bench: the previously 100%-reproducible triple-fault
  cycle now runs crash-free indefinitely.
- **Rejected-frame hex dump**: `[SerialBus] Packet parse failed` now
  logs the head bytes of the rejected packet (the input-gap saga's
  decisive tool, permanently installed).

### Known (hardware, not firmware)
- With a brushed gear motor attached and driving, the hub↔expander USB
  link dies within ~1 s (hcd transaction errors, no disconnect
  callback) and self-heals in ~8 s — survives cable swaps and both
  battery topologies.  Classic brushed-motor EMI: fit suppression caps
  on the motor, twist the leads, ferrite the USB cable.  Firmware now
  rides through it; the link hygiene is a bench/airframe task.

---

## 2.44.2 — 2026-08-08 — loopTask stack overflow during gear cycles

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.44.2 | ESP32-S3 | `hubfx-v2.44.2` |

PATCH: crash fix, found via flash coredump minutes after 2.44.1.

### Bug Fixes
- **loopTask stack overflow → PANIC reboot mid-gear-cycle** (coredump:
  exccause 0x41 DebugException, backtrace dead at an 0xa5-poisoned
  frame — the end-of-stack watchpoint).  A gear cycle converges the
  gear FSM, role-event forwards, SerialBus parsing (including rejecting
  USB-corrupted packets), the audio rail governor, and USB writes on
  the ONE 16 KB loop task; the 2.44.1 synthesized-disconnect teardown
  ran there too — repeating the exact mistake the June USB saga
  documented (deep teardown on a shallow context).  Fixes: the
  synthesized disconnect is now QUEUED to the 8 KB `usb_worker` task
  (new PendingWork::SynthDisconnect, duplicate-safe), and the loop task
  grows 16 → 24 KB as convergence headroom.

---

## 2.44.1 — 2026-08-08 — USB half-dead expander slot self-heals

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.44.1 | ESP32-S3 | `hubfx-v2.44.1` |

PATCH: USB-host robustness.

### Bug Fixes
- **A half-dead expander slot now self-heals.**  An expander that drops
  off the bus WITHOUT delivering the CDC disconnect callback left the
  hub with an open slot whose driver handle was dead — every forwarded
  packet failed `ESP_ERR_INVALID_STATE` and the hub spammed
  `[UsbHost] TX failed` forever (bench: Studio's motor-status poll
  against GearControl, cable/ground glitch suspected as trigger).
  `cdcWrite` now counts consecutive INVALID_STATE failures and after 8
  SYNTHESIZES the disconnect through the normal teardown path (close,
  unmount, auto-recovery timer) so re-enumeration brings the device
  back — a ~2 s blip instead of a wedged session.  The TX warning is
  rate-limited (first + every 32nd).

---

## 2.44.0 — 2026-08-08 — voltage-first gear drive (raw duties retired)

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.44.0 | ESP32-S3 | `hubfx-v2.44.0` |
| GearControl (expander) | 1.3.0 | RP2040 | `gearcontrol-v1.3.0` |
| PortExpander (expander) | 1.0.0-rc2 | RP2040 | `portexpander-v1.0.0-rc2` |

MINOR: config-schema change on the hub; the expander bump fixes the motor
PWM carrier (below) — reflash expanders for quiet, full-torque partial-duty
drive.

### Breaking ⚠️
- **`gears[].deploy_duty` / `retract_duty` RETIRED** (parsed keys are
  ignored).  The raw numbers silently re-meant themselves on every pack
  change — the same `20000` that drove a mechanism at 9 V on 4S starved
  it at 6 V-capped on 6S (the 2026-08-07 bench stall).  Direction moves
  to a `reverse:` flag (default false = deploy forward); operators with
  reversed struts re-set the Direction toggle once.

### New Features
- **Voltage-first drive**: each strut declares only its motor DRIVE
  voltage (`motor_voltage_mv`, default 6000, floor 1000 — "no cap" is
  gone since full-scale would mean raw pack voltage).  The strut seeks
  at full scale and the BiDcMotor role's cap delivers exactly that
  average at the motor on ANY pack, battery-sag compensated at 10 Hz.
- **Studio**: the calibration dialog drops the Duty field (Motor V +
  Timeout are the only drive knobs; sweeps/jogs run at the drive
  voltage, identical to a real deploy); the strut card summary shows
  `drive V · direction · timeout`; the Direction toggle writes the
  `reverse` flag.  `commitDuties` + its tests deleted.

### Bug Fixes
- **Expander motor PWM carrier fixed: ~490 kHz → 20 kHz.**  The Pico
  native-PWM path never set a slice clock divider, so H-bridge pins
  free-ran at clk_sys/256 ≈ 490 kHz — irrelevant historically because
  the old raw duties (20000 on a 255-count port) clamped to 100 % =
  smooth DC, but the voltage-first cap drives REAL partial duty and half
  a megahertz is far beyond clean H-bridge FET switching: audible whine,
  mushy torque, ripple-rattled stall sensing (the 4S bench symptom).
  `NativePwmPort::setFrequencyHz` now lands on the Pico slice divider;
  GearControl + PortExpander set 20 kHz (ultrasonic, in driver spec) on
  every motor at bring-up.  Frequency is per-slice, so a slice-partner
  LED changes carrier too — harmless.
- **Dual-PWM H-bridge PWM switched to SLOW-DECAY (drive/brake)** (1.2.1).
  The old mapping (active=duty, other=0) coasted every off-phase — fast
  decay — so at real partial duty the motor current collapsed each
  cycle: jerky motion, weak torque, low/peaky current sense and false
  stall trips.  The active side now stays HIGH while the complementary
  side chops drive↔brake, keeping motor current continuous (both-high =
  brake was already the hardware contract).  PwmDir bridges
  (PortExpander) are unaffected — their decay mode is fixed by the
  driver chip.
- **No-load fault diagnosis + high-rail sensing limit documented**
  (1.3.0, ISSUES.md §7): the GearControl in-line INA226 cannot read
  motor current in ONE drive direction above ~8 V rail (common-mode
  dependent; blind at 16 V/4S and 23 V/6S, both directions fine at
  7.6 V/2S — full characterization in ISSUES.md §7).  A no-load fault
  that coincides with REAL rail sag (vs the pre-drive baseline) is now
  diagnosed as "shunt unreadable at this common-mode — run the motor
  rail from a lower pack" instead of "no motor"; either way the stroke
  FAULTS AND BRAKES immediately (a timed-drive workaround was tried and
  rejected — grinding an unsensed stroke into its endstop wedges the
  mechanism).  **Operational guidance: power the GearControl motor rail
  from 2S/3S** (the expander's cap + sensing use its LOCAL rail — the
  hub's own pack voltage is never involved, so a 6S hub + 2S gear rail
  is fully supported).  Cross-pack validation of the sensed direction:
  endstop stall reads 1170/1286/1333 mA on 2S/4S/6S — one untouched 1 A
  threshold across a 3× rail range.  Hardware fix (low-side shunts or
  INA240) queued for the board rev.
- **Fixed-mode stall guard gains inrush blanking** (same 150 ms window
  as LiveRatio): from standstill the motor draws near locked-rotor
  current until the mechanism breaks away, which false-tripped a 1 A
  Fixed guard ~150 ms into every stroke.  An endstop cannot legitimately
  arrive inside the blank.
- `[mdiag]` drive instrumentation: while any motor is driven, the board
  logs both bridge pins' real PWM slice state + the raw INA226 shunt µV
  / bus mV every 500 ms — the tooling that isolated ISSUES.md §7.

---

## 2.43.1 — 2026-08-07 — TAS5825M is the default codec + M activate fixed

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.43.1 | ESP32-S3 | `hubfx-v2.43.1` |

PATCH: no wire change — default codec driver switches P → M, plus two M
driver activate() fixes found on first bench contact with M silicon.

### Bug Fixes
- **TAS5825M activate() could never reach PLAY** — the FS_MON clock-lock
  gate polled while the chip was still in the DEEP-SLEEP park state
  phase 1 left it in, where the clock detector doesn't sample: the gate
  timed out every boot (`FS_MON never locked, CLKDET_STATUS=0x00` with
  clean I2S clocks) and the codec stayed silent.  The driver now enters
  HiZ (DIS_DSP still held, muted) BEFORE the gate — the detector wakes,
  lock lands in ~30 ms, and the DSP is still only released after a
  proven lock.  Bench 9C6C: `FS_MON locked at 48kHz`, `PLAY OK`, chime
  audible.
- **Every activate() wait with an observable behind it is now a bounded
  poll of that observable** (shared `pollUntil`, first check immediate):
  FS_MON lock, the PVDD ADC sample (its first read right after deep sleep
  is empty — previously fell back to −8 dB; bench: 16.13 V → the optimal
  −5.5 dB step), and the HIZ→PLAY transition (each poll pass re-clears
  the inrush PVDD_UV latch — the retry is the recovery).  Blind delays
  remain only where the datasheet mandates a settle with nothing to
  observe (post-reset 50 ms).  Activation is faster AND deterministic.

### Internal
- **`-DHUBFX_CODEC_TAS5825M` is now set in the stock hubfx build** — the
  pcb-nextver board revision ships TAS5825M silicon, bench-confirmed on
  HubFx-9C6C (`die id: 0x95`).  The M driver adds readback-verified
  init, the DIE_ID identity gate, and M fault decode.  Older P-silicon
  boards (e.g. 78A4, DIE_ID 0x97) drop the define to build the P driver.

---

## 2.43.0 — 2026-08-07 — gear-motor rated-voltage cap

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.43.0 | ESP32-S3 | `hubfx-v2.43.0` |
| GearControl (expander) | 1.1.0 | RP2040 | `gearcontrol-v1.1.0` |

MINOR bump on both: additive Rule 11 wire fields; the cap logic lives in the
shared `BiDcMotorRole`, so the GearControl expander must be reflashed for the
cap to actually clamp the motor.

### New Features
- **Gear motors can declare their rated voltage** (`gears[].motor_voltage_mv`
  in `/gearcontrol.yaml`, **default 6000 mV**; explicit `0` = cap off).  The
  BiDcMotor role CAPS every commanded |duty| — drive, seek, move-to-end — at
  `maxDuty × rated_mV / rail_mV`, so a 6 V retract motor survives a 2S–6S pack.
  The rail is the LIVE per-motor INA226 voltage sense when present (re-clamped
  ~10 Hz, so battery sag auto-compensates), else the attach-time declared port
  rail.  A **cap, not a scale**: duties already tuned below the ceiling keep
  their exact meaning.  Follows the GunFX `element_mv` layering (rail on the
  port, mechanism on the role, rating in the effect config).
- **Studio: "Motor V" field in the gear-motor calibration dialog** (seeded from
  the strut, saved back by *Save to strut*); the live-status grid shows the
  active cap (`V-cap ±duty @ rail V`).  The calibration guard push includes the
  voltage so sweeps drive exactly like a real deploy.
- **CLI**: `bimotor-guard … [motorMv]` optional trailing arg sets the cap
  (omit = leave unchanged); `bimotor-status` renders the volt-cap line.

### Protocol Changes
- `BIMOTOR_SET_GUARD` (0x77) Rule 11 append `[14:16] element_mV` (0 = cap
  off; 14-byte form leaves the peer's cap untouched).  Rides SET_GUARD because
  the 0x40–0x7F role opcode space is exhausted (0x78+ = SBUS/Jeti input).
- `BIMOTOR_STATUS_RESP` (0x6C) Rule 11 append `[10:12] element_mV [12:14]
  railNow_mV [14:16] capDuty` (lengths 8/10/16 all valid).

### Internal
- New voice **PEWPEW** demo gun sounds (`media/sounds/PEWPEW/pew_45ppm.wav` /
  `pew_80ppm.wav`) — one cute voice pew per loop; the loop length sets the
  fire rate (45 / 80 pews per minute).

---

## 2.42.1 — 2026-08-01 — input remaps take effect on Apply

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.42.1 | ESP32-S3 | `hubfx-v2.42.1` |

PATCH: logic fix on top of 2.42.0.

### Bug Fixes
- **Remapping an input to a different RC channel now takes effect on the
  Input & Ports Apply.**  Every effect resolves its input NAMES against
  /hubfx.yaml's inputs[] at its OWN apply time (Rule 43); Studio's
  input-mapping Apply reloads only the /hubfx.yaml store, so the
  lightfx selector / landing / gear / engine / gun bindings all kept
  listening on the OLD channel until that effect's config happened to
  be re-applied (the reported symptom: light selector moved ch8 → ch10,
  saved + shown in the UI, but only responding after an unrelated light
  edit + Apply).  The hubfx reload callback now re-installs every
  input-driven subscription (`reinstallInputBindings()` — watch for
  `[config] input bindings re-resolved` in the diag log).  This also
  closes the gunfx apply comment's known "Phase 4 polish" gap.

---

## 2.42.0 — 2026-08-01 — PVDD auto-gain + audio power telemetry

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.42.0 | ESP32-S3 | `hubfx-v2.42.0` |

MINOR bump: additive CODEC_STATUS wire fields + a retired config key.

### New Features
- **Codec analog gain auto-detects the amp rail.**  Both TAS5825 drivers
  measure PVDD via the chip's own ADC (0x5E) during `activate()` (in HiZ,
  analog powered) and pick the exact −0.5 dB AGAIN step whose full-scale
  output fits under the measured rail — e.g. a 4S pack at 14.8 V lands at
  −6.5 dB instead of the old fixed 12v preset's −8 dB.  Implausible ADC
  reading (< 4.5 V floor) falls back to the safe −8 dB with a WARN.
- **Audio power telemetry** appended to `CODEC_STATUS_RESP` (Rule 11,
  after the codec name): `pvdd_mV:u16` (live rail), `againReg:u8`
  (auto-chosen gain step), `dieId:u8` (0x95 = TAS5825M silicon — the
  BOM-vs-silicon check), `outPeak:u16` (mixed-output peak since last
  query, tracked losslessly in the Core-1 consumer).  `codec-status`
  renders rail volts, gain in dB, output level %, and an estimated
  watts @4Ω/@8Ω (computed client-side from level × gains; a true
  measurement needs the M's IV-sense/PPC3 pipeline — future work).
- **Codec build flag**: `-DHUBFX_CODEC_TAS5825M` selects the M driver
  for boards carrying M silicon; default stays P.

### Bug Fixes
- **Studio: applied light-program edits no longer revert on tab switch.**
  `loadLightFxConfig` seeded any active program whose name matched a
  preset-library template FROM THE TEMPLATE instead of the device file —
  so after an Apply, remounting the Lighting tab reverted the UI to the
  pre-edit template, and the NEXT Apply pushed that stale template back
  to the board (silently undoing the operator's applied change, e.g.
  freshly added light bindings).  The device file is now authoritative
  for active programs; the library is only the fallback when the file
  is missing from the board.

### Breaking ⚠️
- **`audio.codec_supply` retired** (config-only; wire-compatible).  The
  key in an old `/hubfx.yaml` parses to nothing and is ignored; Studio
  no longer round-trips the `audio:` block.  The CODEC_STATUS supply
  byte now always reports 4 ("auto").

### Internal
- `Supply` enum / `parseSupply` / `setSupplyVoltage` deleted from both
  drivers; presets (`minimal.yaml`, `helicopter_ka50.yaml` ×2 copies)
  and the Studio `yamlAudio` round-trip struct dropped.

---

## Unreleased (pcb-nextver) — TAS5825 register-map datasheet audit

No flashed release yet.  The bench boards turned out to carry TAS5825**M**
silicon (the BOM said P), which prompted restoring the M driver — and a full
audit of `tas5825_regs.h` against the official TI datasheet **SLASEH7H**
(Table 9-6).  The folk map the drivers had shipped with diverged on six
registers; audio only ever worked because the mis-aimed writes were mostly
harmless and one of them ("CLK_SRC" at 0x33) accidentally landed on the REAL
SAP_CTRL1 and set the required 16-bit word length.

### Bug Fixes
- **`tas5825_regs.h` rewritten to the official map** (datasheet = gold
  standard, addresses cited per table): SAP_CTRL1 is **0x33** (was
  mislabeled CLK_SRC; "SAP_CTRL1"=0x60 is really GPIO_CTRL), GPIO1_SEL is
  **0x62** (0x4F is the emergency volume-ramp register), analog gain is
  ANA_CTRL **0x53** / AGAIN **0x54** (0x46 is DSP_CTRL — the old
  "ANALOG_CTRL=0x11" write there silently forced the DSP to 96 kHz
  processing).
- **DIG_VOL scale corrected** — 0x00 is **+24 dB full gain**, 0xFF is mute
  (was inverted: `VOL_MUTE=0x00` meant `setMute()`/`setVolume(0)` would
  command maximum gain).  `setVolume`/`setVolumeDB` formulas fixed; new
  `volRegForDb()` helper.
- **FS_MON code table corrected** — 48 kHz reports **0x09** (Table 9-19),
  not 0x04 (which is 16 kHz); the fabricated 8/44.1/88.2/176.4 kHz codes
  are gone.
- **Fault bit decode corrected** — GLOBAL_FAULT1 is CLK=bit2 /
  PVDD_OV=bit1 / PVDD_UV=bit0 (+OTP-CRC/BQ/EEPROM in bits 7-5); DC/OC
  faults live in CHAN_FAULT (0x70), over-temp shutdown in GLOBAL_FAULT2.
- **Both drivers corrected** (`tas5825_m_codec` restored + ported to
  `SfxI2cBus`; `tas5825_p_codec` same fixes, permissive flow kept):
  DIS_DSP held until I2S clocks are proven (per datasheet), GPIO1→FAULTZ
  now actually output-enabled via GPIO_CTRL, DIE_ID (0x67, =0x95) identity
  check at probe, undocumented "identity DSP coefficient" book writes
  dropped (ROM mode + ZROM defaults are the documented pass-through).

### New Features
- **`tests/hw/tas5825m_beep`** — self-contained pure-IDF bring-up probe:
  1 kHz beep, every register write readback-verified, FS_MON/CLKDET
  decode, live PVDD voltage (PVDD_ADC 0x5E), fault-decoded 2 s heartbeat
  with PLAY auto-recovery.  Hand-off zip: `tas5825m_beep_20260730.zip`.

### Internal
- HubFX still compiles the P codec (`getCodecType()` 1=M / 2=P unchanged);
  switching to the M driver is a one-line typedef change in
  `hubfx_esp32s3.cpp` once the beep probe confirms the silicon.  A version
  bump lands with that switch.

---

## Studio 2026-07-26 hotfix — the REAL "hanging UI" root cause

No firmware change (HubFX stays 2.41.0).  The fresh-board freeze survived the
2.41.0 Studio fixes because its true cause was deeper than the wizard modal /
connect dialog: **Svelte 3's `svelte/store` shares ONE module-level
`subscriber_queue` across every store in the app, and a subscriber callback
that throws mid-notification leaves that queue non-empty forever — after
which every `set()` on ANY store silently notifies nobody.** Handlers still
fire and no error surfaces (nothing is ever marked dirty, so the FE.FLUSH
scheduler watchdog sees a clean queue), which is exactly the observed
"clicks land but nothing re-renders" freeze.

### Bug Fixes
- **Trigger fixed**: `escTelemetryActive` derived read `$t.devices.some(…)`
  while Go's `TelemetrySnapshot.Devices` is a nil slice (→ JSON `null`) on
  any board with no ESC/Jeti device attached — it threw on the first
  telemetry poll after every connect, freezing the whole UI.  Telemetry
  snapshots are now normalized on ingest (`devices`/`sensors` null → `[]`)
  and the derived is null-tolerant.
- **Class fixed**: `svelte/store` is vite-aliased to a hardened drop-in
  (`lib/safestore.ts`, same 3.59.2 semantics) whose notify queue try/catches
  every subscriber and always drains — one bad subscriber can no longer mute
  the app.  Every caught throw is reported to the diag log as `FE.STORE`
  with phase + stack (rate-limited), so the culprit names itself.
- Regression net: `safestore.test.ts` (queue-poisoning, derived-throw,
  cross-store isolation) + the whole vitest suite now runs through the
  aliased store.

### Internal
- GUI-driving harness: `winshot.ps1` gained the Alt-key foreground unlock +
  verification (background `SetForegroundWindow` is silently blocked by the
  OS foreground lock, so synthetic clicks landed on the wrong window).

---

## 2.41.0 — 2026-07-26

The effect-enable rationalization: the `/hubfx.yaml` `features:` master-enable
matrix is RETIRED — each effect's enable lives ONLY in its own sub-config
(`/enginefx.yaml` `enabled:` …), and hubfx.yaml is pure port/input/audio
mapping again.  Plus the fresh-board Studio "hanging UI" fixes and the
no-signal input-broadcast throttle.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.41.0 | ESP32-S3 | `hubfx-v2.41.0` |

### Breaking ⚠️
- **`/hubfx.yaml` `features:` removed** (MINOR bump — additive-tolerant: an
  old file's `features:` key parses to nothing and is ignored).  Effect enable
  is each sub-config's own `enabled:` flag; the firmware no longer overrides
  it.  Rationale: Studio re-uploading hubfx.yaml on every effect toggle could
  collide with audio playback (Rule 54 upload exclusivity) and wedged the
  flash — toggling an effect now touches ONLY that effect's file.

### New Features
- **All effects default OFF** (firmware struct+schema, seed `minimal.yaml`,
  Studio Go DTO defaults, frontend drafts) — a freshly-flashed board boots
  inert; the operator opts each effect in from its panel.
- **No-signal input-broadcast throttle** (2.40.1): an input role with no valid
  signal (RX unplugged) broadcasts at a 4 Hz heartbeat instead of 50 Hz junk
  frames; full rate resumes the instant the signal returns (hot-plug safe).
  Applies to PPM / SBUS / Jeti EX via the shared `InputBroadcaster`.
- ESC-telemetry role label corrected to **"ESC Telemetry"** (was the stale
  "Jeti EX Telemetry"); telemetry type (Kontronik/Scorpion/Hobbywing) is
  selectable inline on the input card AND in the PCB-diagram port popover.
- Input-role split: on a 2-input board IN2 offers only ESC Telemetry, IN1
  only the RC-channel protocols.

### Bug Fixes (Studio)
- **Fresh-board "hanging UI" root-caused + fixed** (GUI-verified via
  screenshots + synthetic input): the Setup Wizard no longer auto-opens as a
  click-swallowing modal (toolbar-only + Escape closes); the Connect dialog
  cannot be dismissed while disconnected (dismissing it left a dead blank UI).
- Robustness: deep device-model normalize (nested `caps`/`allowedRoles`/
  `channels`/`slots` null-guards), FE.CLICK click-path tracer, early
  mount-time error capture (main.ts), telemetry input card layout de-squished.
- PCB overlays: HubFX rev-B input headers now INP/TEL at their real top-center
  position (was rev-A right-column IN1/IN2); GearControl IN + servo positions
  corrected; PortExpander overlay added.

---

## PortExpander 1.0.0-rc1 — 2026-07-25

First release candidate for the new **PortExpander** generic-expander board —
an RP2040 thin expander exposing 8 servo + 5 H-bridge ports to the HubFX
master (the largest expander port surface to date; GearControl is 7+3).

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| PortExpander (expander) | 1.0.0-rc1 | RP2040 | `portexpander-v1.0.0-rc1` |

### New Features
- **New board firmware** (`controllers/portexpander/pico/`), mirroring the
  GearControl thin-expander architecture (`BoardOf<>` + auto policies; roles
  attached by the hub at runtime, Rule 58): 8 × ServoActuator-capable servo
  headers (GP14–21) + 5 × BiDcMotor-capable motor drivers.
- Motor drivers are **PWM+DIR** topology → `PwmDirHBridgePort` (brake falls
  back to coast), unlike GearControl's dual-PWM bridges.
- Per-motor **INA226 current/voltage monitors** on I²C0 (GP4/5), addresses
  decoded from the netlist strapping: 0x40/0x45/0x44/0x41/0x42 for MOT1–5.
- USB identity `2E8A:0183` ("ScaleFX PortExpander"); device name prefix
  `PortExp-<guid>`.
- Recognition landed across the stack in the same change: HubFX
  `ExpanderKind::PortExpander` + PID classification, Go protocol/client/
  devicemodel mirrors, CLI controller registry (`scalefx-flash build|flash
  portexpander`), and Studio (board naming, Firmware tab, PCB overlay with
  the expander_top render).

### Internal
- Pin map decoded from `instructions/schematics/expander.tel` — the netlist
  references QFN-56 **package pins**, not GPIOs (SDA=U1.6→GP4, SCL=U1.7→GP5
  confirmed the mapping).  L_CH1/L_CH2/POWER LEDs are passive (no firmware
  driver); LED1/LED2 (GP25/24) are the standard indicator pair.
- Known-open before 1.0.0 final: shunt value assumed 5 mΩ (verify against
  BOM); HubFX must be reflashed with the PID-classification build to label
  the board on its USB host ports; bench pass on motors + current sense.

---

## 2.39.0 — 2026-07-15

The input-signal saga: RC blackouts during audio playback root-caused to FIVE
stacked causes and fixed; the legacy Jeti EX-Bus downstream slave mode removed;
RPM scaling by motor poles + gear ratio.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.39.0 | ESP32-S3 | `hubfx-v2.39.0` |

### Bug Fixes (the input-gap stack — each necessary, none sufficient alone)
- **asset_loader priority 22 → 3** (Core 0): the SD→PSRAM preload outranked the
  RC input task and starved it for whole read bursts.
- **UART ISRs into IRAM** (`ESP_INTR_FLAG_IRAM` + `CONFIG_UART_ISR_IN_IRAM`):
  MP3-decode cache pressure delayed the flash-resident RX ISR past the 128 B
  FIFO horizon (~10 ms @125k) — real byte loss in 1–2 s windows.
- **MP3 decoder task → Core 1 @ prio 22**: the decoder's initial-prefetch
  sprint (flat-out decode on every source open = every engine transition)
  broke reception even with the IRAM ISR.  The historical "decoder on Core 1
  = 10× underruns" was a priority mistake (it was tried at prio 5, starving
  BELOW the producer/consumer pair), not a core problem.
- **EX frame parser validates the type byte** (0x01/0x03 only): a stray
  0x3E after a gap or echo leak seeded a bogus frame whose "length" was a
  channel-value byte, swallowing valid polls — one glitch became a
  seconds-long channel/telemetry blackout.
- **TX self-echo tail drained deterministically**: flushRx() ran before the
  echo's last bytes physically landed (echoShort ≈ 100 % of replies), leaking
  bytes into the parser; now a bounded ≤500 µs wait inside the master's quiet
  window drives the leak to zero.
- **Failsafe debounce** (TriggerInput): signal-loss behaviours (force_low/…)
  engage only after 500 ms of sustained loss — a single bad frame no longer
  commands "engine off" (Jeti receivers themselves hold ~1 s).
- ESC-telemetry raw-UART RX ring 1024 → 4096 B (~1 s of stream headroom).
- Studio: a topology refresh racing the /hubfx.yaml hydration could invent a
  default ESC stream selector and PERSIST it on Apply, silently overwriting
  the operator's choice (the kontronik→jeti-exbus flip) — defaults are no
  longer invented.
- Reply-gate de-beat: the 12 ms floor against the ~11.6 ms poll period
  skipped almost every other poll; a 3 ms early margin lifts replies from
  ~48 Hz to ~65 Hz (per-sensor refresh up ~35 %).

### New Features
- **RPM scaling by Motor poles + Gear ratio** (Telemetry sub-tab, per
  esc-telemetry port): published RPM = transmitted ÷ (poles/2 × gear) —
  Kontronik transmits ELECTRICAL rpm.  Persisted as `esc_motor_poles` +
  `esc_gear_ratio`; one combined ×100 divider on the wire (unchanged).
- Reply-rate instrumentation: the 2 s `[jexp] TX` line now reports measured
  `respHz` + `vals/s` + the target interval; `[jexp] IN_1 failed-frame[N]`
  hex-dumps the first CRC-failed frame per window (the tool that cracked
  the saga).

### Breaking ⚠️
- **Jeti EX-Bus downstream slave mode REMOVED** (the IN_2 master link that
  polled an ESC as a Jeti slave with mirrored channel frames): native ESC
  telemetry supersedes it, its 25 ms mirror TX taxed the input task, and a
  stale config resurrecting it caused a perf regression.  `esc_protocol:
  jeti-exbus` (or unknown/missing values) now falls back to kontronik; the
  Jeti input attach no longer auto-claims a second input port.

### Internal
- jeti_ex_telemetry_monitor.h deleted; expander is IN_1-only (~10 KB smaller
  firmware).  DRAM audit: branch adds ~3 KB; the session-time drop is the
  (pre-existing, documented) lazy MP3 decoder pool + the 32 KB DMA WAV
  buffer.

---

## 2.38.1 — 2026-07-14

Native ESC telemetry (Kontronik / Scorpion / Hobbywing) on the TELEM port,
a protocol-agnostic telemetry collection, ESC fault messages on the radio,
and RPM pole/gear scaling. Consolidates the unreleased 2.36.1–2.38.0 work.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.38.1 | ESP32-S3 | `hubfx-v2.38.1` |

### New Features
- **ESC Telemetry role (`esc-telemetry`, RoleKind 0x05):** an input port can
  listen to an ESC's native telemetry broadcast — Kontronik KODL/KODI
  (115200 8E1, CRC32, bench-verified on a KOLIBRI), Scorpion Tribunus
  "Unsc Telem" (38400 8N1, CRC16-CCITT), Hobbywing Platinum V4/FlyFun
  (19200 8N1) and Platinum V5 (115200 8N1, CRC16-MODBUS) — normalized to
  RPM/V/I/mAh/throttle/temps/BEC/faults and published to the radio + Studio.
  Each protocol is a decoder class behind a C++20 `EscTelemetryDecoder`
  concept, composed by `EscTelemetryMonitorT<...>` (adding an ESC = one
  decoder + one alias entry). Attach config: `[protocol][baudK][rpmDivider]`.
- **Protocol-agnostic TelemetryHub** (`sfx_telemetry::TelemetryHub`, moved out
  of `jeti_ex/`): producers publish `SensorKind` Int/Gps/DateTime values with
  no radio knowledge; the Jeti responder picks the smallest EX wire type per
  value at encode time (also fixes the fixed-`Int6` throttle truncation trap).
- **ESC fault messages on the radio:** the Kontronik operation-error bits are
  mapped from the official V5 spec (archived in `telemetry/docs/`) with
  warning/error severities; fault CHANGES post a per-device message into the
  hub which the responder forwards as a Jeti **EX Message** packet (v1.07,
  class 2–4 → DC/DS log + popup, e.g. "ESC OVERTEMP +2"). The benign
  ProgAllow bit 17 is masked (it reads constantly while idle).
- **RPM pole/gear scaling:** Kontronik transmits ELECTRICAL rpm — Studio's
  Telemetry sub-tab gains **Motor poles** + **Gear ratio** per esc-telemetry
  port; the firmware divides by (poles/2 × gear) at publish
  (`esc_motor_poles` / `esc_gear_ratio` in `/hubfx.yaml` ports[]).
- **Studio:** ESC telemetry configuration moved to the Input & Ports →
  Telemetry sub-tab (stream selector + RPM scaling + live streaming chip);
  the Input panel card keeps the protocol select with a pointer. The
  Telemetry sub-tab now shows for esc-telemetry sources too.
- **Jeti expander robustness (2.36.1/2.36.2 work):** restart-on-attach (live
  role moves no longer need a reboot), deferred link-loss self-restart
  (unplugged Rx recovers on replug, 15 s watchdog), ESC channel-frame mirror
  emulating the real master cycle on the downstream link (Kolibri needs
  channel frames before its telemetry-reply slot opens; rev-B-only).
- **LightFX Studio:** loading a program template on a fresh board now seeds
  channels from unclaimed LED-animator ports (mapped + renamed) instead of a
  half-empty program.

### Bug Fixes
- **Raw-UART listen is RX-only:** the TX pad (GPIO3 sits directly on the
  TELEM line on rev B) idled push-pull HIGH and flattened the ESC's start
  bits (`rxB=0`) — raw mode never attaches TX; half-duplex roles gate it
  like JETI_EX.
- **UART handoff on role change:** attaching a native ESC protocol makes a
  running JetiExpander release its EX downstream port
  (`setDownstreamPort`) — previously both drained the same UART; switching
  back to jeti-exbus re-adopts it.
- **Kontronik KODI is 44 bytes** (spec field sum; the header sheet's 40 only
  covers KODL) — the info frame now decodes: device identity (KOLIBRI/
  KOSMIK/…) + firmware version, zero CRC errors at 100 frames/s.
- Device name refreshes on every hub push, so the identity upgrades from the
  protocol default once the info frame lands.

### Protocol Changes
- RoleKind 0x05 renamed `jeti-ex-telemetry` → `esc-telemetry` (legacy yaml
  names map over; zero-length attach config = the old downstream-marker
  semantics, so existing configs keep working).
- Telemetry-collection sensor `type` byte (0xEC) now carries the agnostic
  `SensorKind` (0=int, 1=gps, 2=datetime) instead of the Jeti ExDataType —
  no host code interpreted the old byte.
- UART raw mode gains parity support (`kSerial8E1` — Kontronik).

### Breaking ⚠️
- None on the wire (Rule 11 append-only attach config; legacy names accepted).

### Internal
- Official spec PDFs archived: Kontronik Telemetrieprotokoll V5
  (`telemetry/docs/`) + JETI EX protocol v1.07 (`jeti_ex/docs/`).
- Bench instrumentation: `[esctelem]` 2 s health line (rxB/frames/errs +
  raw-byte snapshot) behind `SFX_INSTRUMENTATION`.

---

## 2.36.0 — 2026-07-14

Battery + expander-rail telemetry on rev B, enabled by the U43 address
rework (0x40 → 0x44) that clears the historic PCA9685 collision.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.36.0 | ESP32-S3 | `hubfx-v2.36.0` |

### New Features
- **INA226 coulomb counter (generic driver capability):** `update()`
  self-integrates consumed charge; `consumed_mAh()` / `resetConsumed_mAh()`
  available to every board that polls an INA226.
- **Rail telemetry on Jeti EX + Studio:** five HubFx-local sensors —
  Batt U (V), Batt I (A), Batt used (mAh), Exp I (A), Exp used (mAh) —
  fed at 2 Hz from the 10 Hz sense cadence; auto-discovered by the
  transmitter and mirrored in Studio's Telemetry panel. Sensors register
  only for monitors that came up (stock un-reworked boards show no Exp
  rows). Undervoltage alert remains parked.
- **U43 re-enabled @ 0x44** (`kInaAddrs = {0x41, 0x44}`) after the bench
  restrap (lift A0) — bench-verified clash-free; PCB rev C bakes it in.
- `tests/hw/i2c_probe` gains a clash detector (repeat-read stability of
  the ID registers — the wire-AND signature of two chips at one address).

### Breaking ⚠️
- None.

---

## 2.35.1 — 2026-07-02

**HubFX PCB rev B support** (branch `pcb-nextver`) + a complete
fresh-board provisioning chain: a factory-new board now goes from blank
silicon to a seeded, Studio-accessible config with one `scalefx-flash
flash hubfx`. (2.35.0 — the rev B pin map — never shipped separately;
2.35.1 folds it in with the storage self-heal.)

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.35.1 | ESP32-S3 | `hubfx-v2.35.1` |
| ScaleFX Studio + CLI | 1.0.0 | Windows (host) | — |

### New Features
- **PCB rev B pin map** behind a compile-time `HUBFX_PCB_REV` switch
  (rev B default; `-DHUBFX_PCB_REV=1` rebuilds for rev A). Rev B: split
  TX/RX input headers (INP RX=GPIO2/TX=GPIO1, TELEM RX=GPIO21/TX=GPIO3,
  2.2 kΩ bridge), servo headers SRV1..10 on the 6 V rail, status LED
  GPIO46, 2 × INA226 rail monitors (battery @ 0x41 driven; expander-rail
  U43 @ 0x40 disabled — see the collision note below).
- **`EspInputPort` split TX/RX** — explicit `(rxPin, uartNum)` single-wire
  and `(rxPin, txPin, uartNum)` constructors; the dedicated TX pad idles
  high-Z and drives only its half-duplex reply slot.
- **Fresh-board provisioning**: `scalefx-flash` now writes the FACTORY
  image (bootloader + partition table + app — LittleFS region untouched,
  existing configs survive), LittleFS self-formats/self-heals, and the
  default `/hubfx.yaml` is seeded after a readiness check.
- `tests/hw/i2c_probe` — minimal I²C bring-up/scan firmware with INA226
  canonical-ID identification.

### Bug Fixes — firmware
- **LittleFS self-heal**: `FlashModule::begin()` retries through an
  explicit format and records the error code; `FLASH_STATUS_REQ` retries
  a failed boot-time init (the self-recovery bring_up.h always promised);
  the boot log states WHY flash init failed.
- **Uploads into missing directories** no longer fail with
  `FILE_IO_ERROR`: ESP32 write-opens create the parent chain (mkdir -p)
  — fixes applying a LightFX preset program to a fresh board; covers
  flash AND SD.

### Bug Fixes — ScaleFX Studio
- **Fresh-board connect no longer freezes/blanks the UI**: empty
  `devicemodel:changed` broadcasts are suppressed (Go) and ignored over a
  populated model (frontend); a **Svelte flush watchdog** self-heals the
  swallowed-exception scheduler wedge within 1 s and logs the culprit
  (`FE.FLUSH`).
- A **disabled** effect no longer gates the global Apply (fresh boards
  showed a permanent red "resolve errors: enginefx" from the disabled
  default draft).
- `scalefx-flash` seeding reports WHY it skipped (flash unavailable)
  instead of silently preserving nothing.

### Hardware findings (documented, fix scheduled for PCB rev C)
- **The rev A "counterfeit INA226 @ 0x40" was never a clone**: the
  PCA9685's hardware address is 0x40 (all A-pins grounded; the firmware's
  0x70 is its all-call alias) — an address collision with U43. Full
  re-interpretation in instructions/18; findings log + rev C checklist in
  `hardware/pcb-nextver/ISSUES.md` (also covers the C2 MLCC dead-short
  that smoked the first rev B board, and the unprotected VBAT feed on the
  USB1/USB4 expander ports).

### Breaking ⚠️
- None on the wire. Rev A boards must build with `-DHUBFX_PCB_REV=1`.

---

## 2.34.1 — 2026-06-23

Post-RC1 maintenance: a new **manual gear setup/maintenance** feature, several
ScaleFX Studio configuration fixes, and a broad firmware-hardening pass from a
full audit of the HubFX firmware + shared libraries.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.34.1 | ESP32-S3 | `hubfx-v2.34.1` |
| ScaleFX Studio + CLI | 1.0.0 | Windows (host) | — |

### New Features
- **Manual door + strut control (gear setup/maintenance).** Each strut card in
  Studio gains a *Manual / maintenance* section to open/close the doors and
  drive the strut up/down independently of the full deploy/retract sequence,
  plus fleet "all" buttons. Firmware-enforced safety interlocks block the unsafe
  combinations — you can't close the doors while the strut is out, and you can't
  move the strut unless the doors are open; the matching button greys out and
  says why.

### Bug Fixes — ScaleFX Studio
- Port-role dropdowns no longer blank to empty strings after Apply/Save.
- A gear motor keeps its Forward/Reverse direction across a calibrate + save.
- Servo `setProfile()` keeps the role's reversed flag coherent with the
  profile's inverted bit, so deploy/retract no longer drives the wrong
  calibrated end after a profile-only update.

### Bug Fixes — firmware hardening (audit pass)
A full audit of the HubFX firmware + shared libraries surfaced and fixed **47
issues across 10 subsystems** (7 HIGH severity). Highlights:
- **audio** — Q15 volume overflow inverted phase at unity gain; the MP3 tail
  fade-out never armed on a frame-count overshoot; widened the decoder teardown
  wait to fully close a source use-after-free window.
- **storage** — status / SD-init commands dispatched mid-upload self-deadlocked
  on the storage mutex; the upload-active flags are now cross-task atomic;
  zero-byte BATCH uploads complete instead of hanging.
- **usb** — CDC device tracking is now slot-stable (compaction silently
  re-pointed a surviving expander's wire at the wrong device); a close-vs-TX
  use-after-free handshake; a devAddr-wrap sentinel collision.
- **config** — a tab-indent byte-offset bug overshot the YAML content pointer;
  over-deep documents no longer silently mis-parent; store reset/invalidation
  correctness.
- **effects** — sequenced gear no longer stalls forever on one strut's fault;
  a landing-gear travel backstop; an engine cross-fade ping-pong on short
  tracks; two-edge + reversed-channel selector hysteresis.
- **roles / board** — a free-running stall now cuts motor power; servo velocity
  telemetry int16 clamp; motion-profile + effect-clock telemetry correctness.
- **peripherals / platform / serial** — INA226 divide-by-zero guard; a
  shared-I2C bus mutex; null-mutex guards; stale pending-query-tag reset;
  diag-log torn-entry-under-lock.

### Protocol Changes
- **Additive only (Rule 11).** New packets `GEAR_DOOR` / `GEAR_STRUT` /
  `GEAR_DOOR_ALL` / `GEAR_STRUT_ALL` (`0x01–0x04`) and a two-byte STATUS tail
  `[doorsOpen][strutState]`. No breaking changes — an older master ignores the
  new tail. The audit-hardening fixes change no wire formats.

### Version bump
- **MINOR** 2.33.2 → 2.34.0 for the additive manual-gear packets, then **PATCH**
  → 2.34.1 for the audit-hardening logic fixes (no further wire changes).

---

## RC1 — 2026-06-14 (Release Candidate 1)

The first coordinated release candidate of the full system. This RC lands a
ground-up rewrite of the **retractable landing-gear / undercarriage** subsystem
— from a brittle one-shot op-queue into a robust, target-driven state machine
with clean pre-emption, real safety behaviour, and a control-and-status surface
in ScaleFX Studio + the CLI.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.33.2 | ESP32-S3 | `hubfx-v2.33.2-rc1` |
| GearControl (expander) | 1.0.0 | Pico (RP2040) | `gearcontrol-v1.0.0-rc1` |
| LightFX (expander) | 1.0.0 | Pico (RP2040) | `lightfx-v1.0.0-rc1` |
| ScaleFX Studio + CLI | 1.0.0 | Windows (host) | `studio-v1.0.0-rc1` |

---

## Headline: the gear / undercarriage subsystem

A scale aircraft's retractable gear is more than a motor — each leg is a small
choreography: doors swing open, a strut drives out to its endstop, the doors
close again, and the whole set has to do this *together*, *safely*, and react
the instant the pilot changes their mind. RC1 models exactly that.

### Every strut is a target-driven state machine

Each landing strut now owns a **target** (Gear Up or Gear Down) and a single
engine that, from wherever the strut currently is, computes the next move toward
that target. It walks a symmetric transit:

```
Gear Up  ⇄  doors opening ⇄ doors open ⇄ strut moving ⇄ strut done
         ⇄  doors closing  ⇄ doors closed ⇄  Gear Down
```

Because every action is recomputed from "where am I + what's my target," the
hard parts come for free:

- **Clean mid-cycle pre-emption.** Flip the switch from *down* to *up* while the
  gear is half-way out and it simply **reverses** — the motor re-seeks the other
  way, doors that were closing re-open — with no stop/restart stutter and no
  illegal states. Reversal is a property of the machine, not a special case.
- **Single signal in, full choreography out.** One stick movement (or one button)
  drives the entire doors → strut → doors sequence; the operator never sequences
  legs by hand.
- **Per-leg watchdogs.** Door legs complete on the servo's motion-done event with
  a re-armed timeout backstop, so a lost event can never hang the machine; the
  strut leg is bounded by the motor's endstop seek + travel timeout.

### Door choreography

Each strut drives up to two door servos with selectable sequencing — both
together, staggered, or one-then-the-other — and a post-deploy close policy
(close both, close one, or leave open). Doors and strut are interlocked: the
strut only runs once the doors are open, and a retract re-stows whatever it
opened. A strut with no doors simply runs bare.

### Coordination across the set

Two deliberately-simple modes coordinate the legs:

- **Independent** — each strut runs (and pre-empts) on its own.
- **Full-sync** — the whole set moves in lockstep: all doors open together, then
  all struts run together, then all doors close together.

### Safety is first-class

- **Emergency hold (E-Stop).** Freeze the set — or a single strut — exactly where
  it is: brake the motor, hold the doors. Position becomes "uncertain," and the
  gear resumes on the next Gear Up *or* Gear Down command.
- **Self-homing from unknown.** At power-up a strut's physical position is
  honestly *Unknown*; the first command drives it to a known endstop (the stall
  guard trips when it arrives) rather than assuming a position.
- **No-motor detection.** If a strut is commanded to move but draws essentially
  no current, the firmware concludes the motor is missing / unplugged / the
  H-bridge output is dead — a **distinct fault from a stall or a timeout** — and
  reports it as such.
- **Fail-safe to gear-down.** A lost RC link lowers the gear (the safe state for
  a landing aircraft); an optional input-link-loss watchdog can emergency-deploy
  the whole set.
- **Faults are recoverable.** A faulted strut reports *why* (timeout / no-motor),
  and the next Gear Up / Gear Down auto-clears the fault and retries — no manual
  reset dance.

### Pilot-facing control

- **RC up/down channel** — switch ON lowers the gear, OFF raises it (the natural
  "flip on to deploy for landing" convention; reverse it on the radio, not here).
- **Single-step** — advance the transit one leg at a time for bench setup.

---

## Control & status surface (ScaleFX Studio + CLI)

The host side was rebuilt around the new model so the operator can *see and
drive* the gear without guesswork.

- **Live status, per strut.** A read-only status row for each leg shows its phase
  (gear up / lowering / gear down / raising / held / error / unknown), its door
  state (open / closed / moving), and a live **Doors ▸ Strut ▸ Doors** lifecycle
  strip that lights up the stage currently in motion. Door UI appears only for
  struts that actually have doors.
- **Fleet + per-strut control.** A single fleet row drives the whole set (Gear
  Down / Gear Up / E-Stop) with a sync-mode selector; per-strut manual control
  (Gear Up/Down · Hold · Reverse · Resume · Retry) lives in each strut's
  configuration card.
- **Master enable** gates the whole subsystem; status re-polls on (re)connect so
  the panel never sits on a stale reading.
- **CLI** — a richer, colour-coded `gear-status` (name · state · stage · detail)
  and a verbose `gear-info` with a textual transit cycle bar, plus `gear-step`
  and `gear-estop`.

---

## Per-component summary

### HubFX 2.33.2 (ESP32-S3 master)
Hosts the per-strut state machines, the multi-strut coordinator, the RC-channel
driver, and the gear-bound landing-light fan-out. New wire surface (all Rule 11
append-only, old hosts keep decoding): `GEAR_ESTOP` (0xD8), `GEAR_STEP` (0xD9),
a trailing `errReason` byte on gear status / phase events, `GearError::NO_MOTOR`
(0x66), and `BiMotorSeekOutcome::NoLoad` (3).

### GearControl 1.0.0 (Pico expander)
Drives the H-bridge gear motors and door servos. Adds **no-load / open-circuit
detection** in the bi-directional DC-motor role: a driven-but-currentless seek
finishes with the `NoLoad` outcome that the hub maps to a *no-motor* fault.

### LightFX 1.0.0 (Pico expander)
Carried forward unchanged in this RC.

### ScaleFX Studio + CLI 1.0.0 (host)
The redesigned Gear / Undercarriage panel and the gear CLI commands described
above. Internal: Rule 63 (uniform control-row height); Rule 11 wire mirrors for
the gear additions.

Shipped as a **self-contained Windows package** (`scalefx-studio-v1.0.0-rc1.zip`)
— unzip and run `scalefx-studio.exe`, no Python or toolchain needed. Bundles
`scalefx-studio.exe`, `scalefx-cli.exe`, `scalefx-flash.exe`, and `esptool.exe`
(the ESP32-S3 flashing backend, found colocated next to `scalefx-flash.exe`).
Reproduce the package with [`tools/package-studio.ps1`](tools/package-studio.ps1)
(`pwsh tools/package-studio.ps1 -Version 1.0.0-rc1`).

---

## Verification & known issues

**Pre-merge gate: 28/28 PASS** — Go unit suites, native C++ doctest (103 cases),
Studio frontend Vitest, the HubFX hardware integration suite, and the hubfx
firmware build.

**Before promoting RC1 → release:**
- **Flash the GearControl expander** for the no-motor detection — it runs in the
  motor role on the Pico, not on the hub.
- **Bench-verify on hardware:** mid-cycle pre-emption, E-Stop hold/resume, the
  door no-op fix (`close_policy: none` retract), Full-sync coordination, and the
  RC up/down channel.

---

## Earlier releases

See the [GitHub releases](https://github.com/mkudzia84/scalefx/releases) page for
the history prior to RC1.
