# Recommended setup workflow

Order matters: the radio input and the ports must be set up before the effects
can use them. The Setup Wizard walks this exact path.

1. **Pick features.** Choose which effects you want (engine, guns, gear,
   lighting, landing lights). Turning one on creates a sensible starter setup.
2. **Set up the radio input.** Choose the link type (standard servo-pulse / PPM,
   SBUS, or Jeti) and how many channels. There's a live auto-detect if you're
   unsure.
3. **Map channels to functions.** Give each channel a name (*engine on/off*,
   *landing gear*, *gun fire*, *light program*, …). The wizard warns if an effect
   you turned on has no channel mapped to it.
4. **Set up each effect.** For each effect, pick its ports from the free,
   compatible ones (the wizard attaches the right behaviour for you) and set the
   few settings that matter — defaults are sensible.
5. **Review & Apply.** Check the summary and Apply. Fine-tune later in the
   per-effect tabs.

## Quick recipes per effect

- **Engine:** pick a type and speaker output → map *engine on/off* to a switch →
  set the failsafe to "stop on signal loss" → choose a **running** sound (start
  and stop sounds optional).
- **Gear:** map *landing gear* to a switch (ON = down) → for each leg, pick a
  motor port, set the deploy direction and a travel timeout → optionally add door
  servos and an opening style → turn on **deploy-on-signal-loss**.
- **Gun:** map *gun fire* → pick an LED port for the muzzle flash, set duration
  and brightness → optionally add smoke (heater + fan ports) and a turret (yaw /
  pitch servos).
- **Lighting:** add LED channels (one port each) with a default brightness →
  optionally add a program-selector channel → create a program and design its
  timeline on the Lighting tab.
- **Landing lights:** for each light, pick the deploy servo(s) and LED(s), a
  fade-in time, and how it activates (manual or a radio channel).

## Typical channel maps

- **Warbird with retracts + guns (6+ channels):** the usual flight channels
  (throttle, ailerons, elevator, rudder), then *landing gear* on a 2-position
  switch, *gun fire* on a momentary switch, *engine on/off* on a switch, and
  optionally *light program* on a 3-position switch. ScaleFX only reads the
  channels you map — your flight controls are untouched.

## Safety reminders

- Bench-test gear and motors with the model **supported and supervised** — a
  wrong direction or timeout can stall a motor or strip a gearbox.
- Set the gear failsafe so a lost radio link lowers the gear.
- Calibrate servo end-points before driving doors or a turret to full travel.
