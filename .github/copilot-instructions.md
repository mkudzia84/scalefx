# ScaleFX Development Guide

> **AI AGENTS:** This is a multi-platform embedded effects system. Read `/instructions/README.md` first for comprehensive guidance.

## System Architecture

ScaleFX is a modular scale model effects system with three platform targets:
- **Pico Controllers** (RP2040): Real-time device control (GunFX, LightFX, HubFX Pico)
- **Raspberry Pi Hub** (Linux/C): Audio mixing and PWM monitoring (HubFX Pi)
- **Windows Studio** (.NET 8/C#): Visual configuration editor

**Communication:** Binary COBS protocol over USB serial (115200 baud)
- Packet format: `[type:u8][len:u8][payload:0-64][crc8:u8]`
- CRC-8 polynomial: 0x07
- Endianness: Little-endian for ALL multi-byte values

## Quick Command Reference

```bash
# Build Pico firmware (PlatformIO)
cd controllers/{gunfx|lightfx|hubfx}/pico
python -m platformio run -e pico

# Build and flash with verification (centralized script)
python scripts/build_and_flash.py gunfx
python scripts/build_and_flash.py lightfx --port COM10
python scripts/build_and_flash.py noop --no-build
python scripts/build_and_flash.py gunfx --no-clean      # incremental build
python scripts/build_and_flash.py lightfx --skip-verify  # skip post-flash check

# Build Raspberry Pi hub (C, requires GCC 14+)
ssh helifx@helifx "cd ~/helifx/controllers/hubfx/pi && make"

# Build Windows Studio (.NET 8)
cd app/win32/ScaleFXStudio
dotnet build -c Release

# Run Python tests (requires hardware)
pytest tests/gunfx/ -v

# Interactive CLI
python -m tests.cli.interactive --port COM5
```

## Critical Development Rules

### 0. Documentation Is Code (MANDATORY)

**Always update documentation alongside code changes.** Documentation is part of the deliverable, not an afterthought.

When making ANY code change:
1. Update relevant README.md files (controller, library, or module level)
2. Update `/instructions/` documents if architecture or workflows change
3. Update this file (`copilot-instructions.md`) if development patterns change
4. Update `AGENTS.md` if project overview changes

Documentation updates should be included in the same commit/session as code changes.

### 1. Protocol Constant Synchronization (MANDATORY)

When modifying serial protocol, these files MUST stay in sync:

| C++ Source | Python Mirror | Content |
|------------|---------------|---------|
| `controllers/lib/serial/serial_core.h` | `tests/framework/packets.py` | Packet type constants |
| `controllers/lib/serial/serial_error.h` | `tests/framework/packets.py` | Error codes |
| `controllers/lib/serial/serial_gunfx.h` | `tests/framework/commands.py` | GunFX commands |
| `controllers/lib/serial/serial_lightfx.h` | `tests/framework/commands.py` | LightFX commands |

**Verification:** Run `python -m py_compile tests/framework/packets.py` after C++ changes.

### 2. Command Addition Checklist

When adding a new command to an existing controller, update ALL these files:
1. `serial_core.h` - Add packet type constant in namespace (e.g., `GunFxPacket::NEW_CMD = 0xNN`)
2. `serial_xxxfx.h` - Add callback typedef, `onNewCmd()` method, handler case using `SFX_*` macros
3. `xxxfx_pico.ino` - Implement callback, register in `setup()`
4. `tests/framework/packets.py` - Mirror constant in `XxxPacket` class
5. `tests/framework/commands.py` - Add static builder method in `XxxCommands`
6. `tests/xxxfx/test_<feature>.py` - **REQUIRED:** Add tests for new functionality
7. `tests/cli/handlers/xxxfx.py` - Add command to handler class
8. `controllers/xxxfx/pico/README.md` - Document payload format

**ALWAYS update tests when protocol is changed or new features are added.** Tests are not optional.

**ALWAYS update the CLI when new commands are added.** The CLI is the primary debugging tool.

See `/instructions/03-PROTOCOL-EXTENSION.md` and `/instructions/04-CHANGE-PROPAGATION.md` for details.

### 3. Endianness Pattern (CRITICAL)

**C++ payload parsing:**
```cpp
uint16_t value = payload[0] | (payload[1] << 8);  // Little-endian
```

**Python payload building:**
```python
payload = struct.pack('<H', value)  # '<' = little-endian
```

### 4. Chain of Responsibility Pattern (Server Controllers)

Pico server firmware uses handler chain for commands:
```cpp
CommandRouter router;
router.addHandler(&coreServer);      // System commands (INIT, REBOOT, etc.)
router.addHandler(&protocolHandler); // GunFxServer or LightFxServer
// In loop(): router.process();
```

Each handler returns `CommandHandleResult::Handled` or `NotMyCommand`.

## Key Architecture Patterns

### Client-Server Topology
```
HubFX (Client) - USB Host with RP2040
  ├─ USB Port 0 → GunFX Pico (Server)
  ├─ USB Port 1 → LightFX Pico (Server)
  └─ USB Port N → Other Servers
```

### Handler Registration (CRITICAL)

Every Pico server firmware **MUST** register `coreServer` with the `commandRouter` **before** the module handler. Without this, core commands (INIT, REBOOT, BOOTSEL, etc.) return `INVALID_COMMAND`.

```cpp
// CORRECT - both handlers registered in priority order
commandRouter.addHandler(&coreServer);      // Priority 1: core/system commands
commandRouter.addHandler(&xxxfxServer);      // Priority 2: module commands

// WRONG - coreServer missing, INIT/REBOOT/BOOTSEL will NACK
commandRouter.addHandler(&xxxfxServer);      // ← Missing coreServer!
```

### Shared Serial Library (`controllers/lib/serial/`)
- **serial.h** - Umbrella include (use this)
- **serial_core.h** - CoreProtocol class, packet types, COBS/CRC, `SFX_*` handler macros, `StatusDataCallback`
- **serial_bus.h** - SerialBus class (client), UsbHost (PIO-USB CDC)
- **serial_command_handler.h** - ICommandHandler interface, CommandRouter
- **serial_gunfx.h** - GunFxClient, GunFxServer, `GunFxSpec` validation namespace
- **serial_lightfx.h** - LightFxClient, LightFxServer, `LightFxSpec` validation namespace

Include order: `#include "serial.h"` (includes everything needed)

### Server Handler Macros (serial_core.h)
Reduce boilerplate in `tryProcess()` switch cases:
```cpp
SFX_REQUIRE_LEN(n)                    // NACK MISSING_PARAMETER if len < n
SFX_VALIDATE(cond, err)               // NACK err if !cond
SFX_DISPATCH(callback, args...)       // Call callback, ACK/NACK on result
SFX_HANDLE_CHANNEL_CMD(v, err, cb)    // Validate + dispatch single-param cmd
```

### Rich STATUS Pattern

Every controller provides board-specific status via `CoreCommandServer`:

```cpp
// In setup(): Register module status callback
coreServer.onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
    buf[0] = myFlag;
    CoreProtocol::putU16LE(&buf[1], myServo);
    return 3;  // bytes written
});

// In loop(): Keep free RAM current
coreServer.updateFreeRam(rp2040.getFreeHeap());
```

STATUS response = 12-byte core header `[counter:u32][uptime:u32][freeRam:u32]` + module callback data.

INIT_READY payload = length-prefixed binary: `[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]`

See `controllers/lib/serial/PROTOCOL.md` for full wire format.

### Python Test Framework (`tests/`)
```
framework/
  ├── connection.py    - ScaleFXConnection class
  ├── protocol.py      - COBS encode/decode, CRC-8, packet helpers
  ├── packets.py       - Constants (MUST mirror C++ headers)
  └── commands.py      - High-level command builders (e.g., GunFxCommands.trigger_on())
cli/
  ├── base.py          - CommandInfo, OutputMixin, ControllerType, base classes
  ├── parsers.py       - Response packet parsing utilities
  ├── interactive.py   - Main CLI class (composes handlers)
  └── handlers/
      ├── core.py      - Core/protocol commands (connect, init, status)
      ├── gunfx.py     - GunFX commands (trigger, servo, smoke)
      └── lightfx.py   - LightFX commands (led, servo, power)
{controller}/
  └── test_*.py        - pytest test files (requires hardware)
```

**Test execution:** Always flash firmware before running tests (`python scripts/build_and_flash.py gunfx`)

## Packet Type Allocation

| Range | Module | Status | Notes |
|-------|--------|--------|-------|
| 0x01-0x2F | GunFX | Used | Trigger, servo, smoke |
| 0x30-0x3F | Reserved | - | Future expansion |
| 0x40-0x5F | LightFX | Used | LED, servo, power |
| 0x60-0xEF | Available | Free | New controllers |
| 0xF0-0xFF | Core | Reserved | INIT, ACK, NACK, REBOOT, etc. |

## Platform-Specific Notes

### Pico Controllers (C++17, Arduino)
- Build with PlatformIO: `python -m platformio run`
- Framework: Arduino-Pico (Earle Philhower core)
- Key libraries: Servo, Wire (I2C), SD (FatFS), USB Host (PIO-USB for HubFX)
- BOOTSEL mode: Send `BOOTSEL` command via serial for firmware updates

### Raspberry Pi Hub (C23, Linux)
- Build with Make: `make` in `controllers/hubfx/pi/`
- Requires GCC 14+ (C23 standard)
- Dependencies: libyaml, libasound2, libsndfile1, pigpio
- Audio: WM8960 Audio HAT (I2C/I2S)
- PWM input: pigpio daemon required (`sudo systemctl start pigpiod`)
- Remote build: Use VS Code task "Build HubFX Pi on Raspberry Pi (Remote SSH)"

### Windows Studio (C# 12, .NET 8)
- Build: `dotnet build -c Release` in `app/win32/ScaleFXStudio/`
- Framework: Windows Forms
- Purpose: Generate `config.yaml` for Raspberry Pi hub
- Partial classes: MainForm split across multiple files (Fields, EngineFxTab, GunFxTab, etc.)

## Common Workflows

### Adding a New Command
1. Read `/instructions/03-PROTOCOL-EXTENSION.md`
2. Choose packet type ID from available range
3. Update C++ serial library (serial_core.h, serial_xxxfx.h) — use `SFX_*` macros
4. Update firmware (xxxfx_pico.ino)
5. Update Python framework (packets.py, commands.py)
6. Update CLI handler (tests/cli/handlers/xxxfx.py)
7. Add test (tests/xxxfx/test_feature.py)
8. Update README.md protocol table
9. Verify: `pio run && python -m py_compile tests/framework/packets.py`

### Creating a New Controller
1. Read `/instructions/02-NEW-CONTROLLER.md`
2. Reserve packet type range (0x60-0xEF available)
3. Create `controllers/lib/serial/serial_newfx.h` (NewFxClient, NewFxServer)
4. Create `controllers/newfx/pico/` directory structure
5. Create Python test framework classes
6. Add CLI commands
7. Document in `README.md`

### Debugging Protocol Issues
- Use interactive CLI: `python -m tests.cli.interactive`
- Check constants match: Compare `serial_core.h` vs `packets.py`
- Verify endianness: All multi-byte values are little-endian
- Check CRC: CRC-8 poly 0x07 over [type][len][payload]
- NACK codes in `serial_error.h` and `packets.py` must match

## Essential Documentation

Comprehensive agent instructions are in `/instructions/`:
- **README.md** - Quick navigation, constants, file index
- **01-ARCHITECTURE.md** - System topology, packet format, class hierarchy
- **02-NEW-CONTROLLER.md** - Step-by-step controller creation
- **03-PROTOCOL-EXTENSION.md** - Adding commands to existing controllers
- **04-CHANGE-PROPAGATION.md** - File sync checklists, verification
- **05-BUILD-AND-FLASH.md** - Build systems, PlatformIO, BOOTSEL
- **06-TEST-SUITE.md** - pytest usage, test patterns
- **07-CLI-UPDATES.md** - Interactive CLI modification

Additional references:
- `controllers/lib/serial/PROTOCOL.md` - Binary protocol specification
- `controllers/lib/serial/README.md` - Serial library architecture
- `tests/README.md` - Test framework overview
