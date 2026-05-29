# tests/

Four parallel test trees, each with a single purpose:

```
tests/
├── host/              Host-runnable automated tests + integration drivers
│   ├── go_unit/             Fast Go unit tests (pure logic; `go test ./...`)
│   ├── go_integration/      Go tests that drive REAL HubFX hardware
│   └── ports/               Shared helpers (port discovery, fixtures)
├── native/            C++ host-compilable unit tests for controllers/lib/*
│                      (PlatformIO `[env:native]` + doctest)
├── hw/                Firmware test sketches — flashed to a real board
└── fixtures/          Static test data (upload payloads, SD images)
```

## host/go_unit/ — fast Go unit tests

Pure-logic Go tests that build and run on the dev machine with **no
hardware required**. Each subdirectory is its own Go module with
`replace scalefx => ../../../../app/go` so it builds against the
in-tree SDK without publishing it. Run `go test ./...` from inside any
directory, or `make go-unit` from the repo root to run all of them.

| Directory | Purpose |
|-----------|---------|
| `protocol_test/` | COBS encoding, CRC-8, packet framing — regression nets for the wire format |
| `client_test/`   | (planned) Per-board command builders + ACK/NACK parsing |
| `config_yaml_test/` | (planned) YAML round-trip for `/hubfx.yaml`, `/enginefx.yaml`, … |
| `devicemodel_test/` | (planned) Port / role label resolution + capability bits |

The default "Run Go Unit Tests" VS Code task runs every `go_unit/*`
suite in turn.

## host/go_integration/ — integration tests that need real hardware

Tests that talk to a connected HubFX over USB serial via the
`scalefx/client` package. They send real commands and assert on the
ACK / NACK / async response, exactly the way Studio and the CLI do.

**Port discovery** (per `tests/host/ports/`):

1. If `SCALEFX_HUBFX_PORT` is set, use it verbatim (`COM14`,
   `/dev/ttyUSB0`, …).
2. Otherwise scan for the first device with VID:PID `1A86:55D3` (CH343
   USB-UART bridge populated on HubFX).
3. If neither resolves, the test calls `t.Skip("no HubFX detected")`
   so CI without hardware exits clean.

Every integration test ALSO honours `testing.Short()` — `go test
-short` skips the whole tree.

| Directory | Purpose |
|-----------|---------|
| `upload_test/`    | (planned) sync + stream uploads, SD + flash targets, MD5 verification |
| `audio_test/`     | (planned) play / stop / status, AUDIO_STATUS_RESP observability |
| `config_test/`    | (planned) save → reload → readback round-trip |
| `storage_test/`   | (planned) sd-init, file.list, mkdir, file.info |
| `topology_test/`  | (planned) port enumeration, role attach via `/hubfx.yaml` |
| `lifecycle_test/` | (planned) IDENTIFY, INIT, STATUS, RESET |

## native/ — C++ host-compilable unit tests for lib/*

Pure-logic tests for `controllers/lib/*` that compile to a host binary
(no ESP32 / RP2040 needed). Uses PlatformIO's `[env:native]` target
plus the **doctest** single-header framework.

Run with `pio test -e native` from `tests/native/`.

| Test | Subsystem | What it locks in |
|------|-----------|------------------|
| `wire_test/`              | `sfx_platform/platform/sfx_wire.cpp` | CRC-8 poly 0x07, COBS encode/decode, putU16LE/getU32LE |
| `packet_test/`            | `sfx_serial/serial/` | Packet framing assemble/parse round-trip |
| `config_test/`            | `sfx_config/` | YAML parser on canonical examples from the lib README |
| `motion_profile_test/`    | `sfx_board/motion/motion_profile.h` | Trapezoidal / S-curve integrator boundary cases |
| `element_scaling_test/`   | `sfx_board/element/element_scaling.h` | `scaleDuty()` for sub-rail elements |
| `port_descriptor_test/`   | `sfx_core::ports::list` | `with_voltage_mV` / `with_vSense_array` chain |

## hw/ — physical-board test firmware

Each subdirectory is a standalone PlatformIO project. Flash one to a
freshly-populated PCB to verify wiring before running the real
firmware. They deliberately bypass the ScaleFX protocol so a multimeter
+ a servo + a logic analyzer is enough to validate a board.

| Directory | Target | What it does |
|-----------|--------|--------------|
| `gearcontrol_hwtest/`    | RP2040    | Sweeps every servo + motor channel, captures gear-input PWM |
| `gunfx_hwtest/`          | RP2040    | Servo sweep, smoke heater + fan, RC trigger, INA226 readout |
| `led_blink/`             | ESP32-S3  | Drives all 8 LED channels via the PCAL6416A I²C expander |
| `noop_simple/`           | ESP32-S3  | Minimal no-op image; useful when bisecting framework issues |
| `ppm_test/`              | ESP32-S3  | PPM signal decoder bench |
| `hubfx_av_hwtest/`       | ESP32-S3  | HubFX audio + video (LED) combined bring-up |
| `hubfx_i2c_scan/`        | ESP32-S3  | I²C bus scan + INA226 / PCA9685 / TAS5825x probe |
| `hubfx_led_hwtest/`      | ESP32-S3  | HubFX 6-channel LED via AW9523B in LED-mode PWM (legacy rev) |
| `pca9685_hwtest/`        | ESP32-S3  | PCA9685 16-channel 12-bit PWM bring-up |
| `hubfx_pca9685_hwtest/`  | ESP32-S3  | HubFX 8-channel LED via PCA9685 @ 0x70 (current rev) |
| `page_cache_test/`       | ESP32-S3  | PSRAM page cache for SD-backed byte streams |
| `sfx_test_p/`            | ESP32-S3  | TAS5825**P** sine bring-up (original HubFX silicon) |
| `sfx_test_m/`            | ESP32-S3  | TAS5825**M** sine bring-up (current HubFX silicon) |
| `storage_test/`          | ESP32-S3  | Upload throughput + hang-recovery harness |

> **Audio codec variants.** TI's TAS5825P and TAS5825M are pin-compatible (same QFN-32 RHB package) but enforce different init flows. The P silicon is permissive about clock-detect timing and has no smart-amp / IV-sense; the M latches CDET out of reset, requires FS_MON to report a valid sample-rate code before HIZ → PLAY, and reserves the GPIO1_SEL bit pattern the P uses for Hybrid-Pro feedback.

Build/flash one with `pio run -t upload` from inside the directory, or
use the parameterised "Build and Flash Firmware" VS Code task with the
`tests/hw/<name>` controller.

## fixtures/ — test data

| Directory | Used by |
|-----------|---------|
| `upload_fixtures/` | `tests/host/go_integration/upload_test/` |

Generated artifacts are gitignored; helpers in the directory rebuild
them on demand.

## Adding a new test

| Kind | Where it goes |
|------|---------------|
| Pure-logic Go test (no HW) | `tests/host/go_unit/<name>/` (own go.mod, `replace scalefx => ../../../../app/go`) |
| Go test that drives real HW | `tests/host/go_integration/<name>/` (same go.mod recipe, plus `testing.Short()` + port-discovery guard) |
| Pure-logic C++ test for lib/* | `tests/native/<name>/` (doctest source, registered in `tests/native/platformio.ini` env) |
| Firmware that runs on a real board | `tests/hw/<name>/` (PlatformIO project) |
| Static test data | `tests/fixtures/<name>/` |

**Rule 21** (test placement) and **Rule 51** (tests must build cleanly;
refactors carry their tests with them) — see
[.github/copilot-instructions.md](../.github/copilot-instructions.md).

Production code (`app/go/`, `controllers/`) MUST NOT import from
`tests/`.

## Removed in this cleanup (2026-05-28)

Four host directories were stale since the protocol / engine refactor
that landed pre-IDF-migration; they referenced packages
(`scalefx/api`, `scalefx/engine`, `scalefx/protocol/hubfx`) that no
longer exist on the live tree. Deleted rather than carried as dead
weight — the replacement test architecture above captures the
intent:

- `host/handler_test/` (5 files) — engine command decoders. New
  equivalents land in `go_unit/client_test/`.
- `host/upload_test/` (1 file) — sync upload regression nets. New
  equivalent in `go_integration/upload_test/`.
- `host/storage_test_client/` (3 files + checked-in `.exe`) — a
  one-off throughput driver. Replaced by `go_integration/upload_test/`.
- `host/usb_diag/` (3 files) — one-off USB host diagnostic tool.

See **Rule 51** for why this stays a single-commit obligation going
forward.
