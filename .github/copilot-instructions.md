# ScaleFX Development Guide

> **AI AGENTS:** This is a multi-platform embedded effects system. Read `/instructions/README.md` first for comprehensive guidance.

## System Architecture

ScaleFX is a modular scale model effects system with three platform targets:
- **Pico Controllers** (RP2040): Real-time device control (GunFX, LightFX, GearControl)
- **ESP32-S3 Controller**: HubFX ESP32-S3 (master hub, active development)
- **Windows Studio** (.NET 8/C#): Visual configuration editor

> **HubFX Pico (RP2350) is OBSOLETE.** The Pico variant (`controllers/hubfx/pico/`) is frozen as a reference implementation. All new HubFX development (features, bug fixes, protocol additions) MUST target `controllers/hubfx/esp32s3/`. See Rule 17.

**Communication:** Binary COBS protocol over USB serial (6Mbps baud)
- Packet format: `[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]`
- CRC-8 polynomial: 0x07
- Endianness: Little-endian for ALL multi-byte values

## Quick Command Reference

```bash
# Build Pico firmware (PlatformIO)
cd controllers/{gunfx|lightfx|gearcontrol}/pico
python -m platformio run -e pico

# Build ESP32-S3 firmware (PlatformIO)
cd controllers/hubfx/esp32s3
python -m platformio run -e esp32s3

# Build and flash with verification (centralized script)
python scripts/build_and_flash.py gunfx
python scripts/build_and_flash.py lightfx --port COM10
python scripts/build_and_flash.py noop --no-build
python scripts/build_and_flash.py gunfx --no-clean      # incremental build
python scripts/build_and_flash.py lightfx --skip-verify  # skip post-flash check
python scripts/build_and_flash.py hubfx                  # ESP32-S3 (uses esptool)

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
| `controllers/lib/sfx_serial/serial/core/core.h` | `tests/framework/packets.py` | Packet type constants, generic error codes |
| `controllers/lib/sfx_serial/serial/gunfx/gunfx.h` | `tests/framework/packets.py`, `commands.py` | GunFX packet types, error codes, commands |
| `controllers/lib/sfx_serial/serial/lightfx/lightfx.h` | `tests/framework/packets.py`, `commands.py` | LightFX packet types, error codes, commands |
| `controllers/lib/sfx_serial/serial/gearcontrol/gearcontrol.h` | `tests/framework/packets.py`, `commands.py` | GearControl packet types, error codes, commands |
| `controllers/lib/sfx_serial/serial/hubfx/hubfx.h` | `tests/framework/packets.py`, `commands.py` | HubFX packet types, error codes, commands |

**Verification:** Run `python -m py_compile tests/framework/packets.py` after C++ changes.

### 2. Command Addition Checklist

When adding a new command to an existing controller, update ALL these files:
1. `xxxfx/xxxfx.h` - Add packet type constant, callback typedef, `onNewCmd()` method, handler case in `handleModulePacket()` using `SFX_*` macros, error codes if needed
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

### 6. SfxServer Pattern (Server Controllers)

All Pico server controllers use `SfxServer` to eliminate boilerplate:
```cpp
SfxServer server;
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
    busy_wait_ms(1);
}
```

SfxServer handles: serial init, device naming, indicator LEDs, CoreCommandServer, CommandRouter, connection timeout, free RAM updates.

### 7. Component Reuse (MANDATORY)

**Always check `controllers/lib/` before writing hardware-specific code.** If a generic driver or abstraction already exists, use it. If it's close but not quite right, **enhance the existing library** — do not duplicate functionality in controller firmware.

**Rules:**
1. **Reuse first:** Before writing any LED, servo, PWM input, I2C, USB, audio, storage, or power monitoring code, check if a component exists in the shared libraries (sfx_peripherals, sfx_audio, sfx_storage, sfx_usb, etc.)
2. **Enhance, don't bypass:** If an existing library component almost fits but needs a minor addition (new method, new callback, configuration option), add it to the library. Never work around a library limitation by writing inline code in controller firmware.
3. **Generalize new hardware drivers:** When adding support for a new hardware peripheral (sensor, actuator, display, etc.), create the driver in the appropriate `lib/sfx_*` library as a reusable class — not inline in controller firmware
4. **Controller firmware = glue code:** Controllers should only contain protocol handling and controller-specific logic. Hardware interaction should be delegated to component classes
5. **Extend I2CDevice:** All new I2C device drivers MUST extend `I2CDevice` and override `identify()` for device verification
6. **Follow existing patterns:** New components should follow the same API patterns as existing ones (e.g., `begin()` for init, callbacks via `std::function`, state queries)

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

**All Pico server controllers use `SfxServer` which automatically manages indicator LEDs on GP13/GP14.** Controllers only need to set error/warning conditions.

#### Indicator LED Standard

| LED | Pin | Purpose | Waiting for INIT | Connected | Connection Lost |
|-----|-----|---------|-----------------|-----------|----------------|
| **LED 0** | GP13 (Pico) / GP48 (ESP32-S3) | Connection | Blink 500ms | Solid ON | OFF |
| **LED 1** | GP14 (Pico) / disabled (ESP32-S3) | Error | OFF | OFF | OFF (blink 200ms if error) |

#### Required Implementation

Every controller firmware uses `SfxServer` which handles indicators automatically:
```cpp
// Declare SfxServer (handles indicators internally)
SfxServer server;

// In setup():
server.begin("MyController", FIRMWARE_VERSION, BUILD_NUMBER);
server.onInit([]()     { performSafeInit(); });
server.onShutdown([]() { performSafeShutdown(); });

// In loop(): set error/warning conditions, SfxServer updates LEDs
server.indicators().setErrorCondition(hasError);
server.indicators().setWarningCondition(hasWarning);
server.loop();  // Calls indicators.update() automatically
```

**Connection state rules (handled by SfxServer):**
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
5. **Agent MUST bump version proactively** — do not wait for the user to ask. Determine the version impact at time of code change:
   - If you changed a wire format field type (e.g., u16→u32), that is a **MAJOR** breaking change
   - If you added a new packet type or optional payload field, that is a **MINOR** change
   - If you only fixed logic without changing any wire format, that is a **PATCH** change

**Breaking change examples (MAJOR):**
- Changing a payload field's type or size (e.g., `remaining_ms` from u16 to u32)
- Reordering payload fields
- Removing a packet type or changing its value
- Changing the meaning of an existing field

**Non-breaking examples (MINOR):**
- Adding a new packet type
- Appending optional fields to the end of an existing payload (with length check)
- Adding a new error code

### 10. Flash Verification and Build Number Tracking (MANDATORY)

**Rules:**
1. **`build_and_flash.py` auto-increments BUILD_NUMBER** on every build invocation. If a flash fails (verification timeout), the BUILD_NUMBER in source is still incremented. Track the actual running firmware by its INIT_READY response, not the source define alone.
2. **Verification timeout ≠ flash failure.** If the script copies the UF2 successfully but the post-flash COM port check times out, the firmware IS flashed. Re-run with `--no-build` to verify, or connect via CLI and run `init`.
3. **Always verify after flash:** Run `init` in the CLI to confirm the device name, version, and build number match expectations. Don't assume a successful UF2 copy means the firmware is running correctly.
4. **Use `--no-build` for re-flash:** If a flash verification fails, use `--no-build` to skip recompilation and just re-attempt the flash/verify cycle.

### 11. Optional/Backward-Compatible Payload Extension (MANDATORY)

When extending an existing command's payload, **new fields MUST be optional and appended at the end** to preserve backward compatibility.

**Pattern (C++ server):**
```cpp
// Read optional field with default fallback
uint8_t newField = (len >= 2) ? payload[1] : 0;  // Default to 0 if not sent
```

**Pattern (Python client):**
```python
# Only include field in payload when non-default
if new_field > 0:
    payload = struct.pack('<BB', required_field, new_field)
else:
    payload = struct.pack('<B', required_field)
```

**Rules:**
1. **Never change the meaning** of existing payload bytes — append only
2. **Default = no-op:** The default value for optional fields MUST preserve the original behavior (e.g., `timeout_s=0` means "no timeout")
3. **Check `len`** on the server side to detect whether the optional field was sent
4. **Document backward compatibility** in the protocol table (e.g., "2nd byte optional")

### 12. Channel Enable/Disable Pattern (RECOMMENDED)

For controllers with multiple independent channels (gears, LEDs, servos), implement a channel enable/disable mechanism:

**Rules:**
1. **Guard at the top** of action methods: `if (!_enabled) return ERROR_DISABLED;`
2. **Safe disable:** When disabling, stop any active operations (sequences, calibrations, motors)
3. **Default enabled:** All channels start enabled on power-up
4. **Not persisted:** Enable/disable state resets on reboot (safety — all channels available after power cycle)
5. **Reflected in STATUS:** Use a runtime-only bit in the config flags (e.g., bit 7 = ENABLED)
6. **Dedicated error code:** Return a specific "disabled" error, not a generic one — the client needs to distinguish "disabled" from "busy" or "invalid"

### 13. Error State Lifecycle (MANDATORY)

Controllers MUST provide a way to clear error states. Error states should not require a full reboot to recover.

**Rules:**
1. **Explicit reset command:** Provide a `RESET` or `CLEAR_ERROR` command (not just reboot)
2. **Transition ERROR → UNKNOWN:** After clearing, the state should be indeterminate (UNKNOWN), not assumed (DEPLOYED/RETRACTED)
3. **Clear error reason:** Reset both the state and the error reason/diagnostic data
4. **Reject operations in ERROR:** Commands like deploy/retract should fail when in ERROR state — require explicit reset first
5. **Report in STATUS:** Error reasons, error state, and clear actions should all be visible in the STATUS response

### 14. Singleton Pattern for Board-Unique Resources (MANDATORY)

Any object that exists as a **single instance per board** — whether it wraps a hardware peripheral or represents a logical registry — MUST be implemented as a singleton using the C++11 thread-safe static local pattern.

**Qualifying criteria (any of these → singleton):**
1. **Single hardware resource:** Only one physical peripheral exists (SD card, flash, USB host, I2S output, audio codec)
2. **Single logical registry:** Central catalog of system state (slave registry, config reader)
3. **Board-wide service:** Infrastructure used by multiple modules from any core (diagnostic log)

**Required pattern:**
```cpp
class MyModule {
public:
    static MyModule& instance() {
        static MyModule inst;
        return inst;
    }

    // Delete copy/move
    MyModule(const MyModule&) = delete;
    MyModule& operator=(const MyModule&) = delete;
    MyModule(MyModule&&) = delete;
    MyModule& operator=(MyModule&&) = delete;

    bool begin(/* init params */);  // Idempotent initialization

private:
    MyModule();  // Private constructor
};
```

**Current singletons:**

| Class | Location | Resource Type |
|-------|----------|---------------|
| `DiagLog` | `lib/sfx_platform/platform/diag_log.h` | Board-wide logging service |
| `SdCardModule` | `hubfx/pico/src/storage/sd_card.h` | Single SPI SD card |
| `FlashModule` | `hubfx/pico/src/storage/flash.h` | Single onboard LittleFS flash |
| `AudioMixer` | `lib/sfx_audio/audio/audio_mixer.h` | Single I2S audio output |

**Rules:**
1. **Access via `::instance()`** — never via global pointer, extern declaration, or injected pointer. Consumers call `MyModule::instance()` directly.
2. **Private constructor** — prevents accidental stack/heap allocation.
3. **Deleted copy/move** — enforces single instance.
4. **Idempotent `begin()`** — safe to call multiple times (e.g., `_mutexInitialized` guard for pico mutexes).
5. **Pre-`begin()` safety** — methods called before `begin()` must fail gracefully (check `_serial != nullptr` or `_initialized`).
6. **Thread safety** — if accessed from multiple cores, protect shared state with `mutex_t`. The `static` local initialization itself is thread-safe under C++11.
7. **No setter injection** — do NOT add `setXxx(Singleton* ptr)` methods to consumers. Consumers access the singleton directly.

**NOT singletons (these are correct as regular instances):**
- Protocol handlers (`*Server` extending `BusServer`) — receive `Stream*` and participate in `CommandRouter` chain
- Per-slave clients (`GunFxClient`, `LightFxClient`, etc.) — one per connected slave device
- Per-channel hardware (`ServoControl[]`, `LedControl[]`, `INA226[]`) — arrays of independent channels
- Effect modules (`EngineFX`, `MuzzleFlash`, `SmokeGenerator`) — configurable effect instances

**Anti-pattern:**
```cpp
// BAD: Global pointer + setter injection for a single-instance resource
SdCardModule* _sdCard = nullptr;
void setSdCardModule(SdCardModule* sd) { _sdCard = sd; }
if (!_sdCard) { sendNack(ERROR); return; }  // Null check everywhere

// GOOD: Singleton — always available, no null checks
SdCardModule& sd = SdCardModule::instance();
if (!sd.isInitialized()) { sendNack(ERROR); return; }  // Check state, not existence
```

### 15. Dual-Core Thread Safety (MANDATORY — HubFX/RP2350)

**HubFX runs on RP2350 (Pico 2) with dual cores.** Any flag or state variable accessed by both cores MUST use `std::atomic<T>` with explicit memory ordering (`memory_order_release` for writes, `memory_order_acquire` for reads). This replaces manual `volatile` + `__dmb()` with a portable, self-documenting C++ pattern.

**Core responsibilities:**
| Core | Responsibilities |
|------|------------------|
| **Core 0** | Protocol handling, SD card, WAV decoding, mixing (producer) |
| **Core 1** | Audio I2S output (consumer), USB Host (when enabled) |

**Rules:**
1. **`std::atomic<T>` for cross-core flags** — any `bool`, `int`, `uint*_t`, or pointer written by one core and read by another MUST be `std::atomic<T>`. The atomic type prevents register caching AND provides memory ordering guarantees.
2. **`memory_order_release` on writes** — ensures all preceding writes are visible before the flag is set. Replaces the old `volatile` + `__dmb()` pattern.
3. **`memory_order_acquire` on reads** — ensures subsequent reads see all writes that happened before the corresponding release. Use in poll loops and cross-core guard checks.
4. **`memory_order_relaxed` for own-index reads** — in SPSC patterns, the owning core can read its own index with relaxed ordering (no barrier needed for self-reads).
5. **Implicit operators for same-core reads** — `std::atomic<T>::operator T()` uses `seq_cst` by default, which is correct and convenient for reads on the same core that writes.
6. **Annotate with core ownership** — document which core writes and which reads:
   ```cpp
   std::atomic<bool> _initialized{false};   // Core 0 writes, Core 1 reads
   std::atomic<uint32_t> _writeIdx{0};      // Core 0 writes, Core 1 reads
   ```
7. **Use mutex for complex shared state** — for multi-field updates or producer-consumer queues where multiple values must stay consistent, use `mutex_t` with `mutex_enter_blocking()` / `mutex_exit()`. Variables protected by mutex MUST still be `std::atomic` when accessed outside the mutex (e.g., diagnostic reads, status queries).
8. **No `volatile` — always `std::atomic`** — even diagnostic/status counters (`_underruns`, `_consumeLoops`, `_channelPlaying[]`, etc.) MUST be `std::atomic<T>`. Use `memory_order_relaxed` for same-core or non-critical reads/writes. `volatile` is never correct for cross-core visibility on Cortex-M33.
9. **Pointer atomics need local extraction** — `std::atomic<T*>` has no `operator->()`. Extract to a local:
   ```cpp
   Stream* serial = _serial.load(std::memory_order_acquire);
   if (!serial) return;
   serial->write(buf, len);  // use local, not _serial->write()
   ```
10. **Zero-tolerance `volatile` audit rule** — when reviewing or modifying any HubFX or shared-library code (`controllers/lib/`), **proactively scan for `volatile` variables and replace them with `std::atomic<T>`**. This applies even if the variable is currently single-core — shared library code may run on multi-core targets. The only acceptable use of `volatile` is for memory-mapped I/O registers (hardware peripheral access), never for inter-thread or inter-core communication.
11. **Banned Arduino APIs** — see Rule 16 for the full platform-native API table. Key points: `delay()`, `sleep_ms()`, `delayMicroseconds()` are **banned on ALL cores** (use `busy_wait_ms()`/`busy_wait_us_32()`). `Serial.print()` is unsafe on Core 1 (use DiagLog). `setup1()` should be minimal — set an atomic flag and return.

**Current cross-core variables (HubFX):**

| Variable | Type | Location | Owner | Ordering |
|----------|------|----------|-------|----------|
| `audioInitialized` | `std::atomic<bool>` | hubfx_pico.ino | Core 0 writes | release/acquire |
| `core1Ready` | `std::atomic<bool>` | hubfx_pico.ino | Core 1 writes | release/acquire |
| `loop1Count` | `std::atomic<uint32_t>` | hubfx_pico.ino | Core 1 writes | relaxed/acquire |
| `_initialized` | `std::atomic<bool>` | AudioMixer | Core 0 writes | release/acquire |
| `_i2sRunning` | `std::atomic<bool>` | AudioMixer | Core 1 writes | release/acquire |
| `_serial` | `std::atomic<Stream*>` | DiagLog | Core 0 writes | release/acquire |
| `_mutexInitialized` | `std::atomic<bool>` | DiagLog | Core 0 writes | release/acquire |
| `_head` / `_tail` | `std::atomic<uint16_t>` | DiagLog | mutex + atomic | release/acquire |
| `_overwritten` | `std::atomic<uint32_t>` | DiagLog | mutex + atomic | relaxed/acquire |
| `_writeIdx` / `_readIdx` | `std::atomic<uint32_t>` | AudioRingBuffer | SPSC | release/acquire |
| `_cmdQueueHead/Tail` | `std::atomic<int>` | AudioMixer | mutex + atomic | release/acquire |
| `_underruns` | `std::atomic<uint32_t>` | AudioMixer | Core 1 writes | relaxed/acquire |
| `_consumeLoops` | `std::atomic<uint32_t>` | AudioMixer | Core 1 writes | relaxed/acquire |
| `_consumeFrames` | `std::atomic<uint32_t>` | AudioMixer | Core 1 writes | relaxed/acquire |
| `_channelPlaying[]` | `std::atomic<bool>` | AudioMixer | Core 0 writes | relaxed (same-core) |
| `_channelRemainingSec[]` | `std::atomic<float>` | AudioMixer | Core 0 writes | relaxed (same-core) |
| `_minLevel` | `std::atomic<uint8_t>` | DiagLog | Core 0 writes | relaxed/relaxed |
| `_waitingTag` | `std::atomic<uint8_t>` | ResultQueue | same-core (atomic for policy) | relaxed |
| `_waitResolved` | `std::atomic<bool>` | ResultQueue | same-core (atomic for policy) | release/acquire |

**Anti-pattern (caused the Core 1 hang bug):**
```cpp
// BAD: volatile alone — Core 1 never sees update on Cortex-M33
volatile bool audioInitialized = false;
audioInitialized = true;          // Core 0 writes — may sit in write buffer
// Core 1:
while (!audioInitialized) {}      // Hangs forever! Write buffer not drained

// GOOD: std::atomic with release/acquire — portable, self-documenting
std::atomic<bool> audioInitialized{false};
audioInitialized.store(true, std::memory_order_release);   // Core 0
// Core 1:
if (audioInitialized.load(std::memory_order_acquire)) break;  // Sees update
```

**Single-core controllers (GunFX, LightFX, GearControl):** These rules do not apply — they run single-threaded on Core 0 only. However, shared code (DiagLog) uses `std::atomic` unconditionally for portability — on single-core targets the atomic operations compile to simple loads/stores with negligible overhead.

### 16. Platform-Native API Usage (MANDATORY)

**All firmware code MUST use the native SDK for the target platform, not Arduino wrapper functions.** Arduino wrappers hide platform differences but often use shared resources (alarm pools, interrupt handlers) that cause contention on multi-core chips or have unexpected behavior. Shared library code (`controllers/lib/`) MUST use `#ifdef` guards to select the correct native API per platform.

**Platform detection macros:**

| Macro | When Defined | Platform |
|-------|-------------|----------|
| `PICO_RP2040` | RP2040 builds | Pico (GunFX, LightFX, GearControl, NoOp) |
| `PICO_RP2350` | RP2350 builds | Pico 2 (HubFX current) |
| `ARDUINO_ARCH_RP2040` | Any RP2xxx Arduino-Pico build | Both RP2040 and RP2350 |
| `ESP32` or `ARDUINO_ARCH_ESP32` | ESP-IDF Arduino build | ESP32-S3 (HubFX future) |
| `CONFIG_IDF_TARGET_ESP32S3` | ESP32-S3 specifically | ESP32-S3 only |

**Platform-native API equivalents:**

| Operation | Arduino (AVOID) | Pico SDK (RP2040/RP2350) | ESP-IDF (ESP32-S3) |
|-----------|----------------|--------------------------|---------------------|
| Delay (ms) | `delay(ms)` | `busy_wait_ms(ms)` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| Delay (µs) | `delayMicroseconds(us)` | `busy_wait_us_32(us)` | `esp_rom_delay_us(us)` |
| Milliseconds | `millis()` | `to_ms_since_boot(get_absolute_time())` | `esp_timer_get_time() / 1000` |
| Microseconds | `micros()` | `to_us_since_boot(get_absolute_time())` | `esp_timer_get_time()` |
| Free heap | `rp2040.getFreeHeap()` | `rp2040.getFreeHeap()` | `esp_get_free_heap_size()` |
| Reboot | `rp2040.reboot()` | `watchdog_reboot(0,0,0)` | `esp_restart()` |
| Bootloader | `rp2040.rebootToBootloader()` | `reset_usb_boot(0,0)` | N/A (OTA or USB-DFU) |
| GPIO write | `digitalWrite(pin, val)` | `gpio_put(pin, val)` | `gpio_set_level(pin, val)` |
| GPIO read | `digitalRead(pin)` | `gpio_get(pin)` | `gpio_get_level(pin)` |
| GPIO mode | `pinMode(pin, mode)` | `gpio_init(pin); gpio_set_dir(pin, dir)` | `gpio_set_direction(pin, dir)` |
| Mutex lock | N/A | `mutex_enter_blocking(&mtx)` | `xSemaphoreTake(mtx, portMAX_DELAY)` |
| Mutex unlock | N/A | `mutex_exit(&mtx)` | `xSemaphoreGive(mtx)` |
| Task/core pin | `setup1()`/`loop1()` | Native dual-core | `xTaskCreatePinnedToCore()` |
| Serial output | `Serial.print()` | DiagLog (mutex-safe) | `ESP_LOG*()` macros |

**Shared library pattern (`controllers/lib/`):**

All platform-specific abstractions are centralized in `platform/sfx_platform.h`. Shared library code MUST use these instead of raw SDK calls:

```cpp
// In any shared component:
#include "platform/sfx_platform.h"

SFX_DELAY_MS(10);                    // busy_wait_ms (Pico) / vTaskDelay (ESP32)
SFX_DELAY_US(100);                   // busy_wait_us_32 (Pico) / esp_rom_delay_us (ESP32)
uint32_t heap = SFX_FREE_HEAP();     // rp2040.getFreeHeap() / esp_get_free_heap_size()
SFX_REBOOT();                        // rp2040.reboot() / esp_restart()

SfxMutex mtx;
sfxMutexInit(mtx);                   // mutex_init (Pico) / xSemaphoreCreateMutex (ESP32)
sfxMutexLock(mtx);                   // mutex_enter_blocking / xSemaphoreTake
sfxMutexUnlock(mtx);                 // mutex_exit / xSemaphoreGive
```

See `controllers/lib/sfx_platform/platform/sfx_platform.h` for the full abstraction table including GPIO, servo, interrupt, I2S, memory attributes, and dual-core detection.

**ESP32-S3 specific notes (future HubFX migration):**
- **FreeRTOS-based** — `delay()` calls `vTaskDelay()` and yields the task (safe, unlike Pico's alarm pool). However, prefer explicit `vTaskDelay(pdMS_TO_TICKS(ms))` for clarity and portability.
- **Dual-core via tasks** — uses `xTaskCreatePinnedToCore()` instead of `setup1()`/`loop1()`. Cross-core sync uses FreeRTOS primitives (`xSemaphore*`, `xQueue*`) or `std::atomic`.
- **Logging** — use `ESP_LOGI()`, `ESP_LOGW()`, `ESP_LOGE()` instead of `Serial.print()`. These are task-safe with built-in log levels.
- **`std::atomic`** remains correct and portable for cross-core flags — same patterns as RP2350.
- **No `busy_wait_*`** — ESP-IDF provides `esp_rom_delay_us()` for µs spin-waits, but prefer `vTaskDelay()` for ms-scale waits to yield CPU to other tasks.

**Rules:**
1. **Controller firmware** uses platform-native API directly — no abstraction needed for single-platform code
2. **Shared library code** (`controllers/lib/`) MUST use `sfx_platform.h` abstractions (e.g., `SFX_DELAY_MS`, `SfxMutex`, `SFX_FREE_HEAP`) — never raw Pico SDK or ESP-IDF calls
3. **`millis()` is acceptable everywhere** — safe on all current platforms (Pico: timer register, ESP32: `gettimeofday` wrapper). Use native API only in performance-critical paths.
4. **When migrating a controller** to a new platform, audit ALL native API calls and replace with the target platform's equivalents
5. **New shared components** MUST compile on both RP2040/RP2350 and ESP32-S3 from day one — include `platform/sfx_platform.h` and use its macros/types
6. **New platform abstractions** go in `sfx_platform.h` — do not scatter `#ifdef` blocks across individual component files

### 17. HubFX Development Target (MANDATORY)

**HubFX Pico (RP2350) is OBSOLETE and FROZEN.** The `controllers/hubfx/pico/` codebase is preserved as a reference implementation only. **All new HubFX development MUST target `controllers/hubfx/esp32s3/`.**

**Rules:**
1. **No new features** in `controllers/hubfx/pico/` — do not add commands, handlers, or protocol extensions
2. **No bug fixes** in `controllers/hubfx/pico/` — unless explicitly requested by the user for hardware compatibility
3. **Reference only** — when implementing features in the ESP32-S3 variant, consult the Pico implementation for protocol patterns and domain logic, then adapt to ESP-IDF/FreeRTOS
4. **Shared libraries are shared** — changes to `controllers/lib/` that support HubFX ESP32-S3 ARE allowed and expected (with platform guards via `sfx_platform.h`)
5. **Protocol compatibility** — the ESP32-S3 variant uses the same HubFX packet types (0x80-0xAF) and wire format as the Pico variant. Protocol definitions in `hubfx/hubfx.h` are shared.
6. **When asked to "work on HubFX"** — always target `controllers/hubfx/esp32s3/` unless the user explicitly says "HubFX Pico"

### 18. Compile-Time Dispatch — Policy-Based Templates (PREFERRED)

**When a class has platform-specific behavior but only ONE instance exists per binary, use policy-based composition** to achieve compile-time dispatch without virtual functions or CRTP complexity.

**When to use each approach:**

| Technique | When | Example |
|-----------|------|---------|
| **Policy-based template** | Platform varies, single instance per binary, large shared protocol code, thin platform hooks | `StorageServerT<TPolicy>` |
| **Pure arch implementations** | Platform varies, minimal shared logic, large platform-specific implementations | `EspUsbHost` / `PicoUsbHost` |
| **`#ifdef` in single class** | 1-3 trivial platform differences (a few lines each) | Pin numbers, buffer sizes |
| **Virtual / ABC** | Runtime polymorphism needed (multiple implementations active simultaneously) | `ICommandHandler` |
| **CRTP** | Avoid — policy composition is simpler for the same use cases | — |

**Policy-based template pattern:**

```cpp
// ---- Shared state struct (both protocol code and policy need access) ----
struct FooSharedState {
    uint8_t* buffer = nullptr;
    size_t   bufferLen = 0;
    // ...inline helper methods if needed by both sides
};

// ---- Template server (protocol-agnostic, platform-agnostic) ----
template <typename TPolicy>
class FooServerT : public BusServer {
public:
    FooServerT() { _policy.init(&_shared); }
    TPolicy&       policy()       { return _policy; }
    const TPolicy& policy() const { return _policy; }

protected:
    FooSharedState _shared;

private:
    TPolicy _policy;
    // ...protocol state, command handlers...
};

// ---- Platform policy (one per target) ----
class Esp32FooPolicy {
public:
    void init(FooSharedState* state) { _state = state; }
    bool allocateBuffers();    // PSRAM, large
    void startWriterTask();    // Platform-specific public API
    // ...all required hooks...
private:
    FooSharedState* _state = nullptr;
};

class PicoFooPolicy {
public:
    void init(FooSharedState* state) { _state = state; }
    bool allocateBuffers();    // Heap, small
    // Trivial hooks inline: bool checkHealth() { return true; }
private:
    FooSharedState* _state = nullptr;
};

// ---- Auto-select at bottom of header ----
#if SFX_PLATFORM_ESP32
#include "esp32/esp32_foo_policy.h"
using FooServer = FooServerT<Esp32FooPolicy>;
#else
#include "pico/pico_foo_policy.h"
using FooServer = FooServerT<PicoFooPolicy>;
#endif
```

**Rules:**
1. **Template implementation in `.ipp`** — keeps the header clean; included at the bottom of the `.h` before the platform policy includes
2. **SharedState struct** for data both sides need — cleaner than inheritance; policy accesses via `_state->` pointer
3. **`policy()` accessor** for platform-specific public API (e.g., `server.policy().startWriterTask()`)
4. **All hooks required** — no default base class implementations. Trivial hooks on simpler platforms are inline one-liners (explicit > implicit)
5. **Platform guard** in `.cpp` files — `#if SFX_PLATFORM_ESP32` / `#if !SFX_PLATFORM_ESP32` to prevent compilation on wrong target
6. **`using` alias** — consumers use `FooServer` (the alias), never `FooServerT<Esp32FooPolicy>` directly

**Current policy-based templates:**

| Template | Policies | Location |
|----------|----------|----------|
| `StorageServerT<TPolicy>` | `Esp32StoragePolicy`, `PicoStoragePolicy` | `lib/sfx_storage/storage/` |
| `SdCardModuleT<TPolicy>` | `PicoSpiSdPolicy`, `EspSpiSdPolicy`, `EspSdio1BitPolicy`, `EspSdio4BitPolicy` | `lib/sfx_storage/storage/` |

**Current pure arch implementations:**

| Alias | Concrete Classes | Location |
|-------|-----------------|----------|
| `UsbHost` | `EspUsbHost`, `PicoUsbHost` | `lib/sfx_usb/usb/` (`esp32/`, `pico/`) |

**Pure arch implementation pattern:**

When a class has large, fundamentally different implementations per platform but minimal shared logic, use standalone concrete classes with a `using` alias instead of policy templates:

```cpp
// ---- Shared state struct (device tracking, stats, callbacks) ----
struct FooState {
    bool initialized = false;
    int deviceCount = 0;
    // ...helper methods for common device tracking logic
    int findDevice(uint8_t addr) const;
};

// ---- Main header: types + auto-select ----
// sfx_foo.h — defines FooState, types, then:
#if SFX_PLATFORM_ESP32
#include "esp32/esp_foo.h"
using Foo = EspFoo;
#elif SFX_PLATFORM_PICO
#include "pico/pico_foo.h"
using Foo = PicoFoo;
#endif

// ---- Platform implementation (standalone, no inheritance) ----
class EspFoo {
public:
    static EspFoo& instance() { static EspFoo inst; return inst; }
    // Delete copy/move...
    bool begin();
    void process();
    // Same interface as PicoFoo — no virtual, no ABC
private:
    EspFoo() = default;
    FooState _state;  // Shared state via composition
    // ...platform-specific members
};
```

**Rules for pure arch:**
1. **No base class** — each platform class is fully standalone with identical public interface
2. **`FooState` struct** for shared logic — helper methods on the struct, composed via `_state` member
3. **Platform headers included FROM main header** — they see all types defined above the include point. Add "Do not include directly" comment.
4. **`using` alias** — consumers use `Foo` (the alias), never `EspFoo` directly
5. **No forward declarations of the alias** — `class Foo;` is illegal when `Foo` is a `using` alias. Use `#include` instead.
6. **C callback bridges** use `EspFoo::instance()` directly — no `static_cast` needed since there's no ABC

**Current template-parameterized classes:**

| Template | Parameter | Default | Location |
|----------|-----------|---------|----------|
| `UsbRegistryT<MaxSlaves>` | Max slave count | `4` | `lib/sfx_usb/usb/usb_registry.h` |

## Key Architecture Patterns

### Client-Server Topology
```
HubFX ESP32-S3 (Client) - USB Host
  ├─ USB Port 0 → GunFX Pico (Server)
  ├─ USB Port 1 → LightFX Pico (Server)
  ├─ USB Port 2 → GearControl Pico (Server)
  └─ USB Port N → Other Servers

HubFX Pico (OBSOLETE) - RP2350, frozen reference only
```

### Handler Registration (CRITICAL)

`SfxServer` automatically registers `coreServer` before the module handler. All controllers MUST use `SfxServer.addModuleHandler()` which guarantees correct handler priority.

```cpp
// CORRECT - SfxServer handles registration order
SfxServer server;
server.begin("XxxFX", FIRMWARE_VERSION, BUILD_NUMBER);
server.addModuleHandler(&xxxfxServer);  // Core added automatically first

// WRONG - manual setup without SfxServer (deprecated pattern)
commandRouter.addHandler(&xxxfxServer);  // ← Missing coreServer!
```

### Shared Libraries (`controllers/lib/`)
Reusable hardware drivers and protocol library split by domain:
- **sfx_platform/** - Cross-platform abstraction (mutexes, delays, GPIO, SfxWire encoding, DiagLog)
- **sfx_serial/** - Binary COBS protocol, command handlers, server/client classes
- **sfx_server/** - Common server controller boilerplate (SfxServer, indicators, connection management)
- **sfx_peripherals/** - Hardware drivers (LED, servo, PWM input, I2C, INA226 power monitor)
- **sfx_audio/** - 8-channel WAV mixer, I2S output, codec drivers (TAS5825M, SimpleI2S), mock I2S sink
- **sfx_storage/** - SD card (SdFat/ESP SD), LittleFS flash singletons, shared storage types
- **sfx_config/** - YAML config parser, templatized config store, CONFIG_RELOAD/GET protocol server/client
- **sfx_usb/** - USB Host abstraction (PicoUsbHost, EspUsbHost), device registry

### Serial Protocol Library (`controllers/lib/sfx_serial/serial/`)
- **serial.h** - Umbrella include (use this)
- **core/core.h** - CoreProtocol, SerialError, CommandResult, ICommandHandler, CommandRouter, SFX_* macros
- **core/bus_server.h** - BusServer base class + CoreCommandServer (server side)
- **core/stream.h** - StreamProtocol constants (0xA4-0xA6) + StreamWriter (chunked data streaming with CRC-16)
- **client/bus.h** - SerialBus (client-only, COBS over USB CDC)
- **client/bus_client.h** - BusClient base class (client side, extends SerialBus)
- **client/result_queue.h** - ResultQueue (tag-correlated command/response matching)
- **gunfx/gunfx.h** - GunFxServer, GunFxClient, GunFxPacket, GunFxError, GunFxSpec
- **lightfx/lightfx.h** - LightFxServer, LightFxClient, LightFxPacket, LightFxError, LightFxSpec
- **gearcontrol/gearcontrol.h** - GearControlServer, GearControlClient, GearControlPacket, GearControlError, GearControlSpec
- **hubfx/hubfx.h** - HubFxAudioServer, HubFxAudioClient, HubFxStorageServer, HubFxStorageClient, HubFxPacket, HubFxError (NOT auto-included by serial.h — heavy deps)

Include order: `#include <serial/serial.h>` (includes everything needed, except hubfx.h)

### Server Handler Macros (core/core.h)
Reduce boilerplate in `handleModulePacket()` switch cases:
```cpp
SFX_REQUIRE_LEN(n)                    // NACK MISSING_PARAMETER if len < n
SFX_VALIDATE(cond, err)               // NACK err if !cond
SFX_DISPATCH(callback, args...)       // Call callback, ACK/NACK on result
SFX_HANDLE_CHANNEL_CMD(v, err, cb)    // Validate + dispatch single-param cmd
```

### Rich STATUS Pattern

Every controller provides board-specific status via `SfxServer`:

```cpp
// In setup(): Register module status callback
server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
    buf[0] = myFlag;
    CoreProtocol::putU16LE(&buf[1], myServo);
    return 3;  // bytes written
});

// Free RAM is updated automatically by server.loop()
```

STATUS response = 20-byte core header `[counter:u32][uptime:u32][freeRam:u32][lastActivity_ms:u32][keepaliveCount:u32]` + module callback data.

INIT_READY payload = length-prefixed binary: `[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]`

IDENTIFY (0xFE) returns the same payload as INIT_READY but without triggering init callbacks or state changes. The CLI uses IDENTIFY on connect to discover the board type:
- **HubFX** (auto-initializes on boot): IDENTIFY only — no INIT sent
- **Slave controllers**: IDENTIFY to detect type, then INIT to activate hardware
- **Fallback**: if IDENTIFY fails, CLI falls back to INIT (for legacy firmware)

See `controllers/lib/sfx_serial/serial/PROTOCOL.md` for full wire format.

### Python Test Framework (`tests/`)
```
framework/
  ├── connection.py    - ScaleFXConnection class
  ├── protocol.py      - COBS encode/decode, CRC-8, packet helpers
  ├── packets.py       - Constants (MUST mirror C++ headers)
  └── commands.py      - High-level command builders (e.g., GunFxCommands.trigger_on())
cli/
  ├── base.py          - CommandHandlerBase (_send_ack, _wrap_packet), CommandInfo, OutputMixin
  ├── output.py        - TerminalUI split-screen terminal (prompt_toolkit Application)
  ├── parsers.py       - Response packet parsing utilities
  ├── interactive.py   - Main CLI class (prompt_toolkit split-screen, async-safe output)
  └── handlers/
      ├── core.py      - Core/protocol commands (connect, init, status)
      ├── gunfx.py     - GunFX commands (trigger, servo, smoke)
      ├── lightfx.py   - LightFX commands (led, servo, power, sequences)
      ├── gearcontrol.py - GearControl commands (gear, servo, yaw, calibration)
      ├── hubfx.py     - HubFX hub commands + composed slave routing
      └── storage.py   - Reusable file operations (SD/Flash)
{controller}/
  └── test_*.py        - pytest test files (requires hardware)
```

**CLI Composition Pattern:** HubFX handler composes instances of GunFX, LightFX, and GearControl
handlers with `_packet_wrapper = HubFxCommands.slave_route` for transparent hub routing. All
ACK-based commands in direct handlers use `_send_ack()` which applies the wrapper when set.
Query commands are excluded from slave registry since SLAVE_ROUTE only forwards ACK/NACK.

**Dependencies:** `pyserial`, `colorama`, `prompt_toolkit>=3.0.0` (see `tests/requirements.txt`)

## Packet Type Allocation

| Range | Module | Status | Notes |
|-------|--------|--------|-------|
| 0x01-0x2F | GunFX | Used | Trigger, servo, smoke |
| 0x30-0x3F | Reserved | - | Future expansion |
| 0x40-0x5F | LightFX | Used | LED, servo, power |
| 0x60-0x7F | GearControl | Used | Gear, servo, yaw |
| 0x80-0xAF | HubFX | Used | Slaves, audio, engine, config (0x90-0x92, 0xAC), SD, flash, files, USB diag, USB reset, tree, slave info |
| 0xA4-0xA6 | Streaming | Used | STREAM_BEGIN/DATA/END (`core/stream.h`) |
| 0xB0-0xEF | Available | Free | New controllers |
| 0xF0-0xFF | Core | Reserved | INIT, ACK, NACK, REBOOT, IDENTIFY (0xFE), LOG_MESSAGE (0xFD), DIAG_HISTORY (0xFF) |

## Platform-Specific Notes

### Pico Controllers (C++17, Arduino)
- Build with PlatformIO: `python -m platformio run`
- Framework: Arduino-Pico (Earle Philhower core)
- Key libraries: Servo, Wire (I2C), SD (FatFS), USB Host (PIO-USB for HubFX)
- BOOTSEL mode: Send `BOOTSEL` command via serial for firmware updates

### Windows Studio (C# 12, .NET 8)
- Build: `dotnet build -c Release` in `app/win32/ScaleFXStudio/`
- Framework: Windows Forms
- Purpose: Visual configuration editor
- Partial classes: MainForm split across multiple files (Fields, EngineFxTab, GunFxTab, etc.)

## Common Workflows

See the detailed workflow guides in `/instructions/`:
- **Adding a command:** `03-PROTOCOL-EXTENSION.md` + `04-CHANGE-PROPAGATION.md`
- **Creating a controller:** `02-NEW-CONTROLLER.md`
- **Building/flashing:** `05-BUILD-AND-FLASH.md`
- **Writing tests:** `06-TEST-SUITE.md`
- **Updating CLI:** `07-CLI-UPDATES.md`
- **AudioTools library:** `08-AUDIOTOOLS.md`
- **System architecture:** `01-ARCHITECTURE.md`
