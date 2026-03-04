# ScaleFX AI Agent Context

This file provides essential context for AI coding assistants working on the ScaleFX project.

## Instruction Documents

**IMPORTANT:** Before making changes, read the relevant document from `/instructions/`:

| Document | Purpose |
|----------|---------|
| `instructions/README.md` | Index, navigation, constants |
| `instructions/01-ARCHITECTURE.md` | System design, packet format |
| `instructions/02-NEW-CONTROLLER.md` | Create new controller |
| `instructions/03-PROTOCOL-EXTENSION.md` | Add commands |
| `instructions/04-CHANGE-PROPAGATION.md` | File sync checklist |
| `instructions/05-BUILD-AND-FLASH.md` | Build and deploy |
| `instructions/06-TEST-SUITE.md` | Run/write tests |
| `instructions/07-CLI-UPDATES.md` | Update CLI |

## Project Overview

ScaleFX is a modular scale model effects system:
- Client-server architecture over USB serial
- Binary COBS protocol with CRC-8 validation
- All Pico server controllers use `PicoServer` component for common boilerplate
- Controllers: GunFX (weapons), LightFX (lighting), GearControl (landing gear), HubFX (client hub)
- Python test framework with interactive CLI

## Critical Constants

- **Baud rate:** 115200
- **Packet format:** `[type:u8][len:u8][payload:0-64][crc8:u8]`
- **CRC-8 polynomial:** 0x07
- **Endianness:** Little-endian
- **Connection timeout:** 15000ms (all controllers)
- **Indicator LEDs:** GP13 (connection), GP14 (error) — standardized across all Pico controllers

## Packet Type Ranges

- `0x01-0x2F`: GunFX (used)
- `0x40-0x5F`: LightFX (used)
- `0x60-0x7F`: GearControl (used)
- `0x80-0xEF`: Available for new controllers
- `0xF0-0xFF`: Core system commands (reserved)

## Error Code Ranges

- `0x00-0x0F`: Generic errors (OK, UNKNOWN, INVALID_COMMAND, etc.)
- `0x10-0x1F`: Parameter validation (INVALID_PARAM, OUT_OF_RANGE, etc.)
- `0x20-0x4F`: GunFX-specific (SERVO_*, SMOKE_*, TRIGGER_*)
- `0x50-0x5F`: LightFX-specific (LED_*, SERVO_*)
- `0x60-0x6F`: GearControl-specific (GEAR_*, MOTOR_*, SERVO_*, YAW_*)
- `0x70-0x8F`: Reserved for future modules
- `0xF0-0xFF`: System/transport (INTERNAL, TIMEOUT, CRC_ERROR, etc.)

## File Structure

```
scripts/
└── build_and_flash.py   # Centralized build/flash (binary COBS protocol)

controllers/
├── lib/components/      # Reusable hardware drivers (LED, PWM, servo, I2C, PicoServer)
├── lib/serial/          # Shared serial library (C++)
├── gunfx/pico/          # Gun effects controller
├── lightfx/pico/        # Lighting controller
├── gearcontrol/pico/    # Landing gear controller
├── hubfx/               # Master hub
└── noop/pico/           # Protocol test stub

tests/
├── framework/           # Python test framework
├── cli/                 # Interactive CLI (modular)
│   ├── base.py          # Base classes, CommandInfo, OutputMixin
│   ├── parsers.py       # Response packet parsing
│   ├── interactive.py   # Main CLI (~280 lines)
│   └── handlers/        # Command handlers
│       ├── core.py      # Core/protocol commands
│       ├── gunfx.py     # GunFX commands
│       ├── lightfx.py   # LightFX commands
│       └── gearcontrol.py # GearControl commands
├── gunfx/               # GunFX tests
├── lightfx/             # LightFX tests
├── gearcontrol/         # GearControl tests
└── noop/                # NoOp tests
```

## Mandatory File Sync

When modifying protocol, these file pairs MUST stay in sync:

| C++ File | Python File |
|----------|-------------|
| `serial_core.h` | `packets.py` |
| `serial_error.h` | `packets.py` |
| `serial_gunfx.h` | `commands.py`, `cli/handlers/gunfx.py` |
| `serial_lightfx.h` | `commands.py`, `cli/handlers/lightfx.py` |
| `serial_gearcontrol.h` | `commands.py`, `cli/handlers/gearcontrol.py` |

## Change Workflow

1. Read relevant `/instructions/` document
2. Make changes following the checklist
3. Verify C++ compiles: `pio run`
4. Verify Python syntax: `python -m py_compile <file>`
5. Flash and test: `python scripts/build_and_flash.py <controller>`
6. Run tests: `pytest tests/<module>/ -v`
