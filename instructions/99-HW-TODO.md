# Hardware + Feature TODO

> **Status:** hardware spec &middot; **Read when:** planning hardware bring-up or picking up a backlog feature.
> **TL;DR:** running backlog of hardware + firmware feature work (not-yet-done); check here before assuming a feature exists.

## Firmware feature refactors

1. **GunFx — split `GunDef` into `GunWiring` + `GunPreset`** to support
   the preset library pattern (see [19-HUBFX-CONFIG-SCHEMA.md
   §GunFx presets](19-HUBFX-CONFIG-SCHEMA.md)). Today `GunDef` mixes
   airframe-wiring (port refs, audio channel) with behaviour (timing,
   recoil, fire sound, smoke target). The split:
   - `GunWiring` — `id`, `name`, `muzzleFlash`, `recoilServo`,
     `smokeHeater`, `trigger`, `audioChannel`, and a `preset: char[]`
     ref. Lives in `hubfx.yaml` `gunfx:` array.
   - `GunPreset` — `flashDurationMs`, `flashBrightness`, `recoilCenterUs`,
     `recoilJerkUs`, `recoilHoldMs`, `smokeTargetCx10`,
     `triggerThresholdUs`, `defaultIntervalMs`, `fireSoundPath`,
     `outputMask`. Lives in `/gunfx/presets/<Name>.yaml`.
   - `GunFxServicePolicy::configure(wirings, presets)` — the service
     resolves the preset by name into each gun unit's effective config
     at apply time. Inline `overrides:` in `hubfx.yaml` lets the
     airframe tweak any preset field without forking the file.

2. **Input settings + wiring abstraction.** Per-input port mode
   (`pulse|sbus|jeti_ex|uart_raw`) needs an explicit `inputs:` section
   in `hubfx.yaml` + a role-attach path that calls the right
   `configureXxx()` on `EspInputPort`. See
   [19-HUBFX-CONFIG-SCHEMA.md §Deferred](19-HUBFX-CONFIG-SCHEMA.md).

## GearControl

1. Move the VCC/BAT jumper away from servos for usability
2. Servo pin layout is wrong with the GND in the MIDDLE! to fix

## HubFX (8-channel rev A)

1. ~~**Replace U43 (INA226 @ 0x40)** on every board — "counterfeit"
   reporting `mfg=0x0001 die=0x0020`.~~ **MOOT (2026-07-02):** the
   "clone" was never a bad part — the PCA9685's hardware address is
   ALSO 0x40 (all A-pins grounded; firmware's 0x70 is its all-call
   alias), so 0x40 always hosted two chips and the garbage IDs are the
   wire-AND of both answering. Replacing the chip cannot fix an address
   collision. Full re-interpretation in
   [18-HUBFX-INA-CLONE-WEDGE.md](18-HUBFX-INA-CLONE-WEDGE.md); the
   real fix is the rev C restrap below.

## HubFX rev C (fixes for issues found on rev B / pcb-nextver)

Full bring-up findings log:
[hardware/pcb-nextver/ISSUES.md](../hardware/pcb-nextver/ISSUES.md).

1. **Fix the 0x40 I²C address collision** (carried since rev A, netlist-
   proven + bench-confirmed on rev B 2026-07-02): strap the PCA9685 out
   of the INA address range (e.g. A2 → 3V3 = 0x44) and/or restrap U43
   A0 → SDA (= 0x42). Then re-enable the expander-rail monitor in
   `kInaAddrs` (rev B firmware runs battery-only, `{0x41}`). Rev B
   board rework alternative: lift U43 pin 2 (A0), jumper to SDA → 0x42.
2. **Protect the VBAT feed to USB1/USB4** (expander ports). VBUS pins
   carry raw battery with NO fuse/limit — a shorted cable or expander
   pulls unlimited pack current, and a PC misplugged into an expander
   port meets 12 V. Add a per-port e-fuse (TPS2595x-class, ~2-3 A
   limit) or at minimum polyfuses + unmistakable silkscreen.
3. **MLCC flex-cracking on VBAT** — C2 (4.7 µF 0805 directly across
   VBAT) failed as a DEAD SHORT on the first rev B board (smoke on
   battery connect; board recovered by removing C2). Consider soft-
   termination MLCCs / tantalum for caps directly across the battery
   rail, and keep them away from board-flex zones (mounting holes,
   connector edges).
