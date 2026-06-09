# LightFX Pico — generic-expander board (RP2040)

A **thin port + role expander**. It exposes its physical ports and lets the
HubFX master attach roles and drive them over USB CDC. All LED program
sequencing, landing-light state machines, and RC program-select logic live on
the hub (`LightFxEffectServicePolicy` / `LandingLightServicePolicy`) — this
board carries none of it.

> Resurrected 2026-06-09 against the current `BoardServer` API. The prior
> standalone build was retired 2026-06-06 when the LightFx effects moved to the
> HubFX master; this rebuild restores the physical expander board with the
> gearcontrol-era shape (explicit USB-CDC wire stream + `PortCapacity<>`).

## Ports exposed

| Port kind | Count | GPIO | Hub-attached role |
|---|---|---|---|
| PWM (active-high LED) | 8 | GP0–GP7 | `LedAnimator` |
| Servo | 3 | GP8 / GP9 / GP10 | `ServoActuator` |

There is **no input port** — RC reading is centralized on the hub, which drives
program selection over the wire. Per Rule 31 the port direction is fixed at
declaration; GP8 is a plain servo output, not the legacy multi-modal RC input.

Roles are **not** attached on the board. `RoleServicePolicy` (auto-composed via
`BoardOf<>`) accepts `ROLE_ATTACH` from the hub and emplaces the
`LedAnimator` / `ServoActuator` variants at runtime.

## Battery

An ADC + resistor-divider sensor on **GP29** (÷6.18 divider) wired through
`BatteryServicePolicy` (an `ExtraPolicy` on `BoardOf<>`):

- advertises `CoreCapability::BATTERY` in IDENTIFY
- handles `BATTERY_CONFIG` (chemistry / cell count pushed from the hub)
- rides the periodic STATUS broadcast with a battery section
  (`[present][voltage_mV][cellCount][pct][flags]`, Rule 11 append-only)

## Pinout (RP2040 GPIO)

```
GP0..7   : LED PWM channels (CH1..CH8)
GP8/9/10 : servo headers (SRV1..SRV3)
GP24/25  : indicator LEDs (blue = connection, yellow = error)
GP29     : battery voltage sense (ADC, ÷6.18 divider)
```

The 2 indicator LEDs are driven by the auto-prepended `IndicatorServicePolicy`
via `board.begin()`.

## Build / flash

```
app/go/scalefx-flash.exe build lightfx --no-clean
app/go/scalefx-flash.exe flash lightfx --no-clean
```

USB descriptor: VID `0x2e8a`, PID `0x0181`, product `LightFX`.
