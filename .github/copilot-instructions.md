# ScaleFX Development Guide

> **AI AGENTS:** This is a multi-platform embedded effects system. Read `/instructions/README.md` first for comprehensive guidance.

## System Architecture

ScaleFX is a modular scale model effects system with three platform targets:
- **Pico Controllers** (RP2040): Real-time device control (GunFX, LightFX, GearControl, HubFX Pico)
- **Raspberry Pi Hub** (Linux/C): Audio mixing and PWM monitoring (HubFX Pi)
- **Windows Studio** (.NET 8/C#): Visual configuration editor

**Communication:** Binary COBS protocol over USB serial (115200 baud)
- Packet format: `[type:u8][tag:u8][len:u8][payload:0-64][crc8:u8]`
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
| `controllers/lib/serial/serial_core.h` | `tests/framework/packets.py` | Packet type constants, generic error codes |
| `controllers/lib/serial/serial_gunfx.h` | `tests/framework/packets.py`, `commands.py` | GunFX packet types, error codes, commands |
| `controllers/lib/serial/serial_lightfx.h` | `tests/framework/packets.py`, `commands.py` | LightFX packet types, error codes, commands |
| `controllers/lib/serial/serial_gearcontrol.h` | `tests/framework/packets.py`, `commands.py` | GearControl packet types, error codes, commands |

**Verification:** Run `python -m py_compile tests/framework/packets.py` after C++ changes.

### 2. Command Addition Checklist

When adding a new command to an existing controller, update ALL these files:
1. `serial_xxxfx.h` - Add packet type constant, callback typedef, `onNewCmd()` method, handler case in `handleModulePacket()` using `SFX_*` macros, error codes if needed
2. `xxxfx_pico.ino` - Implement callback, register in `setup()`
4. `tests/framework/packets.py` - Mirror constant in `XxxPacket` class
5. `tests/framework/commands.py` - Add static builder method in `XxxCommands`
6. `tests/xxxfx/test_<feature>.py` - **REQUIRED:** Add tests for new functionality
7. `tests/cli/handlers/xxxfx.py` - Add command to handler class
8. `controllers/xxxfx/pico/README.md` - Document payload format

**ALWAYS update tests when protocol is changed or new features are added.** Tests are not optional.

**ALWAYS update the CLI when new commands are added.** The CLI is the primary debugging tool.

See `/instructions/03-PROTOCOL-EXTENSION.md` and `/instructions/04-CHANGE-PROPAGATION.md` for details.

### 3. Client Response Handling (MANDATORY)

All client methods (in `BusClient` subclasses like `GunFxClient`, `LightFxClient`, `GearControlClient`) MUST return `CommandResult`, never `bool`. Every command sent via `sendCommand()` uses **tag correlation** — the `ResultQueue` matches responses to requests by tag.

**Three response categories:**

| Category | Server Pattern | Client Pattern | Tag Resolution |
|----------|---------------|----------------|----------------|
| **Instant** | `SFX_DISPATCH` → ACK/NACK | `sendCommand()` returns result | Automatic (`BusClient::handlePacket()`) |
| **Query** | Custom response packet (no SFX_DISPATCH) | `sendCommand()` + `onModulePacket()` | Manual (`_resultQueue.resolve()` in `onModulePacket()`) |
| **Long-Running** | `SFX_DISPATCH` → immediate ACK | `sendCommand()` + polling/callbacks | Automatic ACK, optional progress resolution |

**Rules:**
1. **Never return `bool`** from client methods — always `CommandResult`
2. **Query responses are implicit ACKs** — when `onModulePacket()` receives a data response, resolve the tag as `CommandResult::Ack()`. The server sending a typed response packet IS the acknowledgment.
3. **Always check `tag != CoreProtocol::TAG_ASYNC`** before resolving — async/unsolicited packets have no pending request to resolve
4. **`onModulePacket()` contract:** parse → fire callback → resolve tag (in that order)

**Anti-pattern:**
```cpp
// BAD: Returns bool — caller loses error info, tag, and message
bool ledBrightness(uint8_t ch, uint8_t val) {
    return sendCommand(LightFxPacket::LED_BRIGHTNESS, ...).success;
}

// GOOD: Returns CommandResult — full error context preserved
CommandResult ledBrightness(uint8_t ch, uint8_t val) {
    return sendCommand(LightFxPacket::LED_BRIGHTNESS, ...);
}
```

See `01-ARCHITECTURE.md` § Client Response Handling Design for full details.

### 4. Endianness Pattern (CRITICAL)

**C++ payload parsing:**
```cpp
uint16_t value = payload[0] | (payload[1] << 8);  // Little-endian
```

**Python payload building:**
```python
payload = struct.pack('<H', value)  # '<' = little-endian
```

### 5. Physical Units in Code (MANDATORY)

**All variables, parameters, struct fields, and methods that represent physical measurements MUST include the unit and magnitude as a suffix.** This prevents unit-mismatch bugs (e.g., treating milliamps as amps).

**Naming convention:** `<name>_<unit>` where unit is the SI abbreviation with magnitude prefix:

| Quantity | Suffix | Example |
|----------|--------|---------|
| Voltage | `_mV`, `_V`, `_uV` | `busVoltage_mV`, `shuntVoltage_uV` |
| Current | `_mA`, `_A` | `current_mA`, `maxCurrent_A` |
| Power | `_mW`, `_W` | `power_mW` |
| Resistance | `_ohms`, `_mohm` | `shuntResistance_mohm` |
| Time | `_ms`, `_us`, `_s` | `timeout_ms`, `pulseWidth_us` |
| Frequency | `_Hz`, `_MHz` | `cpuFreq_MHz` |
| Distance | `_mm`, `_m` | `range_mm` |
| Temperature | `_C`, `_F` | `temp_C` |

**Rules:**
1. Method names: `busVoltage_mV()` not `busVoltage()` — the unit is part of the API contract
2. Struct fields: `float voltage_mV` not `float voltage` — eliminates "what unit is this?" questions
3. Wire format comments: always annotate `// mV`, `// mA` etc. at packing/unpacking sites
4. No implicit conversions at call sites — if the method returns mV, the caller should not need `* 1000`
5. Datasheet references: include section numbers for hardware register constants (e.g., `// INA226 §7.6.2`)

**Anti-pattern (caused a real bug):**
```cpp
// BAD: busVoltage() returns V, current() returns mA — inconsistent, no units in name
float v = monitor.busVoltage();        // V? mV? 
float i = monitor.current() * 1000.0f; // is this A→mA or mA→µA??

// GOOD: units in method names, consistent milli-prefix
float v = monitor.busVoltage_mV();     // unambiguous: millivolts
float i = monitor.current_mA();        // unambiguous: milliamps
```

### 6. PicoServer Pattern (Server Controllers)

All Pico server controllers use `PicoServer` to eliminate boilerplate:
```cpp
PicoServer server;
XxxFxServer xxxfxServer;

void setup() {
    server.begin("XxxFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]()     { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });
    
    xxxfxServer.begin(&Serial, server.deviceName());
    // ... register module callbacks ...
    server.core().onStatusData([](uint8_t* buf, size_t max) -> size_t { ... });
    
    server.addModuleHandler(&xxxfxServer);
}

void loop() {
    server.loop();       // protocol, timeout, indicators
    updateHardware();    // module-specific work
    server.indicators().setErrorCondition(hasError);  // optional
    delay(1);
}
```

PicoServer handles: serial init, device naming, indicator LEDs, CoreCommandServer, CommandRouter, connection timeout, free RAM updates.

### 7. Component Reuse (MANDATORY)

**Always check `controllers/lib/components/` before writing hardware-specific code.** If a generic driver or abstraction already exists, use it.

**Rules:**
1. **Reuse first:** Before writing any LED, servo, PWM input, I2C, or power monitoring code, check if a component exists in the `components` library
2. **Generalize new hardware drivers:** When adding support for a new hardware peripheral (sensor, actuator, display, etc.), create the driver in `components/` as a reusable class — not inline in controller firmware
3. **Controller firmware = glue code:** Controllers should only contain protocol handling and controller-specific logic. Hardware interaction should be delegated to component classes
4. **Extend I2CDevice:** All new I2C device drivers MUST extend `I2CDevice` and override `identify()` for device verification
5. **Follow existing patterns:** New components should follow the same API patterns as existing ones (e.g., `begin()` for init, callbacks via `std::function`, state queries)

**When to create a new component:**
- You need to interact with a hardware peripheral that no existing component covers
- The same hardware interaction pattern appears in 2+ controllers
- A controller-specific hardware class could be useful in other contexts

**Anti-pattern:**
```cpp
// BAD: Hardware I/O embedded in controller firmware
void readBattery() {
    Wire.beginTransmission(0x40);
    Wire.write(0x02);
    Wire.endTransmission();
    Wire.requestFrom(0x40, 2);
    int raw = Wire.read() << 8 | Wire.read();
    float voltage = raw * 1.25;  // what unit? what device? no reuse possible
}

// GOOD: Use component from library
INA226 batteryMonitor;
batteryMonitor.begin(Wire, 0x40, 0.1f, 3.2f);
batteryMonitor.update();
float v = batteryMonitor.busVoltage_mV();  // clear, reusable, tested
```

### 8. Indicator LEDs and Error Reporting (MANDATORY)

**All Pico server controllers use `PicoServer` which automatically manages indicator LEDs on GP13/GP14.** Controllers only need to set error/warning conditions.

#### Indicator LED Standard

| LED | Pin | Purpose | Waiting for INIT | Connected | Connection Lost |
|-----|-----|---------|-----------------|-----------|----------------|
| **LED 0** | GP13 | Connection | Blink 500ms | Solid ON | OFF |
| **LED 1** | GP14 | Error | OFF | OFF | OFF (blink 200ms if error) |

#### Required Implementation

Every controller firmware uses `PicoServer` which handles indicators automatically:
```cpp
// Declare PicoServer (handles indicators internally)
PicoServer server;

// In setup():
server.begin("MyController", FIRMWARE_VERSION, BUILD_NUMBER);
server.onInit([]()     { performSafeInit(); });
server.onShutdown([]() { performSafeShutdown(); });

// In loop(): set error/warning conditions, PicoServer updates LEDs
server.indicators().setErrorCondition(hasError);
server.indicators().setWarningCondition(hasWarning);
server.loop();  // Calls indicators.update() automatically
```

**Connection state rules (handled by PicoServer):**
- `doInit()` sets `connected = true` and `watchdogTriggered = false`
- `doShutdown()` sets `connected = false`
- Connection timeout (15s) triggers `doShutdown()` and sets `watchdogTriggered = true`

#### Error Code Ranges

Each module's error codes MUST be in its assigned range and defined in both C++ and Python:

| Range | Module | C++ Namespace | Python Class |
|-------|--------|---------------|-------------|
| `0x00-0x0F` | Generic | `SerialError` | `CoreError` |
| `0x10-0x1F` | Parameter | `SerialError` | `CoreError` |
| `0x20-0x4F` | GunFX | `GunFxError` | `GunFxError` |
| `0x50-0x5F` | LightFX | `LightFxError` | `LightFxError` |
| `0x60-0x6F` | GearControl | `GearControlError` | `GearControlError` |
| `0x70-0x8F` | Reserved | — | — |
| `0xF0-0xFF` | System | `SerialError` | `CoreError` |

**Error code rules:**
1. Every C++ error constant MUST have a matching Python constant with the same value
2. Never define error codes outside the module's assigned range
3. Remove unused/dead error codes — they cause sync confusion
4. Each error namespace MUST have a `getMessage()` (C++) / `name()` (Python) function
5. Use generic `SerialError` codes (e.g., `INVALID_ID`, `MISSING_PARAMETER`) where appropriate instead of duplicating concepts per module

### 9. Firmware Versioning (MANDATORY)

Every controller firmware defines `FIRMWARE_VERSION` and `BUILD_NUMBER`:
```cpp
#define FIRMWARE_VERSION "0.3.0"
#define BUILD_NUMBER 3
```

**Rules:**
1. **BUILD_NUMBER:** Increment on **every** firmware change that gets flashed, even single-line fixes. This is the primary "did the flash succeed?" indicator — the INIT_READY response includes it, so you can verify the running firmware matches what was built.
2. **FIRMWARE_VERSION:** Follows semantic versioning (`MAJOR.MINOR.PATCH`):
   - **PATCH** (0.3.0 → 0.3.1): Bug fixes, internal refactors, no protocol changes
   - **MINOR** (0.3.0 → 0.4.0): New commands, new features, backward-compatible protocol additions
   - **MAJOR** (0.3.0 → 1.0.0): Breaking protocol changes (payload format change, removed commands)
3. **Update together:** When bumping VERSION, also increment BUILD_NUMBER
4. **README:** Update the version history table in the controller's README.md

## Key Architecture Patterns

### Client-Server Topology
```
HubFX (Client) - USB Host with RP2040
  ├─ USB Port 0 → GunFX Pico (Server)
  ├─ USB Port 1 → LightFX Pico (Server)
  ├─ USB Port 2 → GearControl Pico (Server)
  └─ USB Port N → Other Servers
```

### Handler Registration (CRITICAL)

`PicoServer` automatically registers `coreServer` before the module handler. All controllers MUST use `PicoServer.addModuleHandler()` which guarantees correct handler priority.

```cpp
// CORRECT - PicoServer handles registration order
PicoServer server;
server.begin("XxxFX", FIRMWARE_VERSION, BUILD_NUMBER);
server.addModuleHandler(&xxxfxServer);  // Core added automatically first

// WRONG - manual setup without PicoServer (deprecated pattern)
commandRouter.addHandler(&xxxfxServer);  // ← Missing coreServer!
```

### Shared Serial Library (`controllers/lib/serial/`)
- **serial.h** - Umbrella include (use this)
- **serial_core.h** - CoreProtocol, SerialError, CommandResult, ICommandHandler, CommandRouter, SFX_* macros
- **serial_bus_server.h** - BusServer base class + CoreCommandServer (server side)
- **serial_bus_client.h** - BusClient base class (client side, extends SerialBus)
- **serial_bus.h** - SerialBus (client-only, COBS over USB CDC)
- **serial_result_queue.h** - ResultQueue (tag-correlated command/response matching)
- **serial_gunfx.h** - GunFxServer, GunFxClient, GunFxPacket, GunFxError, GunFxSpec
- **serial_lightfx.h** - LightFxServer, LightFxClient, LightFxPacket, LightFxError, LightFxSpec
- **serial_gearcontrol.h** - GearControlServer, GearControlClient, GearControlPacket, GearControlError, GearControlSpec

Include order: `#include "serial.h"` (includes everything needed)

### Components Library (`controllers/lib/components/`)
Reusable hardware component drivers — use these instead of writing controller-specific code:
- **battery_monitor.h/.cpp** - ADC battery voltage monitor (LiPo/Li-Ion, cell detection, low-voltage alerts)
- **i2c_device.h/.cpp** - I2CDevice base class for all I2C peripherals
- **ina226.h/.cpp** - TI INA226 power/current/voltage monitor (extends I2CDevice)
- **indicator_leds.h/.cpp** - Connection/error LED state machine (GP13/GP14)
- **led_control.h/.cpp** - GPIO LED on/off, toggle, active-low, PWM brightness
- **led_events.h** - ILedEvent interface and built-in animations (LedOn, LedOff, LedFlashing, LedFading, etc.)
- **led_event_seq.h/.cpp** - Looping sequence of LED events
- **pico_server.h/.cpp** - Common Pico server controller boilerplate (serial, device name, indicators, core protocol, connection management)
- **pwm_control.h/.cpp** - RC PWM input with averaging, hysteresis, async callbacks
- **srv_control.h/.cpp** - Servo output with trapezoidal motion profiling and jerk effects

### Server Handler Macros (serial_core.h)
Reduce boilerplate in `handleModulePacket()` switch cases:
```cpp
SFX_REQUIRE_LEN(n)                    // NACK MISSING_PARAMETER if len < n
SFX_VALIDATE(cond, err)               // NACK err if !cond
SFX_DISPATCH(callback, args...)       // Call callback, ACK/NACK on result
SFX_HANDLE_CHANNEL_CMD(v, err, cb)    // Validate + dispatch single-param cmd
```

### Rich STATUS Pattern

Every controller provides board-specific status via `PicoServer`:

```cpp
// In setup(): Register module status callback
server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
    buf[0] = myFlag;
    CoreProtocol::putU16LE(&buf[1], myServo);
    return 3;  // bytes written
});

// Free RAM is updated automatically by server.loop()
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
      ├── lightfx.py   - LightFX commands (led, servo, power)
      └── gearcontrol.py - GearControl commands (gear, servo, yaw)
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
| 0x60-0x7F | GearControl | Used | Gear, servo, yaw |
| 0x80-0xEF | Available | Free | New controllers |
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
2. Determine response category: instant, query, or long-running
3. Choose packet type ID from available range
4. Update C++ serial library (serial_xxxfx.h) — packet type, handler in `handleModulePacket()`, use `SFX_*` macros
5. Add client method returning `CommandResult` (serial_xxxfx.h client class)
6. If query: add response handling in `onModulePacket()` with tag resolution
7. Update firmware (xxxfx_pico.ino)
8. Update Python framework (packets.py, commands.py)
9. Update CLI handler (tests/cli/handlers/xxxfx.py)
10. Add test (tests/xxxfx/test_feature.py)
11. Update README.md protocol table
12. Verify: `pio run && python -m py_compile tests/framework/packets.py`

### Creating a New Controller
1. Read `/instructions/02-NEW-CONTROLLER.md`
2. Reserve packet type range (0x80-0xEF available)
3. Create `controllers/lib/serial/serial_newfx.h` (NewFxServer extends BusServer, NewFxClient extends BusClient)
4. Create `controllers/newfx/pico/` directory structure
5. Create Python test framework classes
6. Add CLI commands
7. Document in `README.md`

### Debugging Protocol Issues
- Use interactive CLI: `python -m tests.cli.interactive`
- Check constants match: Compare `serial_core.h` / `serial_xxxfx.h` vs `packets.py`
- Verify endianness: All multi-byte values are little-endian
- Check CRC: CRC-8 poly 0x07 over [type][tag][len][payload]
- NACK error codes in `serial_core.h` (generic) and `serial_xxxfx.h` (module) must match `packets.py`

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
