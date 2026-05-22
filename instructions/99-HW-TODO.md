# Hardware + Feature TODO

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

## HubFX (8-channel rev)

1. **Replace U43 (INA226 @ 0x40)** on every board. Two boards out of
   two checked ship a counterfeit chip at this slot that reports
   `mfg=0x0001 die=0x0020` instead of the canonical TI `0x5449 / 0x2260`.
   Firmware now refuses to drive it (would otherwise wedge the PCA9685
   @ 0x70 via shared-bus side effects — full writeup in
   [18-HUBFX-INA-CLONE-WEDGE.md](18-HUBFX-INA-CLONE-WEDGE.md)). Boot
   log shows `[INA] ch1 @ 0x40: NOT DRIVEN — non-canonical IDs …` and
   the channel reports zero V/I until U43 is replaced with a genuine
   TI INA226. Investigate the PCB-house's parts sourcing — likely a
   batch-level substitution.
