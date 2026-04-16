# ScaleFX AI Agent Context

> **All development rules are in `.github/copilot-instructions.md`** (auto-loaded by VS Code Copilot).
> **Workflow guides are in `/instructions/`** — see the index below.

## Project Overview

ScaleFX is a modular scale model effects system for RC helicopters:
- **Architecture:** Client-server over USB serial (binary COBS protocol, CRC-8, 6Mbps)
- **Pico Controllers** (RP2040): GunFX (weapons), LightFX (lighting), GearControl (landing gear)
- **ESP32-S3 Controller**: HubFX ESP32-S3 (master hub, active development)
- **HubFX Pico** (RP2350): OBSOLETE — frozen reference implementation, do not modify
- **Go CLI** (`app/go/`): Compiled interactive CLI — 5-package architecture: `protocol/` (wire format, per-module subpackages), `api/` (typed client SDK), `engine/` (shared command engine + handlers), `cli/` (thin terminal wrapper), `flash/` (standalone build/flash tool)
- **ScaleFX Studio** (`app/go/studio/`): Wails v2 GUI — embeds the same engine as the CLI

## Critical Constants

- **Packet format:** `[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]`
- **CRC-8 polynomial:** 0x07 | **Baud rate:** 6Mbps | **Endianness:** Little-endian
- **Connection timeout:** 15000ms | **Indicator LEDs:** GP13/GP14 (Pico), GP48 (ESP32-S3)

## Instruction Documents

| Task | Document |
|------|----------|
| System design, packet format, class hierarchy | `instructions/01-ARCHITECTURE.md` |
| Create new controller | `instructions/02-NEW-CONTROLLER.md` |
| Add commands to existing controller | `instructions/03-PROTOCOL-EXTENSION.md` |
| File sync checklist after changes | `instructions/04-CHANGE-PROPAGATION.md` |
| Build and deploy firmware | `instructions/05-BUILD-AND-FLASH.md` |
| Update interactive CLI | `instructions/07-CLI-UPDATES.md` |
| AudioTools library reference (HubFX audio) | `instructions/08-AUDIOTOOLS.md` |
| Console output schema (all CLIs) | `instructions/09-CONSOLE-OUTPUT.md` |

## Quick Commands

```bash
# Build firmware (via Flash CLI)
app/go/scalefx-flash.exe build hubfx --no-clean
app/go/scalefx-flash.exe build gunfx --no-clean

# Build and flash with verification
app/go/scalefx-flash.exe flash hubfx --no-clean
app/go/scalefx-flash.exe flash gunfx --no-clean

# Build Go CLI
cd app/go && go build -o scalefx-cli.exe ./cli/

# Build Flash CLI
cd app/go && go build -o scalefx-flash.exe ./flash/

# Interactive CLI (Go)
app/go/scalefx-cli.exe -p COM5
```

## File Structure

```
controllers/
├── lib/                 # Shared PlatformIO libraries (10 modules)
│   ├── sfx_platform/    # Cross-platform abstraction (mutexes, delays, GPIO, diag_log)
│   ├── sfx_serial/      # COBS protocol, command routing, bus server/client, per-board handlers
│   ├── sfx_server/      # SfxServer common controller boilerplate
│   ├── sfx_peripherals/ # Hardware drivers (LED, servo, PWM input, I2C, INA226)
│   ├── sfx_audio/       # 8-channel WAV mixer, I2S output, codec drivers, ring buffer
│   ├── sfx_storage/     # SD card (SdFat/ESP SD), LittleFS flash, StorageServerT policy template
│   ├── sfx_config/      # YAML config parser, schema-driven config store, protocol server/client
│   ├── sfx_usb/         # USB Host abstraction (PicoUsbHost, EspUsbHost)
│   ├── sfx_boards/      # Board-specific client/server protocol implementations (GunFX, LightFX, GearControl)
│   └── esp_cdc_acm/     # Vendored ESP-IDF USB Host CDC-ACM class driver (ESP32-S3 only)
├── gunfx/pico/          # Gun effects (RP2040 server)
├── lightfx/pico/        # Lighting effects (RP2040 server)
├── gearcontrol/pico/    # Landing gear (RP2040 server)
├── hubfx/esp32s3/       # Master hub (ESP32-S3, active development target)
└── noop/                # Protocol test stubs
    ├── pico/            # NoOp for RP2040
    └── esp32s3/         # NoOp for ESP32-S3

tests/                       # Standalone tests and diagnostic tools
├── led_blink/           # [Firmware] PCAL6416A I2C GPIO expander — blink all 8 LED channels
├── noop_simple/         # [Firmware] Minimal no-op test
├── ppm_test/            # [Firmware] PPM signal decoder test
├── sfx_test/            # [Firmware] SFX library integration test
└── usb_diag/            # [Go tool] USB host & slave detection diagnostics

app/go/                      # Go CLI (5 packages) + Flash CLI + Studio
```

## Mandatory File Sync (C++ ↔ Go)

| C++ File | Go CLI File |
|----------|-------------|
| `core/core.h` | `protocol/core/core.go` |
| `core/stream.h` | `protocol/stream.go` |
| `gunfx/gunfx.h` | `protocol/gunfx/gunfx.go`, `engine/handlers/gunfx/handler.go` |
| `lightfx/lightfx.h` | `protocol/lightfx/lightfx.go`, `engine/handlers/lightfx/handler.go` |
| `gearcontrol/gearcontrol.h` | `protocol/gearcontrol/gearcontrol.go`, `engine/handlers/gearcontrol/handler.go` |
| `hubfx/hubfx.h` | `protocol/hubfx/hubfx.go`, `engine/handlers/hubfx/handler.go` |
