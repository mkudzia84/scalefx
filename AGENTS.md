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
- Controllers: GunFX (weapons), LightFX (lighting), HubFX (client hub)
- Python test framework with interactive CLI

## Critical Constants

- **Baud rate:** 115200
- **Packet format:** `[type:u8][len:u8][payload:0-64][crc8:u8]`
- **CRC-8 polynomial:** 0x07
- **Endianness:** Little-endian

## Packet Type Ranges

- `0x01-0x2F`: GunFX (used)
- `0x40-0x5F`: LightFX (used)
- `0x60-0xEF`: Available for new controllers
- `0xF0-0xFF`: Core system commands (reserved)

## File Structure

```
scripts/
└── build_and_flash.py   # Centralized build/flash (binary COBS protocol)

controllers/
├── lib/serial/          # Shared serial library (C++)
├── gunfx/pico/          # Gun effects controller
├── lightfx/pico/        # Lighting controller
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
│       └── lightfx.py   # LightFX commands
├── gunfx/               # GunFX tests
├── lightfx/             # LightFX tests
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

## Change Workflow

1. Read relevant `/instructions/` document
2. Make changes following the checklist
3. Verify C++ compiles: `pio run`
4. Verify Python syntax: `python -m py_compile <file>`
5. Flash and test: `python scripts/build_and_flash.py <controller>`
6. Run tests: `pytest tests/<module>/ -v`
