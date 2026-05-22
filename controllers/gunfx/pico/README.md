# GunFX Pico — generic-expander board

> Thin port + role expander for gun effects (RP2040).

## Overview

GunFX is a **dumb expander** in the ScaleFX generic-expander model: it
exposes its physical ports over USB CDC and lets the **HubFX master**
attach roles and drive them. All trigger timing, muzzle-flash pulse/fade,
recoil jerk, and smoke heater/fan sequencing live in the hub's
`GunFxServicePolicy` — **not on this board**. (This replaced ~400 lines of
on-board muzzle / smoke / trigger-capture code; see
[instructions/15-GENERIC-EXPANDER-REFACTOR.md](../../../instructions/15-GENERIC-EXPANDER-REFACTOR.md).)

**Hardware:** Raspberry Pi Pico (RP2040), earlephilhower core
**Protocol:** Binary COBS / CRC-8 over USB CDC (6 Mbps)
**Framework:** `BoardOf<GunFxBoard, BatteryServicePolicy<AdcDividerBatteryT<6000>>>`
— auto Board + Indicator + Port + Role service policies, plus the battery
monitor user policy.

## Ports exposed

`idx` order is the hub's PortRef addressing — keep in sync with the hub's
`/hubfx.yaml` gunfx wiring.

| Kind | idx | GPIO | Role (attached by hub) |
|---|---|---|---|
| PWM   | 0 | GP25 | `LedAnimator` (muzzle flash) |
| PWM   | 1 | GP16 | `DcMotor` (smoke fan) |
| PWM   | 2 | GP17 | `Heater` (smoke heater relay) |
| Servo | 0..2 | GP1, GP2, GP3 | `ServoActuator` (recoil headers) |

**No input port** — the legacy GP0 RC PWM trigger capture is retired. The
hub reads the gun-trigger channel on its own input and drives the muzzle /
recoil / smoke outputs over the wire (matches GearControl). Port direction
is fixed at declaration (Rule 31); GP0 is unused.

Indicator LEDs (GP13 blue = connection, GP14 yellow = error) are driven by
the auto `IndicatorServicePolicy`.

## Battery monitoring (ADC + resistor divider)

A `BatteryServicePolicy<AdcDividerBatteryT<6000>>` user policy senses pack
voltage on **GP29** through a ÷6 resistor divider (≈50k/10k). It:

- advertises `CoreCapability::BATTERY` in IDENTIFY,
- handles `BATTERY_CONFIG` (0xEE) — chemistry + cell-count adjustments from
  the hub (cell count `0` = re-arm auto-detect), and
- rides the periodic STATUS broadcast with a battery section appended via
  `board.core().onStatusData(...)`:
  `[present:u8][voltage_mV:u16LE][cellCount:u8][pct:u8][flags:u8]`
  (flags: bit0 = low, bit1 = critical).

`battery.update()` runs each loop (the sensor throttles its own ADC reads).
Defaults: LiPo, auto cell-count detection.

## Pinout (RP2040 GPIO)

```
GP1/2/3 : recoil servo headers (SRV1..SRV3)
GP16    : smoke fan motor (PWM)
GP17    : smoke heater relay (PWM / on-off)
GP25    : muzzle flash LED (PWM)
GP13/14 : indicator LEDs (connection / error)
GP29    : battery voltage sense (ADC, ÷6 divider)
```

## Build / flash

```bash
app/go/scalefx-flash.exe build gunfx --no-clean
app/go/scalefx-flash.exe flash gunfx --no-clean   # board must be in BOOTSEL
```

The hub talks to this board through the topology surface — verify with
`topo-ports` / `topo-roles` / `system-info` once it enumerates.
