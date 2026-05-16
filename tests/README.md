# tests/

Four parallel test trees, each with a single purpose:

```
tests/
├── hw/             Firmware test fixtures — flashed to a real board
├── host/           Go programs that run on the dev machine
├── virtual_board/  Go simulator that fakes any of the four boards over TCP
└── fixtures/       Test data (upload payloads, SD images)
```

## hw/ — physical-board test firmware

Each subdirectory is a standalone PlatformIO project. Flash one to a
freshly-populated PCB to verify wiring before running the real
firmware. They deliberately bypass the ScaleFX protocol so a multimeter
+ a servo + a logic analyzer is enough to validate a board.

| Directory | Target | What it does |
|-----------|--------|--------------|
| `gearcontrol_hwtest/` | RP2040 | Sweeps every servo + motor channel, captures gear-input PWM |
| `gunfx_hwtest/`        | RP2040 | Servo sweep, smoke heater + fan, RC trigger, INA226 readout |
| `led_blink/`           | ESP32-S3 | Drives all 8 LED channels via the PCAL6416A I²C expander |
| `noop_simple/`         | ESP32-S3 | Minimal no-op image; useful when bisecting framework issues |
| `ppm_test/`            | ESP32-S3 | PPM signal decoder bench |
| `hubfx_led_hwtest/`    | ESP32-S3 | HubFX local 6-channel LED bring-up via AW9523B in LED-mode hardware PWM (430 Hz, 8-bit; inverted polarity for pull-up + N-MOSFET topology). Line-based brightness control + `[PASS]`/`[FAIL]` markers for agent grep |
| `pca9685_hwtest/`      | ESP32-S3 | PCA9685 16-channel 12-bit hardware-PWM bring-up (push-pull voltage drive, configurable 24–1526 Hz). Same line-based UX as `hubfx_led_hwtest/` for A/B comparison; default I²C address 0x40 |
| `hubfx_pca9685_hwtest/` | ESP32-S3 | HubFX 8-channel LED bring-up via PCA9685 @ 0x70 (current HubFX rev with TAS5825P). Automatic gamma-corrected sine breathing across all 8 LED rails, all channels updated atomically via the `ALL_LED` broadcast registers so the fade stays in sync. See [controllers/hubfx/esp32s3/PINOUT.md](../../controllers/hubfx/esp32s3/PINOUT.md) for the underlying pin/address map |
| `sfx_test_p/`          | ESP32-S3 | TAS5825**P** sine-wave bring-up (Class-H + Hybrid-Pro silicon — original HubFX board) |
| `sfx_test_m/`          | ESP32-S3 | TAS5825**M** sine-wave bring-up (smart-amp + FS_MON-gated init — current HubFX board) |
| `storage_test/`        | ESP32-S3 | Upload throughput + hang-recovery harness for SD/flash |

> **Audio codec variants.** TI's TAS5825P and TAS5825M are pin-compatible (same QFN-32 RHB package) but enforce different init flows. The P silicon is permissive about clock-detect timing and has no smart-amp / IV-sense; the M latches CDET out of reset, requires FS_MON to report a valid sample-rate code before HIZ → PLAY, and reserves the GPIO1_SEL bit pattern the P uses for Hybrid-Pro feedback. Use `sfx_test_p/` for the original HubFX board (TAS5825P silicon) and `sfx_test_m/` for boards populated with TAS5825M.

Build/flash one with `pio run -t upload` from inside the directory, or
use the parameterised "Build and Flash Firmware" VS Code task with the
`tests/hw/<name>` controller.

## host/ — Go programs that run on the dev machine

Unit tests, integration drivers, and diagnostic tools. Each is its own
Go module with `replace scalefx => ../../../app/go` so it builds
against the in-tree SDK without publishing it. Run `go test ./...` from
inside any directory.

| Directory | Type | Purpose |
|-----------|------|---------|
| `handler_test/`        | Unit tests | Engine command handlers + observer wiring |
| `protocol_test/`       | Unit tests | COBS encoding, CRC-8, packet framing |
| `upload_test/`         | Integration | Drives real HW upload from desktop |
| `storage_test_client/` | Tool | Throughput driver for `tests/hw/storage_test/` |
| `usb_diag/`            | Tool | USB host + slave detection diagnostics |

The default "Run Go Unit Tests" VS Code task runs in
`tests/host/handler_test/`.

## virtual_board/ — Go simulator

A long-running emulator that fakes any of the four boards (LightFX,
GearControl, GunFX, HubFX) over a TCP socket. Used to:

1. Drive ScaleFX Studio's UI without flashing a controller
2. Reproduce wire-format bugs against a deterministic device model
3. Run the merged event-timing + light-program-runtime tests
   (`go test ./boards/lightfx/`)

See [`virtual_board/README.md`](virtual_board/README.md) for the full
list of supported commands per board, the discovery protocol that
makes virtual boards appear in Studio's Connect dialog and
`scalefx-cli ports`, and the in-process testing recipe.

## fixtures/ — test data

| Directory | Used by |
|-----------|---------|
| `upload_fixtures/` | `tests/host/upload_test/`, `tests/host/storage_test_client/` |

Generated artifacts are gitignored; helpers in the directory rebuild
them on demand.

## Adding a new test

| Kind | Where it goes |
|------|---------------|
| Firmware that runs on a real board | `tests/hw/<name>/` (PlatformIO project) |
| Go test or tool for the desktop | `tests/host/<name>/` (own go.mod, `replace scalefx => ../../../app/go`) |
| Test that drives the protocol without real HW | `tests/virtual_board/boards/<kind>/<kind>_test.go` |
| Static test data | `tests/fixtures/<name>/` |

**Rule 21:** tests live here, not under `controllers/*/test/` or
`app/go/tests/`. Production code (`app/go/`, `controllers/`) MUST NOT
import from `tests/`.
