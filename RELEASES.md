# ScaleFX Releases

Release notes for the ScaleFX multi-platform effects system. Each firmware
component is versioned and released independently (tag `‹component›-v‹version›`);
the host tools (Studio + CLI) ship together. GitHub releases carry the flashable
firmware binary; ScaleFX Studio's **Firmware** tab can flash a release directly.

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
