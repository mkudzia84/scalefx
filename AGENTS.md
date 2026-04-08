# ScaleFX AI Agent Context

> **All development rules are in `.github/copilot-instructions.md`** (auto-loaded by VS Code Copilot).
> **Workflow guides are in `/instructions/`** — see the index below.

## Project Overview

ScaleFX is a modular scale model effects system for RC helicopters:
- **Architecture:** Client-server over USB serial (binary COBS protocol, CRC-8, 6Mbps)
- **Pico Controllers** (RP2040): GunFX (weapons), LightFX (lighting), GearControl (landing gear)
- **ESP32-S3 Controller**: HubFX ESP32-S3 (master hub, active development)
- **HubFX Pico** (RP2350): OBSOLETE — frozen reference implementation, do not modify
- **Go CLI** (`app/go/`): Compiled interactive CLI — 3-package architecture: `protocol/` (wire format, packets, commands, connection), `api/` (typed client SDK), `cli/` (interactive terminal UI)
- **Flash CLI** (`app/go/flash/`): Standalone build/flash/upload tool for all controllers

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
├── lib/                 # Shared PlatformIO libraries (8 independent modules)
│   ├── sfx_platform/    # Cross-platform abstraction (mutexes, delays, GPIO, diag_log)
│   ├── sfx_serial/      # COBS protocol, command routing, bus server/client, per-board handlers
│   ├── sfx_server/      # SfxServer common controller boilerplate
│   ├── sfx_peripherals/ # Hardware drivers (LED, servo, PWM input, I2C, INA226)
│   ├── sfx_audio/       # 8-channel WAV mixer, I2S output, codec drivers, ring buffer
│   ├── sfx_storage/     # SD card (SdFat/ESP SD), LittleFS flash, StorageServerT policy template
│   ├── sfx_config/      # YAML config parser, schema-driven config store, protocol server/client
│   └── sfx_usb/         # USB Host abstraction (PicoUsbHost, EspUsbHost)
├── gunfx/pico/          # Gun effects (RP2040 server)
├── lightfx/pico/        # Lighting effects (RP2040 server)
├── gearcontrol/pico/    # Landing gear (RP2040 server)
├── hubfx/esp32s3/       # Master hub (ESP32-S3, active development target)
└── noop/pico/           # Protocol test stub

app/go/                      # Go CLI (3 packages: protocol, api, cli) + Flash CLI + Studio
```

## Mandatory File Sync (C++ ↔ Go)

| C++ File | Go CLI File |
|----------|-------------|
| `core/core.h` | `packets.go` |
| `core/stream.h` | `packets.go` |
| `gunfx/gunfx.h` | `packets.go`, `commands.go`, `handler_gunfx.go` |
| `lightfx/lightfx.h` | `packets.go`, `commands.go`, `handler_lightfx.go` |
| `gearcontrol/gearcontrol.h` | `packets.go`, `commands.go`, `handler_gearcontrol.go` |
| `hubfx/hubfx.h` | `packets.go`, `commands.go`, `handler_hubfx.go` |
