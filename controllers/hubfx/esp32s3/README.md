# HubFX ESP32-S3 — Master Controller

HubFX is the ScaleFX master, built on the consolidated board framework. The
firmware is **pure ESP-IDF** (`framework = espidf`, no Arduino — `app_main`
spawns the loop task in [`hubfx_esp32s3.cpp`](src/hubfx_esp32s3.cpp)). One
firmware-level object — `HubFxBoard : sfx_core::BoardOf<...>` from
[sfx_board](../../lib/sfx_board/) — composes the lifecycle service, the
indicator-LED runtime, the hub-local port + role services, and every effect
/ subsystem policy into a single `board.process()` loop.

Architecture diagrams (composition / dispatch / role transport):
[instructions/32-ARCHITECTURE-DIAGRAMS.md](../../../instructions/32-ARCHITECTURE-DIAGRAMS.md).
Hardware reference: [PINOUT.md](PINOUT.md).

## Subsystem status

Every HubFX subsystem is composed into the `BoardOf<...>` pack and working:

| Subsystem | Status |
| --------- | ------ |
| Core protocol (`BoardServicePolicy`: INIT / STATUS / KEEPALIVE / IDENTIFY / I2C_SCAN / DIAG_HISTORY) | wired |
| Indicator LEDs (`IndicatorServicePolicy`) | wired (onboard GPIO48 only) |
| DiagLog over UART | wired |
| Hub-local ports + roles (`PortServicePolicy` / `RoleServicePolicy`) | wired |
| Audio mixer + I²S output (`AudioService`, TAS5825P, dual-core) | wired |
| Storage — LittleFS flash + SD_MMC 4-bit SDIO (`StorageService`) | wired |
| Config stores — seven YAML stores (`ConfigServicePolicy`) | wired |
| USB Host — expander enumeration + IDENTIFY (`HubFxExpanderService`) | wired |
| Topology — GUID-addressed port/role surface (`HubFxTopologyService`) | wired |
| Engine FX (`EngineFxService`) | wired |
| LightFX LED program runtime (`LightFxEffectService`) | wired |
| Landing lights (`LandingLightService`) | wired |
| GearControl gear state machines (`GearControlService`) | wired |
| GunFX trigger / smoke / recoil (`GunFxService`) | wired |
| System alerts / chimes (`AlertService`) | wired |
| RC inputs — PPM / SBUS / Jeti EX fanout (`InputDispatcherService`) | wired |

The board is declared in [`hubfx_esp32s3.cpp`](src/hubfx_esp32s3.cpp) as a
`BoardOf<>` subclass — `BoardOf<TBoard, TStream, Caps, ...ExtraPolicies>`
takes the concrete board type, the wire stream, the hub-local port capacity,
then the effect/subsystem policy pack:

```cpp
class HubFxBoard : public sfx_core::BoardOf<HubFxBoard,
                                            sfx::NativeUartStream,         // TStream (Rule 33)
                                            sfx_core::PortCapacity<10,8,0,2>,
                                            HubFxExpanderService,
                                            HubFxTopologyService,
                                            InputDispatcherService,
                                            LandingLightService,
                                            LightFxEffectService,
                                            GearControlService,
                                            EngineFxService,
                                            GunFxService,
                                            StorageService,
                                            AudioService,
                                            AlertService,
                                            ConfigServicePolicy> { ... };
```

`BoardServicePolicy` + `IndicatorServicePolicy` + `PortServicePolicy` +
`RoleServicePolicy` are prepended automatically by `BoardOf<>` — they're not
part of the user-policy pack. Every policy satisfies the
`sfx_core::SystemServicePolicy` concept.

## Build

```bash
app/go/scalefx-flash.exe build hubfx
app/go/scalefx-flash.exe flash hubfx --no-clean
```

## Known hardware issue (8-channel rev)

U43 — the INA226 at I²C `0x40`, channel-1 rail monitor — ships as a
counterfeit on every board checked so far. Boot MFG/DIE readback is
`0x0001 / 0x0020` instead of TI's canonical `0x5449 / 0x2260`. The
firmware now detects this in `INA226::begin()` and refuses to drive
the chip, because writing to it corrupts the PCA9685 @ `0x70` on the
shared bus. Boot log surfaces it as:

```
[WARN] [INA] ch1 @ 0x40: NOT DRIVEN — non-canonical IDs mfg=0x0001
              die=0x0020 (expected 0x5449/0x2260, TI INA226).  Clone
              detected — refusing to drive to protect other chips on
              the shared I²C bus.
[WARN] [INA] 7/8 monitors up (1 clone skipped — replace to restore full V/I sense)
```

Channel-1 V/I sense reads zero until U43 is replaced with a genuine
TI INA226. The other 7 channels are unaffected. Full bisection +
mechanism writeup in
[instructions/18-HUBFX-INA-CLONE-WEDGE.md](../../../instructions/18-HUBFX-INA-CLONE-WEDGE.md).

## Version history

| Version | Build | Notes |
|---------|-------|-------|
| 2.13.0-hubfx | 308 | Board-wide undervoltage detector via AlertService.tickVoltage(); configurable via /alerts.yaml oltage_alert: block |
| 2.12.0-hubfx | 296 | ESP-IDF native SD + LittleFS storage (NativeFile RAII, POSIX VFS); renamed init boot sound to init |