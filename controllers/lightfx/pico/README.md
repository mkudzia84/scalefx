# LightFX Pico — generic-expander board

> Thin port + role expander for scale-model lighting (RP2040).

## Overview

LightFX is a **dumb expander** in the ScaleFX generic-expander model: it
exposes its physical ports over USB CDC and lets the **HubFX master**
attach roles and drive them. All LED program sequencing, landing-light
state machines, and RC program-select logic live in the hub's
`LightFxEffectServicePolicy` / `LandingLightServicePolicy` — **not on this
board**. (This replaced ~760 lines of on-board program-engine / landing-
light / RC-input code; see
[instructions/15-GENERIC-EXPANDER-REFACTOR.md](../../../instructions/15-GENERIC-EXPANDER-REFACTOR.md).)

**Hardware:** Raspberry Pi Pico (RP2040), earlephilhower core
**Protocol:** Binary COBS / CRC-8 over USB CDC (6 Mbps)
**Framework:** `BoardOf<LightFxBoard, BatteryServicePolicy<AdcDividerBatteryT<6180>>>`
— auto Board + Indicator + Port + Role service policies, plus the battery
monitor user policy.

## Ports exposed

| Kind | Count | GPIO | Role (attached by hub) |
|---|---|---|---|
| PWM   | 8 | GP0..7 | `LedAnimator` (LED channels — the hub plays LightFx programs; the landing-light bulb is just one of these channels) |
| Servo | 3 | GP8, GP9, GP10 | `ServoActuator` (e.g. landing-light deploy servos) |

**No input port** — the legacy GP8 multi-modal RC PWM input (standalone
program-select) is retired. RC reading is centralized on the hub, which
drives program selection over the wire. GP8 is now a plain servo output
(Rule 31 — port direction is fixed at declaration).

Indicator LEDs (GP24 blue = connection, GP25 yellow = error) are driven by
the auto `IndicatorServicePolicy`.

The hub's `/hubfx.yaml` `expanders:` block maps this board (by GUID/alias)
to its port → role attachments; effects then address its channels by alias
— see [instructions/19-HUBFX-CONFIG-SCHEMA.md](../../../instructions/19-HUBFX-CONFIG-SCHEMA.md)
and [media/presets/lightfx/](../../../media/presets/lightfx/).

## Battery monitoring (ADC + resistor divider)

A `BatteryServicePolicy<AdcDividerBatteryT<6180>>` user policy senses pack
voltage on **GP29** through a ÷6.18 resistor divider (≈41k/8k). It:

- advertises `CoreCapability::BATTERY` in IDENTIFY,
- handles `BATTERY_CONFIG` (0xEE) — chemistry + cell-count adjustments from
  the hub (cell count `0` = re-arm auto-detect), and
- rides the periodic STATUS broadcast with a battery section appended via
  `board.core().onStatusData(...)`:
  `[present:u8][voltage_mV:u16LE][cellCount:u8][pct:u8][flags:u8]`
  (flags: bit0 = low, bit1 = critical).

`battery.update()` runs each loop (the sensor throttles its own ADC reads).
Defaults: LiPo, auto cell-count detection. Cutoff *reactions* (e.g. dimming
channels on low voltage) are a hub-side policy decision, not on-board.

## Pinout (RP2040 GPIO)

```
GP0..7   : LED PWM channels (CH1..CH8)
GP8/9/10 : servo headers (SRV1..SRV3)
GP24/25  : indicator LEDs (connection / error)
GP29     : battery voltage sense (ADC, ÷6.18 divider)
```

## Build / flash

```bash
app/go/scalefx-flash.exe build lightfx --no-clean
app/go/scalefx-flash.exe flash lightfx --no-clean   # board must be in BOOTSEL
```

The hub talks to this board through the topology surface — verify with
`topo-ports` / `topo-roles` / `system-info` once it enumerates.
