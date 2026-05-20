# Hardware TODO

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
