# ScaleFX AI Agent Context

> **All development rules are in `.github/copilot-instructions.md`** (auto-loaded by VS Code Copilot).
> **Workflow guides are in `/instructions/`** — see the index below.

## Project Overview

ScaleFX is a modular scale model effects system for RC helicopters:
- **Architecture:** Client-server over USB serial (binary COBS protocol, CRC-8, 6Mbps)
- **Pico Controllers** (RP2040): GunFX (weapons), LightFX (lighting), GearControl (landing gear)
- **ESP32-S3 Controller**: HubFX ESP32-S3 (master hub, active development)
- **HubFX Pico** (RP2350): OBSOLETE — frozen reference implementation, do not modify
- **Windows Studio** (.NET 8/C#): Visual configuration editor
- **Python test framework** with interactive CLI

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
| Run/write tests | `instructions/06-TEST-SUITE.md` |
| Update interactive CLI | `instructions/07-CLI-UPDATES.md` |
| AudioTools library reference (HubFX audio) | `instructions/08-AUDIOTOOLS.md` |

## Quick Commands

```bash
# Build Pico firmware
python -m platformio run -e pico -d controllers/{gunfx|lightfx|gearcontrol|noop}/pico

# Build ESP32-S3 firmware (HubFX)
python -m platformio run -e esp32s3 -d controllers/hubfx/esp32s3

# Build and flash
python scripts/build_and_flash.py {gunfx|lightfx|gearcontrol|noop}
python scripts/build_and_flash.py hubfx  # ESP32-S3 (uses esptool)

# Python syntax check
python -m py_compile tests/framework/packets.py

# Interactive CLI
python -m tests.cli.interactive --port COM5

# Run tests (requires hardware)
pytest tests/{gunfx|lightfx|gearcontrol|noop}/ -v
```

## File Structure

```
controllers/
├── lib/                 # Shared PlatformIO libraries (7 independent modules)
│   ├── sfx_platform/    # Cross-platform abstraction (mutexes, delays, GPIO, diag_log)
│   ├── sfx_serial/      # COBS protocol, command routing, bus server/client, per-board handlers
│   ├── sfx_server/      # SfxServer common controller boilerplate
│   ├── sfx_peripherals/ # Hardware drivers (LED, servo, PWM input, I2C, INA226)
│   ├── sfx_audio/       # 8-channel WAV mixer, I2S output, codec drivers, ring buffer
│   ├── sfx_storage/     # SD card (SdFat/ESP SD), LittleFS flash singletons
│   └── sfx_usb/         # USB Host abstraction (PicoUsbHost, EspUsbHost)
├── gunfx/pico/          # Gun effects (RP2040 server)
├── lightfx/pico/        # Lighting effects (RP2040 server)
├── gearcontrol/pico/    # Landing gear (RP2040 server)
├── hubfx/pico/          # Master hub (RP2350 client, dual-core) — OBSOLETE, reference only
├── hubfx/esp32s3/       # Master hub (ESP32-S3, active development target)

└── noop/pico/           # Protocol test stub

tests/
├── framework/           # packets.py, commands.py, connection.py, protocol.py
├── cli/handlers/        # CLI command handlers (composition-based slave routing via hubfx.py)
└── {gunfx,lightfx,gearcontrol,noop}/  # pytest test suites

scripts/build_and_flash.py   # Centralized build/flash
app/win32/ScaleFXStudio/     # Windows config editor (.NET 8)
```

## Mandatory File Sync (C++ ↔ Python)

| C++ File | Python File |
|----------|-------------|
| `core/core.h` | `packets.py` |
| `core/stream.h` | `packets.py` (StreamPacket) |
| `gunfx/gunfx.h` | `packets.py`, `commands.py`, `cli/handlers/gunfx.py` |
| `lightfx/lightfx.h` | `packets.py`, `commands.py`, `cli/handlers/lightfx.py` |
| `gearcontrol/gearcontrol.h` | `packets.py`, `commands.py`, `cli/handlers/gearcontrol.py` |
| `hubfx/hubfx.h` | `packets.py`, `commands.py`, `cli/handlers/hubfx.py` |
