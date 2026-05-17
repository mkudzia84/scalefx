# HubFX ESP32-S3 — Empty Entry Point

Fresh restart of the HubFX firmware on top of the consolidated board
framework. One firmware-level object — `sfx_core::BoardServer<...UserPolicies>`
from [sfx_board](../../lib/sfx_board/) — composes the lifecycle service,
the indicator-LED runtime, and every user policy into a single
`board.process()` loop.

The previous implementation is archived at
[controllers/archive/hubfx-esp32s3/](../../archive/hubfx-esp32s3/) as
reference. Hardware reference: [PINOUT.md](PINOUT.md).

## Subsystem status

| Subsystem | Status |
| --------- | ------ |
| Core protocol (`BoardServicePolicy`: INIT / STATUS / KEEPALIVE / IDENTIFY / I2C_SCAN / DIAG_HISTORY) | wired |
| Indicator LEDs (`IndicatorServicePolicy`) | wired (onboard GPIO48 only) |
| DiagLog over UART | wired |
| Audio mixer + I²S output | not migrated |
| Storage (LittleFS + SD) | not migrated |
| Config store (YAML) | not migrated |
| USB Host (generic-expander routing) | not migrated |
| Engine FX | not migrated |
| Local LED runtime | not migrated |
| Battery monitoring | not migrated |
| RC inputs (PWM / PPM / SBUS / Jeti EX) | not migrated |

As HubFX-specific subsystems land in `src/`, the corresponding policies
get added to the `BoardServer<...>` template parameters in
[`hubfx_esp32s3.ino`](src/hubfx_esp32s3.ino):

```cpp
using HubFxBoard = sfx_core::BoardServer<
    AudioServicePolicy<Mixer>,
    StorageServicePolicy<Esp32StoragePolicy>,
    BatteryServicePolicy<Ina226Battery>,
    UsbHostServicePolicy,
    EngineServicePolicy,
    ConfigServicePolicy>;
```

`BoardServicePolicy` and `IndicatorServicePolicy` are prepended
automatically — they're not part of the user-policy pack.

## Build

```bash
app/go/scalefx-flash.exe build hubfx
app/go/scalefx-flash.exe flash hubfx --no-clean
```
