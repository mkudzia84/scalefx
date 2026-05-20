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
