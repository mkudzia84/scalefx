# ScaleFX Releases

Release notes for the ScaleFX multi-platform effects system. Each firmware
component is versioned and released independently (tag `‹component›-v‹version›`);
the host tools (Studio + CLI) ship together. GitHub releases carry the flashable
firmware binary; ScaleFX Studio's **Firmware** tab can flash a release directly.

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
