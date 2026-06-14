# ScaleFX Releases

Release notes for the ScaleFX multi-platform effects system. Each firmware
component is versioned and released independently (tag `‹component›-v‹version›`);
the host tools (Studio + CLI) ship together. GitHub releases carry the flashable
firmware binary; ScaleFX Studio's **Firmware** tab can flash a release directly.

---

## RC1 — 2026-06-14 (Release Candidate 1)

First coordinated release candidate of the full system, headlined by the
gear/undercarriage overhaul.

| Component | Version | Platform | Tag |
|-----------|---------|----------|-----|
| HubFX (master) | 2.33.2 | ESP32-S3 | `hubfx-v2.33.2-rc1` |
| GearControl (expander) | 1.0.0 | Pico (RP2040) | `gearcontrol-v1.0.0-rc1` |
| LightFX (expander) | 1.0.0 | Pico (RP2040) | `lightfx-v1.0.0-rc1` |
| ScaleFX Studio + CLI | 1.0.0 | Windows (host) | `studio-v1.0.0-rc1` |

### HubFX 2.33.2 (ESP32-S3 master)

**New features**
- **Target-driven gear/undercarriage FSM.** Each strut is a target-driven state
  machine (`pump()` engine) that walks a symmetric transit (doors open → strut
  moves → doors close) and **pre-empts cleanly** when the target flips mid-cycle
  (reverse by re-issuing the seek — no stop/restart). Boot state is `Unknown`
  (position self-homes on the first command).
- **Emergency hold (`GEAR_ESTOP` 0xD8)** — brake the motor + freeze the doors in
  place (→ `Held`); resumes on the next Gear Up *or* Gear Down. Whole-set or
  per-strut.
- **Single-step (`GEAR_STEP` 0xD9)** — advance one leg of the transit, then park.
- **Auto error-reset** — a Gear Up / Gear Down (manual, fleet, or RC) clears a
  prior fault and retries; no manual reset needed.
- **Fault reasons on the wire** — `GEAR_STATUS_RESP` / `GEAR_PHASE_EVENT` carry a
  reason byte (timeout / **no-motor**), surfaced in Studio + CLI.
- **RC up/down channel polarity** — switch ON (above threshold) = gear **down**,
  OFF = gear **up**; RC-loss failsafe always lowers the gear.
- **Two coordination modes** — Independent or Full-sync (all doors lockstep →
  struts → doors).

**Bug fixes**
- **Door no-op** — a strut whose doors stayed open (`close_policy: none`) no
  longer stalls 4 s on the door backstop when raising; a command to a door
  already at its commanded end completes instantly (`DoorSequencer` tracks
  per-door position).

**Protocol changes (Rule 11 append-only)**
- `GEAR_ESTOP` 0xD8, `GEAR_STEP` 0xD9; gear status/phase-event gain a trailing
  `errReason` byte; `GearError::NO_MOTOR` 0x66; `BiMotorSeekOutcome::NoLoad` 3.
  Old hosts keep decoding (extra bytes ignored).

### GearControl 1.0.0 (Pico expander)

- **No-motor / open-circuit detection** — during a seek, if the motor is driven
  but draws ≈0 mA past the inrush window, the seek finishes with the new
  `NoLoad` outcome → the hub reports the strut in `Error` with reason
  **no-motor** (distinct from stall/timeout). Self-clears on the next command.

### LightFX 1.0.0 (Pico expander)

- No functional change in this RC (carried forward unchanged).

### ScaleFX Studio + CLI 1.0.0 (host)

**Studio — Gear / Undercarriage panel (redesign)**
- **Master enable** gates the whole subsystem (tab body hides when off).
- **Undercarriage Control** section: single-line fleet row (state + Gear Down /
  Gear Up / E-Stop), a Sync-mode row, and one read-only **status row per strut**
  (fixed-width name, phase pill, door state, live **Doors ▸ Strut ▸ Doors**
  lifecycle strip; door UI only for struts with doors).
- **Per-strut manual control** (context-aware Gear Up/Down · Hold · Reverse ·
  Resume · Retry · Reset) lives in each strut's config-card header.
- Input (RC channel + signal-loss) and Transition-sounds split; live status
  re-polls on (re)connect.

**CLI**
- Richer `gear-status` (name · state · stage · detail, coloured) and a new
  verbose `gear-info` with a transit cycle bar; `gear-step`, `gear-estop`.

**Internal**
- Rule 63 (uniform control-row height); Rule 11 wire mirrors for the gear
  additions.

### Verification
- Pre-merge gate **28/28 PASS** — Go unit, native C++ doctest (103 cases),
  frontend Vitest, HubFX integration suite, hubfx firmware build.

### Known issues / before promoting RC1 → release
- **Flash the GearControl expander** for the no-motor detection (it runs in
  `BiDcMotorRole` on the Pico, not the hub).
- **Bench-verify** on hardware: mid-cycle pre-emption, e-stop hold/resume, the
  door no-op (`close_policy: none` retract), Full-sync coordination, and the RC
  up/down channel.

---

## Earlier releases

See the [GitHub releases](https://github.com/mkudzia84/scalefx/releases) page for
the history prior to RC1.
