# The effects — what they do and what they need

For each effect: what it does, the ports it uses, the radio channel(s) it reads,
and the settings that matter. Refer to ports by their friendly names in Studio.

## Engine sound

Plays layered engine audio (turbine / piston / electric) through the model's
speaker. No output port to assign — sound goes to the built-in speaker output.

- **Radio channel:** an on/off switch (*engine on/off*).
- **Key settings:** engine *type*; *speaker* output (left / right / both); the
  on/off switch threshold; **failsafe** (what to do if the radio signal is lost
  — usually "stop"); and the sound files — a **running** sound (required), plus
  optional **start-up** and **shut-down** sounds.

## Guns

Per gun: a muzzle-flash LED, rate of fire, an optional recoil kick, optional
smoke, and an optional aiming turret.

- **Radio channels:** *gun fire* (trigger); optionally a *fire-mode* channel
  (rate-of-fire selector), *smoke*, and turret *yaw* / *pitch*.
- **Ports:** muzzle flash → an LED port; smoke → an LED/PWM port for the heater
  plus one for the fan; turret → servo ports for yaw and pitch.
- **Key settings:** fire trigger threshold and rate of fire; muzzle flash
  duration and brightness; recoil strength; smoke heater voltage.

## Retractable landing gear

Raises and lowers landing-gear legs (a motor per leg), with optional gear-bay
doors (servos) and coordination between legs.

- **Radio channel:** a switch (*landing gear*) — switch ON lowers the gear.
- **Ports:** each leg → a motor port; each door (up to two per leg) → a servo
  port.
- **Key settings:** *coordination* (each leg independent, or all in sync); the
  deploy direction and a travel timeout per leg; door *opening style* (together,
  staggered, or one-then-the-other) and what the doors do after; and
  **deploy-on-signal-loss** (drop the gear if the radio link is lost — a good
  safety default).

## Landing lights / searchlights

Retractable searchlights: a servo deploys the light and LEDs fade in.

- **Per light:** one or more deploy **servo ports** + one or more **LED ports**,
  a fade-in time, and an activation source (manual, a radio channel, or tied to
  a lighting program). Radio channel: *landing lights*.

## Lighting

Programmable LED channels (navigation, strobe, beacon) with timeline programs
and an optional program selector on the radio.

- **Radio channels:** *light program* (pick a program by switch position) and
  *light brightness* (master dimmer).
- **Ports:** each LED channel → an LED port.
- **Key settings:** master brightness; each channel's default brightness; the
  program-selector channel; and the program timelines (designed on the Lighting
  tab — the wizard just creates a blank program to start from).

## Radio input

ScaleFX reads your receiver using one of the common RC link types (standard
servo-pulse / PPM, SBUS, or Jeti). You choose the link type and how many
channels, then map channels to named functions (above). Some links also send
telemetry back to your transmitter.

## Audio, alerts, and battery

- **Audio** — effect sounds are files on the model's memory card, mixed to the
  speaker output.
- **Alerts** — spoken or tone warnings (e.g. low battery).
- **Battery** — if a board reports pack voltage, Studio shows it; you set the
  low-voltage cutoff and chemistry, and alerts fire below the cutoff.
