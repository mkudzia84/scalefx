# ScaleFX Development Guide

> **AI AGENTS:** This is a multi-platform embedded effects system. Read `/instructions/README.md` first for comprehensive guidance.

## System Architecture

ScaleFX is a modular scale model effects system with two platform targets:
- **Pico Controllers** (RP2040): Real-time device control (GunFX, LightFX, GearControl)
- **ESP32-S3 Controller**: HubFX ESP32-S3 (master hub, active development)

> **HubFX Pico (RP2350) is OBSOLETE.** The Pico variant (`controllers/hubfx/pico/`) is frozen as a reference implementation. All new HubFX development (features, bug fixes, protocol additions) MUST target `controllers/hubfx/esp32s3/`. See Rule 17.

**Communication:** Binary COBS protocol over USB serial (6Mbps baud)
- Packet format: `[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]`
- CRC-8 polynomial: 0x07
- Endianness: Little-endian for ALL multi-byte values

## Quick Command Reference

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

# Build Windows Studio (Wails v2 + Svelte)
cd app/go/studio && wails build

# Interactive CLI (Go)
app/go/scalefx-cli.exe -p COM5
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

| C++ Source | Go CLI Mirror | Content |
|------------|---------------|---------|
| `controllers/lib/sfx_serial/serial/core/core.h` | `app/go/protocol/core/core.go` | Packet type constants, generic error codes |
| `controllers/lib/sfx_serial/serial/gunfx/gunfx.h` | `app/go/protocol/gunfx/gunfx.go` | GunFX packet types, error codes, commands |
| `controllers/lib/sfx_serial/serial/lightfx/lightfx.h` | `app/go/protocol/lightfx/lightfx.go` | LightFX packet types, error codes, commands |
| `controllers/lib/sfx_serial/serial/gearcontrol/gearcontrol.h` | `app/go/protocol/gearcontrol/gearcontrol.go` | GearControl packet types, error codes, commands |
| `controllers/lib/sfx_serial/serial/hubfx/hubfx.h` | `app/go/protocol/hubfx/hubfx.go` | HubFX packet types, error codes, commands |

**Verification:** Run `cd app/go && go build ./cli/` after C++ changes.

### 2. Command Addition Checklist

When adding a new command to an existing controller, update ALL these files:
1. `xxxfx/xxxfx.h` - Add packet type constant, callback typedef, `onNewCmd()` method, handler case in `handleModulePacket()` using `SFX_*` macros, error codes if needed
2. `xxxfx_pico.ino` - Implement callback, register in `setup()`
3. `controllers/xxxfx/pico/README.md` - Document payload format
4. `app/go/protocol/xxxfx/xxxfx.go` - Mirror packet constant, add command builder, register in `init()`
5. `app/go/api/xxxfx.go` - Add typed API method
6. `app/go/engine/handlers/xxxfx/handler.go` - Add CLI command and register in command list
7. `app/go/engine/handlers/xxxfx/parsers.go` - Add response parser if command returns data

**ALWAYS update the Go CLI when new commands are added.** The Go CLI is the primary debugging tool.

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

**Go payload building:**
```go
binary.LittleEndian.PutUint16(buf, value)  // Little-endian
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

Each module's error codes MUST be in its assigned range and defined in both C++ and Go:

| Range | Module | C++ Namespace | Go Constants |
|-------|--------|---------------|-------------|
| `0x00-0x0F` | Generic | `SerialError` | `Err*` |
| `0x10-0x1F` | Parameter | `SerialError` | `Err*` |
| `0x20-0x4F` | GunFX | `GunFxError` | `ErrGunFx*` |
| `0x50-0x5F` | LightFX | `LightFxError` | `ErrLightFx*` |
| `0x60-0x6F` | GearControl | `GearControlError` | `ErrGearControl*` |
| `0x70-0x8F` | Reserved | — | — |
| `0xF0-0xFF` | System | `SerialError` | `Err*` |

**Error code rules:**
1. Every C++ error constant MUST have a matching Go constant with the same value
2. Never define error codes outside the module's assigned range
3. Remove unused/dead error codes — they cause sync confusion
4. Each error namespace MUST have a `getMessage()` (C++) / `PacketTypeName()` (Go) function
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
| `SdCardModule` | `lib/sfx_storage/storage/sd_card.h` | Single SPI SD card |
| `FlashModule` | `lib/sfx_storage/storage/flash.h` | Single onboard LittleFS flash |
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
| `_writerActive` | `std::atomic<bool>` | Esp32StoragePolicy | Core 1 writes | release/acquire |
| `_writerError` | `std::atomic<bool>` | Esp32StoragePolicy | Core 1 writes | release/acquire |
| `_writerDone` | `std::atomic<bool>` | Esp32StoragePolicy | Core 1 writes | release/acquire |
| `_drainRequested` | `std::atomic<bool>` | Esp32StoragePolicy | Core 0 writes | release/acquire |
| `_writerBytesWritten` | `std::atomic<uint32_t>` | Esp32StoragePolicy | Core 1 writes | relaxed/acquire |
| `_writerWriteCount` | `std::atomic<uint32_t>` | Esp32StoragePolicy | Core 1 writes | relaxed/acquire |
| `_writerMaxLatency_ms` | `std::atomic<uint32_t>` | Esp32StoragePolicy | Core 1 writes | relaxed/acquire |
| `_writerTotalStall_ms` | `std::atomic<uint32_t>` | Esp32StoragePolicy | Core 1 writes | relaxed/acquire |

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
| `StorageServerT<TPolicy>` | `Esp32StoragePolicy`, `PicoStoragePolicy` | `lib/sfx_storage/server/` |
| `SdCardModuleT<TPolicy>` | `PicoSpiSdPolicy`, `EspSpiSdPolicy`, `EspSdio1BitPolicy`, `EspSdio4BitPolicy` | `lib/sfx_storage/storage/` |
| `BatteryServerT<TBattery>` | `AdcDividerBatteryT<...>`, `Ina226Battery` | `lib/sfx_peripherals/power/` |
| `ConfigStore<TSchema, TPool>` | per-board schemas (`LightFxConfigSchema`, ...) | `lib/sfx_config/config/` |

**Concepts replace doc-comment contracts (C++20).** The codebase compiles with `-std=gnu++20` on every board. Every policy / schema parameter listed above has a matching `concept` (`StoragePolicy`, `BatteryPolicy`, `ConfigSchema`, `GpioExpander`, `HwPwmExpander`, `LedBrightnessExpander`) attached to its template via a `requires`-clause. Adding a new policy that misses or mistypes a method now fails at the `using` alias / construction site with the missing method named, instead of as a link-time error from inside a controller's `.ino` file. **When you add a new policy-based template, define its concept in the same header** (right above the template) and gate the template with `requires`.

**One slave per type — no router/orchestrator:**

HubFX accepts **at most one slave per `SlaveType`** (one GunFX, one LightFX, one GearControl). That makes the prior applier/router/orchestrator/policy stack (~990 LOC across `sfx_boards/applier/` + `sfx_boards/lightfx/applier/`) overengineered. For hub-side config push, write a **plain function** per (board, host) pair — e.g. `bool pushLightFxConfigToSlave(const LightProgramConfig&, LightFxClient&)` in `controllers/hubfx/esp32s3/src/protocol/hub_lightfx_apply.cpp` — that walks the config and calls typed-client methods directly. Register it from `<board>Config.onLoaded(...)` and from `UsbRegistry::onReady(SlaveType::<Board>, ...)`. On the standalone slave side, the same config struct applies via a plain `applyLightProgramConfigLocal(cfg)` function that drives local peripherals (no applier class). Deleted in HubFX 0.38.0 / LightFX 0.13.0 — do not reintroduce.

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

### 19. Cross-Platform Protocol Sync (MANDATORY)

**When protocol is changed or a new controller/board is added, ALWAYS reflect those changes in the Go CLI** (`app/go/`). The Go CLI is the single client implementation of the protocol — it MUST stay in sync with C++ headers.

**Key files:**

| File | Purpose |
|------|---------|
| `protocol/<module>/<module>.go` | Packet type constants, error codes, command builders (mirrors C++ headers) |
| `api/<module>.go` | Typed API methods (wraps protocol commands) |
| `engine/handlers/<module>/handler.go` | CLI command handlers |
| `engine/handlers/<module>/parsers.go` | Response payload parsers |
| `engine/parsers*.go` | Shared/core response parsers |

**Rules:**
1. **New packet type constant** → add to `protocol/<module>/<module>.go` + register in `init()`
2. **New error code** → add to `protocol/<module>/<module>.go` + register in `init()`
3. **New command** → add builder to `protocol/<module>/<module>.go`; add API method to `api/<module>.go`; add CLI handler to `engine/handlers/<module>/handler.go`
4. **New response parser** → add to `engine/handlers/<module>/parsers.go`
5. **New controller type** → create handler package under `engine/handlers/`; register in `engine/handlers/handlers.go`
6. **Payload format change** → update all parsers and command builders

**Verification:**
```bash
cd app/go && go build ./cli/
```

#### 19a. Decoded Event Types Belong in `engine/handlers/<mod>/` (MANDATORY)

**Async packet decoders live exactly once — in the per-module handler package — and are consumed by BOTH the CLI (console formatting) and Wails Studio (typed Go listeners → frontend events).** Studio must never re-decode a payload locally in `studio/app.go`, and `cli/*` must never re-decode either.

**Structure per module** (`app/go/engine/handlers/<mod>/` — four files, strict roles):

| File | Purpose |
|------|---------|
| `types.go` | JSON-tagged decoded structs + pure `Decode*(payload []byte) *T` functions. No I/O, no formatting. Labels (`ErrorReasonName`, `PhaseName`, …) populated from the authoritative `protocol/<mod>` name tables. |
| `format.go` | `Handler.FormatXxx(*Xxx)` methods that render a decoded struct to the CLI (`h.E.Out`). No decoding. Every formatter is a pure struct→console function. |
| `handler.go` | `Handler` struct with `engine.Observers[T]` listener fields (never raw `func(*T)`). `Register(eng *Engine) *Handler` wires the status/async packets using **inline closures** — no `parseXxx`/`handleXxx` wrapper methods. |
| `parsers.go` | **CLI-only query-response renderers** (reply to `seq.status`, `slaves`, `audio.status`, etc.). These are not broadcast/async paths — those go through the observer chain. |

**The Observer listener pattern** — every Handler field that fires per-event is an `engine.Observers[T]`, a multicast slot with `Add(fn)`, `Fire(v)`, `Len()`, and `Dispatch(out, label, payload, decode)`. Studio subscribes with `.Add(...)`; the CLI subscribes by pre-seeding its formatter in `Register()` (for async events) or by rendering inline in the status closure. Broadcast paths are silent when `Len() == 0` — this keeps the 1 Hz STATUS_BROADCAST from flooding the CLI.

**Inline-closure registration** — Register() wires everything directly, no thin methods:

```go
func Register(eng *engine.Engine) *Handler {
    h := &Handler{E: eng}
    // Sync-status path: CLI prints always (triggered by user `status` command).
    eng.RegisterStatusParser(pcore.CtrlXxx, func(data []byte) {
        if s := DecodeStatusBroadcast(data); s != nil { h.FormatStatusBroadcast(s) }
    })
    // Broadcast path: silent unless Studio has subscribed.
    eng.RegisterStatusBroadcastParser(pcore.CtrlXxx, func(data []byte) {
        if h.OnStatusBroadcast.Len() == 0 { return }
        if s := DecodeStatusBroadcast(data); s != nil { h.OnStatusBroadcast.Fire(s) }
    })
    // Async path: CLI formatter pre-seeded as an observer (decoupled, same chain).
    h.OnCalibStatus.Add(h.FormatCalibStatus)
    eng.RegisterAsyncHandler(xxxp.CalibStatus, func(p []byte) {
        h.OnCalibStatus.Dispatch(h.E.Out, "CalibStatus", p, DecodeCalibStatus)
    })
    eng.AddGroup(h.commands())
    return h
}
```

**Discovery:** `handlers.RegisterDefaults(eng *engine.Engine) *handlers.Registry` returns a struct exposing each `*Handler`. Studio captures the registry in `NewApp` and adds listeners in `startup()`:

```go
a.reg.GearControl.OnCalibStatus.Add(func(c *gearcontrol.CalibStatus) {
    wailsRT.EventsEmit(a.ctx, "gearcontrol:calib", c)
})
```

**Rules:**
1. **New async packet** → struct + `Decode*` in `types.go`; `FormatXxx` method in `format.go`; `engine.Observers[T]` field on `Handler`; inline-closure registration in `Register()`.
2. **Studio emits Wails events by serializing the decoded struct directly** (`EventsEmit(ctx, "<mod>:<event>", decoded)`). The Svelte TS interface mirrors the struct's JSON tags.
3. **Never** add a local `type FooUpdate struct {...}` in `studio/app.go` shadowing a handler type; never add `encoding/binary` reads in `studio/app.go`. If you feel the urge, the missing piece belongs in `engine/handlers/<mod>/types.go`.
4. Authoritative labels (error-reason, phase, state) come from `protocol/<mod>` name tables — mirror them on the frontend by reading `*.Name` fields from the event payload, not by re-mapping codes locally.
5. **No thin wrappers.** If a method only decodes + fires / only forwards to another function, inline it at the call site. `parseXxxStatus` / `handleXxxBroadcast` wrappers around a single decode+Fire are banned — use the closure form above.
6. **No backward-compatibility scaffolding during refactors.** When restructuring, delete dead fields, removed flags, and "pre-vN" fallbacks outright. Rule 11 (append-only wire extension via `len()` checks) still applies — that's protocol compat across firmware versions, not code compat across git revisions. Do not invent `XxxFieldPresent` booleans, keep deleted helpers as thin wrappers, or leave `// removed for back-compat` comments.

**Anti-pattern (do not do this):**
```go
// BAD — studio/app.go re-decoding a packet the handler already decodes
func (a *App) handleStatusBroadcast(src string, data []byte) {
    counter := binary.LittleEndian.Uint32(data[0:4])  // duplicates engine/handlers/<mod>/types.go
    ...
}

// BAD — thin wrapper that only calls decode + fire
func (h *Handler) parseCalibStatus(p []byte) {
    if c := DecodeCalibStatus(p); c != nil { h.OnCalibStatus.Fire(c) }
}
eng.RegisterAsyncHandler(xxxp.CalibStatus, h.parseCalibStatus)  // prefer an inline closure
```

### 20. Use VS Code Tasks for Building and Flashing (MANDATORY)

**The workspace defines predefined VS Code tasks in `.vscode/tasks.json` for all build, flash, and verification operations.** AI agents MUST use these tasks via `create_and_run_task` instead of running raw terminal commands with `run_in_terminal`.

**Why:** Tasks use the correct working directory, environment, and parameterization (e.g., controller picker). Running raw `platformio` commands in `run_in_terminal` is fragile — wrong cwd, wrong terminal reuse, wrong shell quoting.

**Available tasks:**

| Task Label | Purpose | Group |
|------------|--------|-------|
| `Build Firmware` | Build any controller (prompts for controller) | build (default) |
| `Build and Flash Firmware` | Build + flash + verify (prompts for controller) | build |
| `Flash Firmware (no build)` | Flash only, skip build | build |
| `Build All Controllers` | Build all 6 firmware targets sequentially | build |
| `Build Go CLI` | Build the Go CLI binary (`app/go/cli/`) | build |
| `Build Flash CLI` | Build the Flash CLI binary (`app/go/flash/`) | build |
| `Build ScaleFX Studio (GUI)` | Build the Wails v2 Studio app | build |
| `Run ScaleFX Studio (GUI)` | Launch Wails dev server | test |
| `Interactive CLI (Go)` | Launch Go CLI session (prompts for COM port) | test |
| `Flash CLI (Interactive)` | Launch Flash CLI session | test |
| `Publish Firmware Release` | Dispatch GitHub Actions release workflow | build |
| `List Firmware Releases` | List recent GitHub releases | test |

**Rules:**
1. **Always use `create_and_run_task`** for build/flash/syntax-check operations — never `run_in_terminal` with raw `platformio` commands
2. **Reference existing tasks by label** — do NOT create ad-hoc tasks with hardcoded controller names (e.g., "Build HubFX"). Use the parameterized tasks ("Build Firmware", "Build and Flash Firmware") which prompt for controller selection. Ad-hoc tasks pollute `tasks.json`.
3. **No comments in tasks.json** — the file MUST be valid JSON (not JSONC). The `create_and_run_task` tool cannot parse JSON with comments.
4. **Task labels are stable** — reference them by exact label string
5. **Controller selection** — tasks using `${input:controller}` will prompt the user; for non-interactive use, create a task with the controller hardcoded in the command

**Anti-pattern:**
```bash
# BAD: Raw terminal command — fragile, wrong cwd, bypasses task config
run_in_terminal: app/go/scalefx-flash.exe build hubfx

# GOOD: Use the predefined task
create_and_run_task: "Build Firmware"  # prompts for controller
```

### 21. Test and Diagnostic Tool Location (MANDATORY)

**All standalone test projects MUST reside in the `/tests/` directory at the repository root.** This includes both PlatformIO firmware tests AND Go-based diagnostic/troubleshooting tools. Tests are NOT part of the production controller tree (`controllers/`) or production CLI tree (`app/go/`).

#### Firmware Tests (PlatformIO)

**Rules:**
1. **Location:** `tests/<test_name>/` — each test is a self-contained PlatformIO project
2. **Structure:** Each test directory contains `platformio.ini`, `partitions.csv` (if needed), and `src/<test_name>.ino`
3. **No sfx libraries:** Test firmware should use `lib_ignore` to exclude all `sfx_*` libraries unless the test specifically validates a library
4. **Serial output:** Use 115200 baud for human-readable serial monitor output (not 6Mbps binary COBS)
5. **Never under `controllers/`:** Do not create `test/` subdirectories inside controller firmware folders

**Build:** `python -m platformio run -e <env> -d tests/<test_name>`

#### Go Diagnostic Tools

**Rules:**
1. **Location:** `tests/<tool_name>/` — each tool is a self-contained Go module
2. **Structure:** Each tool directory contains `go.mod`, `main.go`, and optionally a `README.md`
3. **Module dependency:** Use a `replace` directive in `go.mod` to reference the main `scalefx` module:
   ```
   replace scalefx => ../../app/go
   ```
4. **Self-contained:** Each tool is a standalone `main` package — it builds to its own binary
5. **Reuse protocol/api packages:** Import `scalefx/protocol`, `scalefx/api`, etc. from the main module — do NOT duplicate wire format or protocol code
6. **Focused purpose:** Each tool should diagnose one specific area (USB, audio, storage, etc.)
7. **Clear output:** Use colored terminal output with diagnostic hints for common issues
8. **No production dependencies:** Production code (`app/go/`, `controllers/`) MUST NOT import from `tests/`
9. **Build:** `cd tests/<tool_name> && go build .`

**Test layout:**

```
tests/
├── hw/                  Firmware test fixtures — flash to a real board
│   ├── gearcontrol_hwtest/   GearControl bring-up sweep (servos, motors, gear input)
│   ├── gunfx_hwtest/         GunFX bring-up sweep (servos, smoke, RC trigger, INA226)
│   ├── led_blink/            LED-channel blink (PCAL6416A I2C expander, ESP32-S3)
│   ├── noop_simple/          Minimal no-op firmware (ESP32-S3)
│   ├── ppm_test/             PPM signal decoder test
│   ├── sfx_test_p/           TAS5825P sine-wave bring-up (original HubFX silicon)
│   ├── sfx_test_m/           TAS5825M sine-wave bring-up (current HubFX silicon)
│   └── storage_test/         Storage upload throughput / hang harness
├── host/                Go programs that run on the dev machine
│   ├── handler_test/         Unit tests for engine handlers
│   ├── protocol_test/        Unit tests for COBS / CRC / framing
│   ├── upload_test/          Integration test driving real HW upload
│   ├── storage_test_client/  Driver for tests/hw/storage_test
│   └── usb_diag/             USB host & slave detection diagnostics
└── fixtures/            Test data
    └── upload_fixtures/      Generated SD images + small flash payloads
```

(The `virtual_board/` TCP simulator + its `tcp://` transport, the
`virtualdiscovery` package, and the Studio/CLI manual-port connect option
were removed 2026-05-22 — Studio and the CLI talk to real serial hardware
only.)

| Group | What it is | When to use |
|-------|-----------|-------------|
| `tests/hw/` | PlatformIO firmware projects | Bench bring-up of a freshly populated PCB; reproducing analog/timing issues that need real silicon |
| `tests/host/` | Go programs | Unit tests, protocol fuzzing, USB diagnostics, driving HW tests from the desktop |
| `tests/fixtures/` | Test data | Inputs for upload/storage tests |

### 22. Release Notes (MANDATORY)

**Every firmware release MUST include meaningful release notes.** The GitHub Actions release workflow requires a `release_notes` input. The agent MUST generate release notes when publishing a release.

#### Agent Release Notes Generation

When the user asks to publish/release firmware, the agent MUST:
1. **Determine the previous release version** for that controller (check the controller's README.md version history or the `FIRMWARE_VERSION` define history)
2. **Collect all changes since the last release** by reviewing:
   - Git log for the controller directory (`controllers/{name}/`)
   - Git log for shared libraries (`controllers/lib/`) that affect the controller
   - Changes to protocol headers (`serial/{module}/{module}.h`)
3. **Categorize changes** using these sections:
   - **New Features** — new commands, new hardware support, new configuration options
   - **Bug Fixes** — corrected behavior, crash fixes, edge case handling
   - **Protocol Changes** — new packet types, payload format changes, new error codes
   - **Breaking Changes** — anything that requires client updates (mark with ⚠️)
   - **Internal** — refactors, code cleanup, documentation (omit if nothing user-facing)
4. **Format as markdown** suitable for the GitHub release body
5. **Include version impact reasoning** — explain why this is MAJOR/MINOR/PATCH

#### Release Notes Format

```markdown
### New Features
- Added `SERVO_SPEED` command for configurable servo movement speed
- Support for INA226 power monitoring on channels 0-3

### Bug Fixes
- Fixed gear deploy timeout not resetting after manual override
- Fixed LED sequence loop count off-by-one

### Protocol Changes
- New packet type: `SERVO_SPEED` (0x0F) — 3-byte payload [channel:u8][speed:u16LE]
- New error code: `ERR_SERVO_MOVING` (0x28)

### Breaking Changes
- ⚠️ STATUS payload extended from 8 to 12 bytes (added servo position fields)
```

#### Where Release Notes Appear

| Consumer | How | Source |
|----------|-----|--------|
| **GitHub Releases** | Embedded in release body | Workflow `release_notes` input |
| **Go CLI** | `fw.notes <controller> [version]` | `Release.Body` from GitHub API |
| **Go CLI** | `fw.releases` shows one-line summary | First content line of Body |
| **ScaleFX Studio** | Collapsible panel in Firmware tab | `ReleaseInfo.body` via `GetReleases()` |

**Rules:**
1. **Never publish a release with empty notes** — the workflow input is required
2. **Be specific** — "fixed bug" is not acceptable; "fixed gear deploy timeout not resetting after ERR_STALL" is
3. **Reference packet types by name and hex value** for protocol changes
4. **Mark breaking changes prominently** with ⚠️ and explain the migration path
5. **Keep notes concise** — 5-15 bullet points typical for a minor release

### 23. Studio Config Validation Framework (MANDATORY for board UIs)

**Every board tab in `app/go/studio/frontend/src/lib/tabs/<Board>Tab.svelte` MUST run its config through a board-specific verifier and visually surface issues in the UI.**

This is the standard pattern for catching invalid wiring (pin role conflicts, disabled-channel references, range violations, missing calibration) **before** the user pushes a bad config to flash.

**Reference implementation:**
- Generic framework: [app/go/studio/frontend/src/lib/config/config-verifier.ts](app/go/studio/frontend/src/lib/config/config-verifier.ts) — `ConfigVerifier<T>` interface, `ResourceTracker`, `Range` helpers, `buildResult`, `EMPTY_RESULT`.
- LightFX board verifier: [app/go/studio/frontend/src/lib/config/light-verifier.ts](app/go/studio/frontend/src/lib/config/light-verifier.ts).
- GearControl board verifier: [app/go/studio/frontend/src/lib/config/gearcontrol-verifier.ts](app/go/studio/frontend/src/lib/config/gearcontrol-verifier.ts).

**Required wiring per board tab:**

1. **Config snapshot interface** — define a board-local interface (e.g. `GearControlConfig`) that mirrors the in-component reactive state. The verifier operates on this snapshot, not on Svelte state directly, so it stays test-friendly and decoupled from the UI framework.

2. **Verifier class** — `class <Board>ConfigVerifier implements ConfigVerifier<<Board>Config>`. Hold a `_pathIndex: Map<string, VerifyIssue[]>` rebuilt every `verify()` call so `hasConflict(path)` and `severityForPath(path)` are O(1) lookups for template re-renders.

3. **Path scheme** — issues carry a stable `path` string (e.g. `pins[3]`, `gears[1].door.0`, `yaw`). Use the same path on the issue and on the template `class:verify-error={sev('pins[3]') === 'error'}` binding.

4. **Reactive verification block** in the tab `<script>`:
   ```svelte
   const verifier = new <Board>ConfigVerifier()
   let liveResult: VerifyResult = EMPTY_RESULT
   $: {
       void <each_reactive_dep>  // touch every dependency
       liveResult = verifier.verify(buildConfig())
   }
   let sev: (path: string) => string | null
   $: sev = (() => { void liveResult; return p => verifier.severityForPath(p) })()
   ```

5. **Visual highlight** — bind `class:verify-error={sev(path) === 'error'}` and `class:verify-warn={sev(path) === 'warning'}` on every UI element whose path can carry an issue (rows, sections, cards, inputs). The shared CSS rules (in each tab `<style>` block, copied from LightFXTab):
   ```css
   .verify-error { border-color: var(--error) !important; background: color-mix(...); box-shadow: ...; }
   .verify-warn  { border-color: var(--warning) !important; box-shadow: ...; }
   ```

6. **Save dialog gate** — replace any direct `SendCommand('config.save')` button with one that opens `<SaveConfigDialog verifyResult={...} />`. The dialog blocks save when `verifyResult.counts.error > 0` and shows all warnings/info inline. Title-bar badge (e.g. `{count} ✕` / `{count} ⚠`) gives a persistent tab-level summary.

**Why this matters:**
- Catches the most common operator mistakes (door bound to disabled channel, yaw min ≥ max, pin role duplicated) at edit time, not after a flash.
- Single source of truth — board verifiers are pure TypeScript, easy to unit test independently of Svelte.
- Consistent UX across boards — every tab shows red borders the same way, so users don't have to learn a new error idiom per board.

**When adding a new board tab:** create `<board>-verifier.ts` BEFORE wiring up the save button. Adding it later means going back through every reactive form field to add `class:verify-*` bindings — much easier to do alongside the initial layout.

### 24. Tooltips, Live Validation & Live Push for Studio Settings (MANDATORY for board UIs)

**Every interactive setting in `app/go/studio/frontend/src/lib/` (tabs, dialogs, shared widgets) MUST have a `title="…"` tooltip describing what the parameter does, its units, and its valid range. Every config field MUST validate locally on change and — when validation passes — debounce-push the new value to the board immediately.**

This guarantees that an operator can hover any control to learn what it does without consulting documentation, and that they get instant visual feedback on bad values rather than waiting for a Save+Flash cycle to discover them.

**Tooltip rules:**

1. **Coverage** — every `<label>`, `<input>`, `<select>`, `<button>`, and labelled toggle in a tab/dialog/shared widget gets a `title=` attribute. Section headers and read-only displays may also benefit when their meaning isn't obvious.
2. **Content** — start with a short verb-led description, then state units and ranges in parentheses. Examples:
   - `title="Minimum servo pulse width (300–2700µs)"`
   - `title="Travel speed limit. 0 = instant motion (no rate limiting). Range: 0–65535 µs/s."`
   - `title="When enabled, Open and Close commands swap travel direction (Open→min, Close→max)."`
3. **Match the actual behavior** — when a parameter's semantics change (e.g. units swap, range tightens), update the tooltip in the same commit. Stale tooltips are worse than no tooltips.
4. **Avoid duplicating the visible label.** If the label says "Min Pulse (µs)", the tooltip should add WHY/WHAT — not repeat the words.

**Live validation + live push rules:**

1. **Local validator** — a pure `configError(state) → string | null` function that checks ranges and cross-field constraints. Keep it next to the field it validates (in the shared widget or tab `<script>`).
2. **Debounced push** — on every `on:change` (or `on:input` for sliders), schedule a push via `setTimeout` (~350ms). Cancel any pending timer before scheduling a new one. On fire: re-run the validator, build the command from current state, dedupe against `lastPushed`, then call `SendCommand(...)`.
3. **Status reflection** — surface push state with a small visual indicator next to the field/section: `pending` (debounce timer running), `sent` (pushed successfully), `invalid` (validation failed — push blocked). CSS reference in [ServoWidget.svelte](app/go/studio/frontend/src/lib/components/ServoWidget.svelte) (`.push-status` / `.push-pending` / `.push-sent` / `.push-invalid`).
4. **Cleanup** — `onDestroy(() => { if (pushTimer) clearTimeout(pushTimer) })`. Reset `lastPushed = ''` and clear pending timer whenever the active channel/target changes (`$: { void activeId; … }`).
5. **Position vs config separation** — **only configuration** fields (ranges, motion profiles, modes, calibration) live-push. **Live action** fields (current position, jog buttons, test triggers) call `SendCommand` directly without debouncing — they are commands, not config.
6. **Persistence still requires Save** — live push updates the running board state (RAM). The user must still click Save to persist to LittleFS via `UploadConfig(yaml)`. Make this clear in the tab UI; don't conflate the two.

**Reference implementation:** [ServoWidget.svelte](app/go/studio/frontend/src/lib/components/ServoWidget.svelte) — the per-channel servo card has full tooltip coverage, `configError()` local validator, debounced `scheduleLivePush()`, dedup against `lastPushed`, and a behavior-summary box that updates live as the user edits values.

**Why this matters:**
- Operators learn the system by hovering, not by reading docs. Tooltips are documentation that ships with the binary.
- Bad values caught at edit time (red border + push-invalid badge) are 10× cheaper than bad values caught after a flash cycle.
- Live push closes the loop — the operator can immediately see whether their change had the intended effect on the actual hardware, before committing the config.

**When adding a new field:** add the tooltip and wire it into `scheduleLivePush()` in the same commit as the input itself. Adding tooltips/validation in a "polish pass" later means going back through every reactive form field — much easier to do alongside the initial layout.

### 25. Shared-Module Commands Are Universal, Peer Capacity Is Dynamic (MANDATORY)

**Command groups driven by a shared firmware module (StorageServer, ConfigServer, any future cross-board module) MUST be registered with `Controller: ""` — the universal filter. They MUST NOT be bundled into a board-specific group and MUST NOT be re-added inside each board handler via `for k, v := range h.E.ConfigCommands()`.**

The engine filters `CmdGroup.Controller` in two places: [engine.go:221-225](app/go/engine/engine.go#L221) (dispatch) and [engine.go:365-368](app/go/engine/engine.go#L365) (help rendering). A command parked inside a group with `Controller: pcore.CtrlHubFX` is hidden from `help` and refused from dispatch whenever the connected board is something else — even when the underlying firmware supports the command. This caused `flash.status`, `file.list`, `config.*`, etc. to be invisible on LightFX/GearControl despite every board registering `storageServer` + `configServer`.

**Rules:**

1. **One universal group per shared module.** In [engine/handlers/hubfx/handler.go](app/go/engine/handlers/hubfx/handler.go), `storageCommands()` is the single source of truth for `sd.*`, `flash.status`, `file.*`, and `config.*`. It has no `Controller` field → visible and dispatchable on every board.
2. **Board handlers MUST NOT merge `ConfigCommands()`** into their own board-filtered group. The universal group already registers them.
3. **HubFX group stays HubFX-only.** `slaves`, `slave.*`, `audio.*`, `codec.*`, `engine.*`, `usb.*` map to packet types only the ESP32-S3 master firmware implements — keep `Controller: pcore.CtrlHubFX` on that group.
4. **Peer capacity is a per-connection property, not a hardcoded constant.** `FILE_UPLOAD_DATA` payload must fit the peer's `MAX_PAYLOAD_SIZE` (Pico: 512, ESP32: 2048). [api/files.go](app/go/api/files.go) tracks this via `FileApi.peerMaxPayload` with `SetPeerMaxPayload(size)`. `Engine.SetControllerType(ct)` ([engine.go:110](app/go/engine/engine.go#L110)) is the single setter that propagates the detected peer type into the API layer — call it from every IDENTIFY/INIT site; never assign `e.ControllerType = …` directly (except when clearing to `""` on disconnect, where the API is already nil).
5. **Chunk-size constants are named and exported.** `PicoMaxPayload`, `Esp32MaxPayload`, `UploadHeaderSize`, `UploadChunkSize` (= `PicoMaxPayload - UploadHeaderSize`, the safe universal default). Do not inline `508` / `2044` magic numbers at call sites.

**Why this matters:**

- Any board that runs `sfx_storage` / `sfx_config` automatically gets the CLI + Studio surface — adding a new Pico board (e.g. a future SoundFX) needs no CLI wiring, just the firmware modules.
- Hardcoded 508-byte chunks on ESP32 uploads leave ~75% of the COBS RX buffer unused and quadruple the sync-upload time. The dynamic setter upgrades HubFX uploads to 2044-byte chunks while keeping Picos safe.
- Silent segment-0 timeout (the symptom of an oversized chunk hitting a Pico) is one of the hardest-to-diagnose bugs in the protocol. Centralizing capacity in one setter prevents the next contributor from regressing it.

**When adding a new shared firmware module with CLI commands:** create its own universal group (either inline in an existing handler or as a new `engine/handlers/<module>/` package registered from [handlers.go](app/go/engine/handlers/handlers.go)). Never attach it to a board-specific group "because HubFX uses it too."

### 26. Per-Board Config Filename + Auto-Hydrate Studio Tabs on Connect (MANDATORY)

**Each controller stores its config in a board-specific YAML file on flash, and every Studio board tab with a `BoardConfigDriver<T>` MUST auto-download that file on connect and populate its UI.** The generic name `/config.yaml` is reserved for legacy/unflashed boards only — new firmware, Studio, and CLI all use per-board filenames.

**Canonical filenames (source of truth: firmware schema `defaultPath()`):**

| Controller   | YAML path                    | Schema `defaultPath()`                                                                                     |
| ------------ | ---------------------------- | ---------------------------------------------------------------------------------------------------------- |
| GearControl  | `/gearcontrol.yaml`          | [gearcontrol_config.h](controllers/gearcontrol/pico/src/config/gearcontrol_config.h)                       |
| LightFX      | `/lightfx.yaml`              | [lightfx_config.h](controllers/lightfx/pico/src/config/lightfx_config.h)                                   |
| HubFX        | `/hubfx.yaml`                | [hubfx_config.h](controllers/hubfx/esp32s3/src/config/hubfx_config.h)                                      |
| GunFX        | `/gunfx.yaml` *(reserved)*   | no schema yet — add with the filename baked in                                                             |
| legacy       | `/config.yaml`               | fallback when `ControllerType == ""` (unidentified board)                                                  |

The firmware never hardcodes the string outside `defaultPath()`. Every `loadConfig()` / `reloadConfig()` / `saveConfig()` call that passes `nullptr` resolves to the schema default, so renaming the schema is the ONE place to change. Never embed `"/config.yaml"` in `.ino` setup code, storage bridges, or new config modules.

**Studio mapping:** [app.go:configPathFor(ct)](app/go/studio/app.go) maps `ControllerType → path`. `DownloadConfig` / `UploadConfig` read this per call — the TS side stays opaque (`DownloadConfig()` takes no path arg).

**Auto-hydrate flow.** Hardcoded TS defaults are fallback behavior for fresh/blank boards, never the steady state — the user expects the Studio to reflect what the board actually has on its flash the moment the connection dialog closes.

This is wired through a single shared helper — tabs do not hand-roll their own subscription, download call, or YAML parse:

```ts
// app/go/studio/frontend/src/lib/tabs/<Board>Tab.svelte
import { autoLoadOnConnect } from '../config/config-loader'

const driver: BoardConfigDriver<TState> = { /* existing driver */ }

onMount(() => autoLoadOnConnect(driver, ['<boardType>']))
```

**Rules:**

1. **Use the helper, not the primitives.** `autoLoadOnConnect(driver, controllerTypes)` lives in [config-loader.ts](app/go/studio/frontend/src/lib/config/config-loader.ts). It subscribes to `connectionInfo`, fires once per `(port × controllerType)` tuple, and calls `driver.parseYaml()` → `driver.applyState()` on success. Do NOT open-code `EventsOn('connection:changed', ...)` + `DownloadConfig()` inside a tab.
2. **Wire from `onMount`, not script top-level.** The driver `const` is initialized during script execution; registering the subscription from `onMount` guarantees the driver exists when the callback fires. Return the unsubscribe fn from `onMount` so Svelte cleans up on destroy.
3. **Filter by controller type.** Pass the exact `controllerType` values the tab supports (`['gearcontrol']`, `['lightfx']`, `['hubfx']`). A HubFX-aware tab that also loads under slave connections passes both. `'*'` is allowed only for genuinely universal tabs (currently none).
4. **`driver.applyState` is the single source of truth for "YAML → UI".** Do not duplicate apply logic in the tab — if a field doesn't round-trip, fix `applyState`, don't branch in the loader. Parse warnings surface as `warning` console messages; hard errors as `error`; successful apply as `ok` (all prefixed `[<boardType>]`).
5. **Missing `/config.yaml` is not an error.** `DownloadConfig()` returns `""` for NOT_FOUND — the loader treats that as "fresh board, keep hardcoded defaults" and emits an `info` line to the console. Do not alert/modal on this case.
6. **No progress dialog.** Auto-load is a silent background fetch. The `UploadProgressDialog` is for user-initiated saves/uploads only — a popup on every connect would be noise.
7. **New board tabs without a `BoardConfigDriver<T>` (e.g. GunFxTab today) are exempt until they gain one.** Adding a driver implicitly opts the tab into Rule 26 — wire `autoLoadOnConnect` in the same PR.

**Why this matters:**

- Without auto-hydrate, the user edits values against stale hardcoded defaults, clicks Save, and silently overwrites their on-device config. The first Save after connect becomes dangerous.
- The YAML round-trip goes through the same driver the Save dialog uses, so any bug in `parseYaml`/`applyState` surfaces on connect rather than on save — the failure mode is visible, not latent.
- Centralizing in one helper means tweaking the hydrate semantics (add an explicit `config.reload` first, throttle re-applies, surface an "out of sync" badge, etc.) is a one-file change, not a four-tab change.

Reference implementation: [config-loader.ts](app/go/studio/frontend/src/lib/config/config-loader.ts), consumed by [GearControlTab.svelte](app/go/studio/frontend/src/lib/tabs/GearControlTab.svelte), [LightFxTab.svelte](app/go/studio/frontend/src/lib/tabs/LightFxTab.svelte), [HubFxTab.svelte](app/go/studio/frontend/src/lib/tabs/HubFxTab.svelte).

### 27. Canonical YAML Style: Indented Block Sequences (MANDATORY)

Every ScaleFX YAML emitter — Studio's `generateGearControlYaml` / `generateLightYaml`, the reference `controllers/*/pico/config.yaml` files, and any future CLI/script that authors a config — must emit **indented block sequences**. Sequence items sit 2 spaces under the parent key; continuation lines sit 4 spaces under.

```yaml
retracts:
  - channel: 0              # 2 spaces under "retracts:"
    enabled: true           # 4 spaces under "retracts:"
    stall_current_mA: 500
    timeout_ms: 60000
```

All parsers (firmware `YamlParser`, Go via `gopkg.in/yaml.v3`, Studio TS `parseYaml`) accept both the indented form AND the YAML-spec "compact" form (sequence items at the same column as the parent key) for backward compatibility with older files:

```yaml
retracts:
- channel: 0                # legal YAML, still parses, but DO NOT emit this form.
  enabled: true
```

They ALSO accept **single-line flow collections** for hand-authored leaf objects — far more readable than a 3-line block for a small map. A flow `{`/`[` must close on the same line; flow and block freely nest:

```yaml
ports:
  - { kind: pwm, idx: 0, role: led_animator, label: "Beacon" }   # flow map item
channels:
  - port: { kind: pwm, idx: 0 }   # flow map value; block events list below
    events:
      - kind: "on"
        brightness_pct: 100
```

**Rules:**

1. **Emitters always use indented BLOCK form** (never flow). The Studio TS generators take a `base` level and emit `L1 = indent(base+1)` for the `- ` line, `L2 = indent(base+2)` for continuations. Treat any new emitter as a pull-request-blocking regression if it drops back to compact or flow form. Flow is an INPUT convenience only — a Studio Save round-trips a hand-authored flow file back to block.
2. **Parsers accept block (indented + compact) AND flow forms.** The Studio TS parser (`parseNested` in [yaml-parser.ts](app/go/studio/frontend/src/lib/config/yaml-parser.ts)) promotes a same-indent `- …` line to a sequence nested under the preceding key; `parseFlowValue` handles `{}`/`[]`. The firmware parser handles same-indent block sequences (see [yaml_parser.ipp](controllers/lib/sfx_config/config/yaml_parser.ipp)) and flow collections via `parseFlowNode`. Go uses `gopkg.in/yaml.v3` (full YAML). Keep the two custom parsers (firmware + TS) in lock-step — if one gains an input form, the other must too, or a hand-authored file parses on one side and silently fails on the other.
3. **Hand-written YAML uses indented form.** Reference `config.yaml` files under `controllers/*/pico/` are the canonical examples; copy their layout.
4. **Round-trip is stable.** After a Save in Studio, the on-device file is indented form. After a CLI upload of a hand-written file, the device stores whatever bytes were uploaded (the firmware caches raw YAML) — so `cat`-ing the repo's reference `config.yaml` into the device preserves the indented form.
5. **Documentation mirrors this rule.** [controllers/lib/sfx_config/README.md](controllers/lib/sfx_config/README.md) carries the canonical example under "Canonical YAML Style". Update both when the style ever changes.

**Why this matters:**

- Compact form is legal YAML but ambiguous to quick-glance readers — a `- ` at column 0 looks like a top-level list, not a nested sequence. Diffs and code review suffer.
- Studio's 880-byte on-device file shipped in compact form was silently mis-parsed by the TS parser: three retracts, seven pins, three door modes, and a battery block were all dropped because `parseMapping` broke at the first `- ` line. The UI hydrated from defaults while claiming "applied" — a silent-data-loss class of bug.
- A single canonical style means the round-trip (firmware ↔ Studio ↔ CLI) is a pure identity transform. Any drift (e.g., Studio emits compact, firmware round-trips compact, CLI parser someday regresses) is a single-file fix instead of a forensic multi-component audit.

Reference: [config-yaml-gen.ts](app/go/studio/frontend/src/lib/config/config-yaml-gen.ts) emitters, [yaml-parser.ts](app/go/studio/frontend/src/lib/config/yaml-parser.ts) `parseNested` / `parseFlowValue`, firmware [yaml_parser.ipp](controllers/lib/sfx_config/config/yaml_parser.ipp) `parseFlowNode`. (Go uses `gopkg.in/yaml.v3`; the legacy hand-rolled `engine/config_schema.go` parser is archived.)

### 28. Shared Servo Calibration Dialog (MANDATORY for board UIs)

**Per-board servo configuration in Studio MUST be done through the shared [ServoCalibrationDialog](app/go/studio/frontend/src/lib/dialogs/ServoCalibrationDialog.svelte). Board tabs MUST NOT inline a custom servo config panel (sliders for min/max/speed/accel/decel/reversed) inside a binding row.**

The dialog is the single canonical surface for servo calibration: it provides a live position slider with throttled jog (~30 ms), debounced config push (~350 ms) for min/max/speed/accel/decel/reversed, a Save commit (`servo.config`) and a Cancel restore. Embedding this UI in every tab fragments the operator experience and duplicates state-management code (debouncing, restore-on-cancel, throttling).

**Rules:**

1. **Tab → Calibrate button → dialog.** Each binding row in a board tab shows a compact summary (range / speed / REV tag) plus a `⚙ Calibrate Servo…` button. Clicking it opens `ServoCalibrationDialog` with the current binding values prefilled.
2. **One dialog instance per tab.** State held in the tab: `calibDialogOpen`, `calibServoId`, `calibServoName`, `calibInit { min_us, max_us, speed, accel, decel, reversed }`, plus a tab-specific target (e.g. `calibTargetGroup` for landing-light groups, `calibTargetPin` for door servos). Mount the dialog once at the bottom of the template; rebind props on each open.
3. **`onApply` writes back to the binding.** The dialog's `onApply(cfg)` callback is the only path that mutates the underlying binding state — no custom Apply buttons inside the tab. The dialog's own debounced live push handles the wire push; `onApply` just persists the values into the binding so they survive a Save.
4. **`supportsAccelDecel` per board.** Set `true` for boards whose servo controller honors accel/decel (LightFX 3-servo board, GearControl door+yaw servos). The dialog hides those fields when `false`.
5. **Old inline panels are deleted, not gated.** When migrating a tab to the shared dialog, remove the inline panel HTML and all its helpers (`setServoPosition`, `applyServoConfig`, `centerServo`, etc.) outright. No "// kept for back-compat" stubs (Rule 21).

**Reference implementations:**
- [GearControlTab.svelte](app/go/studio/frontend/src/lib/tabs/GearControlTab.svelte) — door + yaw servos open the dialog with `openDoorServoSetLimits` / `openYawServoSetLimits`.
- [LightFxTab.svelte](app/go/studio/frontend/src/lib/tabs/LightFxTab.svelte) — landing-light group bindings use `openServoCalibration(gi)`.

**Why this matters:**
- Servo calibration is hard. The shared dialog encodes the right defaults (300–2700 µs guard, 0 = instant, debounce window, restore-on-cancel) — every tab that re-implements them gets at least one wrong.
- A consistent calibration UX means an operator who learned door servos on GearControl can calibrate landing lights on LightFX with zero re-learning.
- Centralised state machines for live-jog throttling + config debounce reduce the cross-board surface area for bugs (e.g. dropped final pushes, double-applying after Save).

### 29. Battery Card Layout (MANDATORY for board UIs with battery monitoring)

**Every board tab that surfaces battery state MUST use the canonical battery card layout (bar + voltage display, then a toggle row, then a chemistry+cell-count row), placed in the LEFT column of the tab.** GearControl is the reference; LightFX matches it.

**Required structure:**

```svelte
<section class="card">
  <div class="card-header"><h3>… Battery</h3></div>

  <div class="batt-display">
    <div class="batt-bar-track">
      <div class="batt-bar-fill" class:low={batteryLow} style="width: {batteryPct}%"></div>
    </div>
    <div class="batt-info">
      <span class="batt-voltage" class:low={batteryLow}>{batteryVolts} V</span>
      <span class="batt-pct" class:low={batteryLow}>{batteryPct}%</span>
      {#if batteryLow}<span class="batt-warn">⚠ LOW</span>{/if}
      {#if batteryLowTriggered}<span class="batt-warn">CUTOFF FIRED</span>{/if}
    </div>
  </div>

  <div class="form-row">
    <label class="toggle">…Auto-Cutoff/Auto-Deploy toggle…</label>
    <span class="push-badge push-{$pushStatus['battery.cutoff']||''}">…</span>
    <button on:click={applyBattery}>Apply</button>
  </div>

  <div class="form-row">
    <select bind:value={batteryChemistry} on:change={scheduleBatteryPush}>…</select>
    <input type="number" bind:value={batteryCellCount} on:input={scheduleBatteryPush} />
    <span class="push-badge push-{$pushStatus['battery']||''}">…</span>
    {#if batteryCellCount === 0}…auto-detect hint…{/if}
  </div>
</section>
```

**Rules:**

1. **Left column placement.** The battery card lives in the LEFT column of the two-column tab layout, after the primary configuration cards (e.g. after Channels in LightFX, after Channel Toggles in GearControl). Never park it in the right column or in a separate tab.
2. **Bar + voltage + pct + warnings live in `.batt-display`.** Use `batteryLow` (computed from per-cell threshold) for `.low` colouring; `batteryLowTriggered` (broadcast bit) for the CUTOFF FIRED badge.
3. **Two `form-row` blocks below the display.** Top row: the auto-cutoff/auto-deploy toggle + push-badge + Apply button. Bottom row: chemistry select + cell-count input + push-badge + auto-detect hint.
4. **Two separate live-push keys.** `battery` for chemistry+cells, `battery.cutoff` for the cutoff toggle. They have independent firmware commands, so independent debounce buckets.
5. **Apply button forces a resend** of both `battery` and `battery.cutoff` (skip dedup). Tooltip: "Force resend `battery` + `battery.cutoff` now".
6. **Cell-count = 0 means auto-detect.** Show the inferred cell count and per-cell voltage as a hint when in auto mode.
7. **Reuse the GearControl CSS classes verbatim** (`batt-display`, `batt-bar-track`, `batt-bar-fill`, `batt-info`, `batt-voltage`, `batt-pct`, `batt-warn`). Do not invent board-local class names.

**Why this matters:**
- Battery is safety-critical UX. An inconsistent layout across boards (e.g. battery on the right in one tab, hidden in a sub-section in another) means operators miss a low-voltage warning at the worst time.
- The bar+voltage+pct triple is the at-a-glance read; everything else (chemistry, cells, cutoff) is configuration. Mixing them in a single row dilutes the at-a-glance.
- Reusing CSS classes means a global tweak (e.g. WCAG contrast bump on `.batt-warn`) ships to every board with a single edit.

**Reference:** [GearControlTab.svelte](app/go/studio/frontend/src/lib/tabs/GearControlTab.svelte) lines 1302–1375, [LightFxTab.svelte](app/go/studio/frontend/src/lib/tabs/LightFxTab.svelte).

### 30. Mandatory Board-Prefix on CLI Commands (CLI + Studio Console)

**Every board command group sets `CmdGroup.Prefix`; the CLI dispatcher rejects bare board names with a "did you mean" hint.** Universal groups (Core, Firmware, Storage/Config) leave `Prefix` empty.

| Group       | `Controller`      | `Prefix`  | Canonical invocation        |
|-------------|-------------------|-----------|------------------------------|
| LightFX     | `CtrlLightFX`     | `light`   | `light:servo 1 1500`        |
| GearControl | `CtrlGearControl` | `gear`    | `gear:reset all`            |
| GunFX       | `CtrlGunFX`       | `gun`     | `gun:trigger on 600`        |
| HubFX       | `CtrlHubFX`       | `hub`     | `hub:slaves`                 |
| Core / Firmware / Storage / Config | `""` | `""`       | `connect`, `init`, `file.list`, `config.save` |

```go
g := &engine.CmdGroup{
    Name:       "LightFX",
    Controller: pcore.CtrlLightFX,
    Prefix:     "light",  // ← mandatory for board groups
    Color:      engine.ColorBlue,
    Commands:   map[string]engine.CmdEntry{
        "servo": {h.cmdServo, "servo set <id> <pulse_us>", "Set servo position", true},
        // … no per-entry prefix; FlatCommands stamps it on automatically
    },
}
```

Wire format is unchanged — the prefix is a CLI surface convention only. It exists because once a hub fans out to multiple slave types, bare names like `servo`, `reset`, `enable`, `battery`, `battery.cutoff` overlap across LightFX / GearControl / GunFX and the engine has no signal to pick one without an explicit target.

The dispatcher surfaces the prefixed candidates whenever a bare board name is typed:

```
scalefx> servo 1 1500
✗ Command 'servo' requires a board prefix. Did you mean: gear:servo, gun:servo, light:servo
```

**Why this matters:**
- A direct-connection script that runs `servo 1 1500` works on *one* board today, breaks silently on a hub tomorrow when a second slave appears with the same command name. Forcing the prefix everywhere makes scripts portable across direct + hub topologies.
- Help output shows the canonical form so muscle memory is built correctly from the first session.
- Studio's Console panel echoes the same prefixed form (it just passes typed input through `SendCommand` → `Engine.Dispatch`).

**Implementation:** `CmdGroup.Prefix` field ([engine/types.go](app/go/engine/types.go)); `FlatCommands` keys entries as `<prefix>:<name>` and stamps the prefix into `CmdEntry.Usage`; `Dispatch` and `CmdHelp` fall through to `suggestPrefixed(name)` when a bare board command is typed. Studio typed APIs (`LightFxApi.*`, `GearControlApi.*`, `GunFxApi.*`, `HubFxApi.*`) are unaffected — only text dispatch carries the prefix. See [13-PASSTHROUGH-ROUTING.md §4.4](../instructions/13-PASSTHROUGH-ROUTING.md).

### 31. Port Direction Is Fixed; Input Count ≤ UART Peripherals

Each port a board declares (`kServoPorts` / `kPwmPorts` / `kHBridgePorts` / `kInputPorts`) has a **direction baked in at compile time**. There is **NO runtime swap between input and output** — once a header is declared as a `ServoPort` (output), it stays output for the firmware's lifetime; once declared as an `InputPort`, it stays input. This is enforced by the role registry: each port kind accepts only a fixed subset of role kinds.

Port-kind direction matrix:

| PortKind | Direction | Multi-modal? | Roles that can attach |
|----------|-----------|--------------|------------------------|
| `Servo`   | output | no  | `ServoActuatorRole` |
| `Pwm`     | output | no  | `LedAnimator`, `DcMotorRole`, `HeaterRole` |
| `HBridge` | output | no  | `BiDcMotorRole` |
| `Input`   | input  | **yes** — pulse capture OR UART RX | `RcPwmInputRole`, `PpmInputRole`, `SbusInputRole`, `JetiExInputRole`, `CrsfInputRole`, future |

The **`Input` kind is the only multi-modal port** — at role-attach time the underlying driver configures the GPIO for either edge-IRQ pulse capture (PPM / RC-PWM) or UART RX (SBUS, Jeti EX, CRSF, future serial protocols). Because every `InputPort` may need a UART peripheral simultaneously, the **count of `InputPort`s a board declares MUST NOT exceed the number of free UART peripherals on the platform** so role attachment never starves on UART:

| Platform | UARTs total | Reserved for console | Max `InputPort` count |
|----------|-------------|----------------------|------------------------|
| ESP32-S3 | 3 (UART0, UART1, UART2) | UART0 → CH343 USB-UART bridge | **2** |
| RP2040/RP2350 | 2 (UART0, UART1) | UART0 → console | **1** (extra UARTs via PIO are not counted toward the budget) |

HubFX currently declares **1 `InputPort` on IN_1 (GPIO5)** — well within the ESP32-S3 budget. IN_2..IN_12 are `ServoPort` outputs and cannot be repurposed as inputs at runtime; if a future board variant needs more inputs, declare them as additional `InputPort`s up to the UART limit and reduce the output count accordingly.

This rule supersedes the older "`ServoPort` can be input or output" model — the legacy `ServoPort` input-mode methods (`readMicroseconds`, `supportsInput`) were retired with the InputPort split.

### 32. Board GUID: 4-Hex Suffix of `deviceName`; Collisions Are Master-Resolved with a Persistent Override

Every ScaleFX board exposes a stable hardware-derived **GUID** so masters can tell two boards of the same kind (e.g. two LightFx expanders) apart and persist per-board state across reconnects.

**Source.** `sfxGetBoardId(out, maxLen)` ([sfx_platform.h](controllers/lib/sfx_platform/platform/sfx_platform.h)) produces an 8-char uppercase hex string (4 bytes). Pico: last 4 bytes of `pico_unique_board_id_t` (8-byte OTP flash unique-id). ESP32: last 4 bytes of the factory MAC (`esp_efuse_mac_get_default`). Both immutable per silicon, survive reflashing.

**Surface on the wire.** `BoardServerBase::buildDeviceName(prefix)` ([board_server.cpp](controllers/lib/sfx_board/server/board_server.cpp)) emits `deviceName = "<Prefix>-<last 4 hex chars>"` — e.g. `"GunFx-3C4D"`. That suffix is the **canonical GUID** broadcast in `INIT_READY` / `IDENTIFY` payloads (16 bits / 65 536 values). Sufficient at ScaleFX scale (a hub hosts ≤ 2 expanders today) but **not collision-proof at fleet scale** — the master MUST detect collisions, not assume them away.

**Collision detection (master side).** HubFX's `ExpanderServicePolicyT<>` tracks every active expander's GUID. If an `IDENTIFY` response carries a GUID already held by another live slot, the master:

1. Sets `entry.spec.collision = true` on BOTH conflicting slots.
2. Logs `SFX_LOG_ERROR("[Expander] GUID COLLISION ...")` with the conflicting USB addresses.
3. Emits `EXPANDER_COLLISION` (0x87) async wire packet `[guidLen][guid][addrA][addrB]` so Studio can prompt the user.
4. Keeps both boards connected (the user needs CDC access to fix the override).
5. Topology-layer routing (`TopologyServicePolicy`, future) MUST refuse to bind roles to collided slots — the binding is ambiguous until the user resolves which physical board is which.

**Override (board side).** `/board.yaml` gains an optional `board.guid:` field (4 hex chars, case-insensitive). When present, firmware emits `<Prefix>-<override>` instead of `<Prefix>-<hwGuid>` in `INIT_READY` / `IDENTIFY`. Schema:

```yaml
board:
  identifier: "left-wing"      # existing, human label (BoardIdentifier)
  guid:       "1ABC"           # NEW, optional GUID override (Rule 32)
```

The override path is: user picks a free 4-char value in Studio → Studio sends a `BOARD_SET_GUID` wire command to the offending expander (forwarded through the hub) → expander persists to `/board.yaml` → reboots → re-enumerates with the new GUID → master accepts.

**Validation on set.** Must be 4 hex chars (`[0-9A-F]{4}`), must not match any other currently-known live GUID, must not be reserved (`0000`, `FFFF`).

**Mirror in Go.** `app/go/protocol/expanders/` carries the matching `EXPANDER_COLLISION` decoder + `GUID_COLLISION` error string; Studio surfaces a red badge on each collided board until a fresh `EXPANDER_IDENTIFIED` arrives without the collision flag.

**Don't confuse with `BoardIdentifier`.** `BoardIdentifier` ([board_identifier.h](controllers/lib/sfx_board/server/board_identifier.h)) is a *separate* user-assigned human label in `/board.yaml`. It's mutable, descriptive, never used as a persistence key. The GUID is the persistence key; the identifier is the display name.

**Future: full-width GUID upgrade path.** If 16 bits becomes insufficient, append a `[guidLen:u8][guid:N]` field to the `INIT_READY` / `IDENTIFY` payload (Rule 11 append-only). The producer (`sfxGetBoardId`) already returns 8 hex chars; emit them all instead of slicing to 4. Old masters keep working — they fall back to the deviceName suffix.

### 33. Eliminate Redundant Template Parameters — Infer From Carriers (MANDATORY)

A class / helper / free function MUST NOT take a template parameter that is recoverable from another template parameter it already has. Every "carrier" type (mixer, board, schema, store, policy, …) is responsible for re-exporting its own template arguments as nested typedefs so downstream consumers can recover them via `Carrier::Member` instead of re-stating them at every use site. Same idea for argument-deduction on free function templates: deduce, never duplicate.

**Required patterns:**

1. **Re-export every template parameter as a nested typedef on the carrier.** A class declared `template <typename TI2S, typename TCodec> class AudioMixer` MUST publish
   ```cpp
   using I2SOutput = TI2S;
   using Codec     = TCodec;
   ```
   so any helper built on top can be **single-arg** and recover the rest:
   ```cpp
   template <typename TMixer>
   class EspDualCoreAudio {
       using CodecType = typename TMixer::Codec;
       using Adapter   = CodecAdapter<CodecType>;
   };
   ```

2. **Trait specialization beats a second template arg** when the second arg is *board-local config keyed by a type* (codec pins / supply, schema → pool size, transport → buffer sizing). Use a trait template the board specializes — NOT a second template param:
   ```cpp
   // ❌ board spells the codec type twice
   template <typename TMixer, typename TCodecAdapter>
   class EspDualCoreAudio { ... };
   static EspDualCoreAudio<Mixer, HubFxCodec> audio;

   // ✅ trait keyed on the inferred codec — sketch states the type once
   template <typename TCodec> struct CodecAdapter { /* primary = no-op */ };
   template <typename TMixer> class EspDualCoreAudio {
       using Adapter = CodecAdapter<typename TMixer::Codec>;
   };
   static EspDualCoreAudio<Mixer> audio;
   template <> struct CodecAdapter<TAS5825PCodec> { /* board-specific */ };
   ```
   The specialization sits next to the board's pin map — exactly where the config it depends on already lives. Primary template = sensible default (no-op / passive); `if constexpr` strips unused paths at compile time.

3. **Platform aliases hide policy types.** Every multi-platform service policy ships its `using XxxService = XxxServicePolicy<PlatformImpl>;` in the library so sketches never spell the policy type out:
   ```cpp
   // sfx_storage exports:
   #if SFX_PLATFORM_ESP32
   using StorageService = StorageServicePolicy<Esp32StoragePolicy>;
   #elif SFX_PLATFORM_PICO
   using StorageService = StorageServicePolicy<PicoStoragePolicy>;
   #endif

   // sketch writes:
   using HubFxBoard = BoardOf<HubFxBoard, ..., StorageService, ...>;
   ```
   New service policies added to a library MUST ship the platform alias in the same header.

4. **Deduce free-function template args from arguments.** When a function template takes both a type parameter and an argument typed on it, the argument deduction MUST do the work — never re-state:
   ```cpp
   // ✅ TStoragePolicy deduced from the reference argument
   wireUploadExclusivity<Mixer>(board.policy<StorageService>());
   ```

**Don't:**

- Leave a template parameter that mirrors a typedef the carrier already exposes (e.g., `EspDualCoreAudio<Mixer, TAS5825PCodec>` after `Mixer` already encodes `TAS5825PCodec`).
- Reach for `std::function` / runtime callbacks to side-step the type system when a trait specialization or a carrier typedef gives equivalent flexibility with **static dispatch** (zero heap, zero indirection, all inlined).
- Force every board sketch to spell `Policy<PlatformImpl>` when one platform alias in the library covers it.
- Add a "concept-driven" `findPolicy` mechanism just to remove one type-mention duplication — that's overshoot; the alias pattern is enough.

**Reference refactor:** [esp_dual_core_audio.h](controllers/lib/sfx_audio/audio/esp_dual_core_audio.h) (May 2026) — codec adapter trait keyed on `TMixer::Codec`; sketch writes `EspDualCoreAudio<Mixer>` and specializes `CodecAdapter<TAS5825PCodec>` next to its pin map. Codec is named once, at `using Mixer = AudioMixer<EspI2SOutput, TAS5825PCodec>;`.

### 34. Studio Design System — Reuse the Shared Component Classes (MANDATORY for Studio UI)

ScaleFX Studio has ONE design language defined in [style.css](app/go/studio/frontend/src/style.css). Every tab/panel/dialog composes those shared classes — it does NOT invent bespoke button / input / card styling in its own `<style>` block. New per-component CSS is for *layout* (grid/flex placement) only, never for re-skinning controls. This keeps control heights, paddings, and colours uniform (the regression that motivated this rule: hand-rolled `.btn`/`select` styles with mismatched padding produced ragged, different-height controls across the IO tab).

**The vocabulary (use these, don't redefine):**

- **Buttons:** the bare `button` element (already styled); `button.primary` for the confirming action; `.small` / `.tiny` size modifiers; `.action-btn` (icon+label), `.danger`. Never set `padding`/`background`/`border` on a button in a component — pick a modifier.
- **Inputs & selects:** `.field-input` (+ `.narrow` / `.wide`). All text inputs, number inputs, and `<select>`s carry it so they share one height. A row of mixed buttons + selects gets `height: 28px; box-sizing: border-box` on the group so they line up exactly.
- **Cards:** `.card` + `.card-header` (with `<h3>` uppercase title, optional inline `svg`) + `.header-actions`.
- **Forms:** `.form-row` (label + controls inline), `.form-field` / `.form-grid.cols-2|3` (stacked), `.field-label`, `.field-hint`.
- **State:** `.state-badge`, `.empty-state`, `.banner` (err/note).
- **Layout:** `.tab-content` (padding + scroll), two-column tabs split with a `1px var(--border)` divider; each column `overflow:auto`.
- **Colours:** always the CSS vars (`--bg-surface`, `--bg-raised`, `--bg-input`, `--border`, `--text`/`--text-dim`/`--text-bright`, `--accent`, `--success`, `--error`, `--warning`). Never hard-code hex.

**Conventions baked into the IO/device-model tabs (follow them):** human-readable labels in every dropdown (role kinds via `devicemodel.RoleLabel`, never raw kebab wire names); a port that can host exactly one role (servo) shows a fixed tag + a name field, not a 1-option dropdown; role pickers list only the kinds a port may host (`Port.AllowedRoles`); live value bars sit *under* the channel they belong to and render an explicit **NO SIGNAL** state (striped track) when the frame is missing/invalid, never a bare dash. Reference: [InputPanel.svelte](app/go/studio/frontend/src/lib/tabs/InputPanel.svelte), [PortRoleTab.svelte](app/go/studio/frontend/src/lib/tabs/PortRoleTab.svelte).

**Row-button order + alignment (MANDATORY).** When a form-row pairs an input with action buttons, place them in a fixed canonical order so columns line up across stacked rows:

1. The **field control** (input/select) takes the flex-1 slot — input on the left, buttons clustered to the right.
2. **Browse/picker** (`…` for file picker, `⚙ Calibrate…` for sub-dialogs) is the **leftmost** button — closest to the field it modifies (it *augments* the field).
3. **Clear / destructive** (`Clear` for clearing a path, never `None`; `Remove`, `Reset`) is the **rightmost** button — furthest from the field, last in tab order. Name it after what it does (`Clear`, `Remove`, `Reset`), not what state it produces (`None`, `Empty`).
4. **Reserve the rightmost slot** with a `visibility: hidden` spacer of the same width on rows that don't expose the destructive action (e.g. required rows). Without this, the browse `…` shifts column position between optional and required rows and the form looks ragged.

Pattern (see [EnginePanel.svelte](app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) sound-rows):
```svelte
<input class="field-input wide" … />
<button class="small btn-slot" on:click={browse}>…</button>      <!-- always present -->
{#if optional}
  <button class="small btn-slot" on:click={clear}>Clear</button> <!-- rightmost -->
{:else}
  <span class="btn-slot btn-spacer" aria-hidden="true"></span>   <!-- alignment shim -->
{/if}
```
The slot CSS is `width: 64px; box-sizing: border-box; flex-shrink: 0` so every cluster column is identical; the spacer is the same slot with `visibility: hidden`. This rule applies any time a row has 2+ action buttons; a single-button row needs no spacer.

**File picker is parametrized by storage backend (MANDATORY for `pickFile()` callers).** `pickFile({ targets })` accepts `'flash' | 'sd' | 'both'` (default `'both'`). Callers MUST pass the narrowest target the field actually lives on — sound files live on SD → `pickFile({ targets: 'sd' })`; config files live on flash → `pickFile({ targets: 'flash' })`. The picker hides the disallowed tab entirely (not just disables it) and opens directly on the allowed backend, so the operator can't accidentally browse the wrong filesystem. `targets: 'both'` stays available for the standalone File Manager dialog where both backends are valid.

**Full panel walkthrough:** [21-STUDIO-ENGINEFX-PANEL.md](../instructions/21-STUDIO-ENGINEFX-PANEL.md) covers the EngineFX panel end-to-end as the reference implementation of every rule in this section — read it before building a new operational effect tab.

### 35. Validation Gates Apply (MANDATORY for any Studio form that writes to the device)

Every Studio form whose **Apply** (or equivalent commit) button pushes settings to the firmware MUST be gated by validation: the Apply button is `disabled` whenever the form has one or more **error-severity** validation findings. Warnings are non-blocking; errors are. This is what stops an operator from pushing a config that the firmware will then NACK / mis-behave on (and gives the same UX whether the form is the EngineFx panel, the Ports & Roles list, the LightFx program editor, etc.).

**Required pattern:**

1. **Validate continuously** as the operator edits — on input change (debounce ~350 ms is fine), on load, and after any sub-dialog returns (file picker, role attach, …). Don't defer validation until Apply is clicked: the operator must see the error *as they cause it*.

2. **Surface every error twice** — at the **field/row level** (red border + light-red background + inline error line under the offending control with a `⚠ <reason>` prefix) AND at the **group level** (section header turns red with a `missing files` / `invalid wiring` chip). Reference: EngineFx panel sound-row validation in [EnginePanel.svelte](app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte).

3. **Disable Apply** when any error is present:
   ```svelte
   <button class="primary" on:click={onApply}
           disabled={busy || !dirty || hasErrors}
           title={hasErrors ? 'Resolve validation errors first' : 'Write … + reload — settings take effect immediately'}>
       Apply
   </button>
   ```
   The dirty/in-sync indicator next to it switches to a red `resolve errors above` (or similar) label so the blocked button isn't a mystery. The PortRoleTab Apply additionally reads `$validationCounts.errors` (the model-side issue count surfaced by the tab-strip badge) so domain-claim errors gate the IO Apply the same way.

4. **Distinguish required vs optional fields** in the validator: empty + required → error; empty + optional → valid (no check). For paths that should exist on the device, batch-probe via the `CheckFiles` binding — empty paths are skipped (a `None` button on optional rows clears the path and re-validates immediately).

5. **Validation is checked on read AND on write.** Reading a config from `/foo.yaml` runs the same validator so a malformed on-device file shows the same error UI the operator would see while editing — they're never surprised by what the device rejected.

6. **Operational actions (Start / Trigger / Test / Preview) are gated the same way.** Anything that *runs* the firmware against its currently-loaded config — `▶ Start`, `Trigger`, `Test fire`, sound `Preview` — MUST be `disabled` whenever the draft is dirty OR has errors, with a tooltip pointing at the cause (`'Apply unsaved changes before starting'` / `'Resolve validation errors first'`). Reason: the operator presses Start expecting their edits to be live, but the firmware only knows what was Applied — pushing Start on a dirty draft tests the *old* config and looks like a bug.

7. **Place Apply next to the operational action it unblocks.** When a panel has both Apply and operational buttons (Start/Stop/Trigger), they live in the SAME row, in the order `[dirty-flag] [Apply] [divider] [▶ Start] [■ Stop]` (a 1×20 px `var(--border)` `.ctrl-sep` separates "commit config" from "operate firmware"). Do not also render a duplicate Apply at the bottom of the form — one Apply per panel. Reference: [EnginePanel.svelte status-row](app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte). Panels that have no operational actions (PortRoleTab, config-only editors) keep Apply in the card header's `.header-actions`.

The point of the rule: a Studio operator should never need to read the wire log to discover that their config is broken. Errors are visible inline, the Apply button refuses to push them, operational buttons refuse to test stale drafts, and the firmware never receives a settings packet that it would NACK.

**Worked example:** [21-STUDIO-ENGINEFX-PANEL.md § 3–5](../instructions/21-STUDIO-ENGINEFX-PANEL.md) — status-row anatomy, sound-row validation lattice (CheckFiles batch-probe, dual-surface errors, optional vs required), `engineDraft`/`engineConfig`/`engineDirty` triad with the `wasDirty` snapshot pattern.

### 36. Channel-Setup Cluster (MANDATORY for any RC-channel-gated form)

Anywhere a Studio panel binds an RC input channel and gates an action on a microsecond threshold (EngineFx ▶ Start gate, GunFx trigger, LightFx mode-switch, …), the channel UI MUST follow this canonical 4-row cluster — channel selector, trigger settings *directly above* the live bar, the bar itself with explicit visual markers, then a single legend line. Don't scatter the threshold halfway down the form: the operator can't dial it correctly without seeing the marker move on the bar in real time.

**Cluster (top-to-bottom, single bordered box `.chan-cluster` so the four rows read as one control):**

1. **Channel selector** — `<select class="field-input wide">` of every bound channel (`fnId → label`), plus a leading `— manual only —` option for the unbound case.
2. **Trigger settings, inline form-row, directly above the bar:**
   ```
   Fires when channel ≥  [1500] µs   ±  [25] µs hysteresis
   ```
   Use **verb-led labels** that read like a sentence (`Fires when channel ≥`, `Activates above`, `Holds below`) — never bare `Threshold (µs)` / `Hysteresis (µs)` field labels. The `±` between threshold and hysteresis makes the semantic clear (deadband around the threshold).
3. **Live bar with visual markers** (`.bar.tall`, 18 px high so markers are readable):
   - **Threshold marker** — solid 2 px `var(--error)` vertical line at `usToPct(thresholdUs)`, with a soft red glow.
   - **Hysteresis band** — translucent `var(--warning)` rectangle from `usToPct(threshold − hyst)` to `usToPct(threshold + hyst)`, dashed warning-tinted side borders. Visible **even with no signal** so the operator can dial the trigger before powering the RC link.
   - **Live fill** — the existing `.bar-fill` gradient from 0 to `usToPct(liveUs)`. Bar shows `NO SIGNAL` (striped track, Rule 34) when the frame is invalid; legend still renders the threshold/hyst values.
4. **One-line legend** under the bar, color-coded to match the markers:
   ```
   <live µs>  ·  <threshold µs> THRESHOLD  ·  ±<hyst µs> HYSTERESIS  ·  1000–2000 µs
   ```
   Live = `--success` green (matches the bar fill end-stop); threshold = `--error` red (matches the line); hysteresis = `--warning` amber (matches the band); the `1000–2000 µs` range hint is right-aligned in `--text-dim`.

**Wire it together:** every input pushes through `mark()` so the marker/band positions update live as the operator types — that's the whole point of putting the settings above the bar. Threshold range is the PPM/SBUS norm `800–2200 µs` `step="10"`; hysteresis `0–500 µs` `step="5"`. Use the `usToPct(us)` helper from `devicemodel.ts` (1000µs → 0%, 2000µs → 100%) so all panels use the same bar scale.

**Shared component (MANDATORY 2026-05-23):** the markup + styling are factored into [`ChannelToggleCluster.svelte`](../app/go/studio/frontend/src/lib/components/ChannelToggleCluster.svelte). **Every new channel-gated form imports it instead of inlining the rows** — props: `channelLabel`, `emptyOption`, `options=[{id,label}]`, `inputId`, `thresholdUs`, `hysteresisUs`, `liveUs`, `liveValid`, `actionVerb` ("Fires" / "Triggers" / "Switches"), `onChange({inputId, thresholdUs, hysteresisUs})`. Reference call sites: [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) (engine on/off), [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) per-gun Trigger section. Inlining the 60-line block again is a Rule 36 violation. **Full walkthrough:** [21-STUDIO-ENGINEFX-PANEL.md § 2](../instructions/21-STUDIO-ENGINEFX-PANEL.md) (the bar with overlays — design rationale, z-order, colour semantics, reuse checklist).

### 37. Port Voltage Metadata (declaration-time rail tagging)

Every output port descriptor (`ports::pwm_array<>`, `ports::servo_array<>`, `ports::hbridge_array<>`, `ports::input_array<>`) MAY carry a `voltageMv` template parameter set at declaration time via `.with_voltage_mV<N>()`. The board author tags the rail each port array is wired to — HubFX CH1..8 = `8000`, HubFX servo headers = `5000`, HubFX input = `3300`, GearControl H-bridges = battery (declare `0` = unknown until measured). The value flows through the binding → `PORT_LIST_RESP` wire (`[idx:u8][flags:u8][voltageMv:u16LE]`, fixed 4 bytes per entry) → Go `ports.PortDescriptor.VoltageMv` → `devicemodel.Port.VoltageMv` → Studio `Port.voltageMv` + `formatPortRail()` helper. `0` = unknown / unconstrained (UI shows no label, no voltage check).

**Why:** effects that drive sub-rail elements (a 5 V smoke heater on the 8 V rail, an LED ring rated for 3 V on the 5 V rail) need to compute a PWM duty that delivers the element's rated voltage — `duty = element_mV / port_mV` linear, or quadratic for power-mode scaling. Without per-port voltage, every effect would have to bake in the platform-specific rail topology.

**Effect-side use:** `binding.voltageMv` is on every `PwmBinding` / `ServoBinding` / `HBridgeBinding` / `InputBinding`. An effect reads it once at role-attach time and stores the computed duty in its config. UI shows the rail in every port picker option (`"HubFX CH3 (8 V) · pwm"`) so the operator can spot voltage mismatches before applying.

**Cap token:** `portCaps()` appends `"VOLTAGE_8V"` / `"VOLTAGE_3V3"` to the `Caps` list when `voltageMv > 0` (uppercase, dot replaced with `V` for fractional volts so the token round-trips into CSS / log filters). Informational only — doesn't gate `Candidates()` filtering.

**Adding voltage to a new board:** in the descriptor list, chain `.with_voltage_mV<N>()` after any `.with_*_array(...)` calls. Defaults to 0 when omitted (safe). Reference: [hubfx_esp32s3.ino kPwmPorts](../controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino) shows the three-array case (CH1..8 = 8000, IN_2..12 servos = 5000, IN_1 input = 3300). Full Phase-0 rollout in [22-GUNFX-FEATURE-ROLLOUT.md § Phase 0](../instructions/22-GUNFX-FEATURE-ROLLOUT.md).

### 40. Global Effect Clock — no raw millis() in the effect layer

All effect-layer code (`controllers/hubfx/esp32s3/src/effects/**`, future GunFx ROF scheduler, fan puffing, heater bang-bang, yaw/pitch motion profiles, the existing EngineFx state machine, GearControl backstop arming, landing-light animator …) MUST read time from `sfx_core::EffectClock::instance()` — never raw `millis()` / `micros()`.

**Wiring (mandatory in every controller's `loop()`):**
```cpp
void loop() {
    sfx_core::EffectClock::instance().latch();   // ONCE per pass, BEFORE board.process()
    board.process();
    // ...
}
```

**Usage (in any effect tick):**
```cpp
#include <server/effect_clock.h>
const uint32_t now = sfx_core::EffectClock::instance().nowMs();
const uint32_t dt  = sfx_core::EffectClock::instance().dtMs();   // delta since previous latch
```

**Why:** a single main-loop pass runs EngineFx, GunFx, GearControl, landing-light animator, and potentially a smoke-fan puff scheduler. If each calls `millis()` independently, their notion of "now" drifts by microseconds — a ROF scheduler can fire a second shot before a fan puff has logged the first, motion profiles double-integrate when one reads time before and another after `vTaskDelay`, etc. Latching once at the top of the loop guarantees lockstep behaviour: every effect that ticks within the pass sees the same `nowMs()` and a consistent `dtMs()` for delta-time math (motion-profile `pos += vel * dt`, fade ramps, etc.).

**Scope:** EFFECT LAYER ONLY. Drivers, the bus, the keepalive, the upload state machine, codec init, sense pollers — these all keep using raw `millis()` (their cadence is independent of the effect tick). Adding a clock latch to them adds no value and creates a coupling we don't want.

**Singleton:** `sfx_core::EffectClock` is a function-local-static thread-safe singleton (C++11+); access only via `EffectClock::instance()`. Header: [controllers/lib/sfx_board/server/effect_clock.h](../controllers/lib/sfx_board/server/effect_clock.h). Idempotent within a tick — extra `latch()` calls when `millis()` hasn't advanced are no-ops, so a misordered call doesn't shift the clock mid-tick.

Phase 0.5 of the GunFX rollout introduced the clock + retrofitted 11 call sites in `effects/`. New effects start from this rule; any future `millis()` call in `effects/**` is a Rule 40 violation. Full rollout context: [22-GUNFX-FEATURE-ROLLOUT.md § Phase 0.5](../instructions/22-GUNFX-FEATURE-ROLLOUT.md).

### 42. Actuator Mechanism Lives on the Role, Not the Effect

Anything that describes **how the physical actuator behaves** — voltage scaling for sub-rail elements, motion-profile shaping for servos, stall guard for motors, calibration limits, REV flag — belongs on the **role layer**, not duplicated inside each effect that happens to drive the actuator. Effects stay at the **intent layer** (`setTarget(us)`, `setPct(pct)`, `armSmoke()`, `puff(200ms)`); roles own the integrators, math, and per-attachment metadata; ports own the rail / hardware-safe bounds.

**Why role, not effect:**
- Actuator behaviour is a property of the **physical wiring + the operator's calibration** (the operator soldered a 5 V heater to this header AND set yaw range to ±30° on this servo), not of the effect that commands it. Reattaching the same heater / servo to a different effect, or no effect, doesn't change those properties.
- Roles already know about their port (Phase 0 / Rule 37 gave each port a declared rail voltage; ServoPort knows its hardware bounds). Adding actuator-side mechanism to the role keeps the math co-located with the data it needs.
- This matches Rule 36's separation of "operator intent" from "actuator implementation" — the same principle, applied below the wire instead of above it.

**Two concrete instances (Phase 2 of GunFX rollout, instructions/22):**

1. **Element voltage scaling** (heaters, DC motors driving sub-rail elements):
   - [`sfx_board/element/element_scaling.h`](../controllers/lib/sfx_board/element/element_scaling.h) — shared `ElementConfig { elementMv, mode }` + `scaleDuty(pct, portMaxDuty, portMv, elem)`. Modes: `Passthrough` / `Linear` / `Quadratic`.
   - [`HeaterRole`](../controllers/lib/sfx_board/roles/heater_role.h) + [`DcMotorRole`](../controllers/lib/sfx_board/roles/dc_motor_role.h) gain `setElement(cfg)` + `setPortRailMv(mv)` + intent-level `setDrivePct(pct)` / `setPct(pct)`.
   - `RoleServicePolicy::attachHeater` / `attachDcMotor` consume `[elementMv:u16LE][scaling:u8]` from the role-attach config; `binding.voltageMv` feeds `setPortRailMv` automatically.

2. **Servo motion profile** (trapezoidal speed/accel/jerk shape):
   - [`sfx_board/motion/motion_profile.h`](../controllers/lib/sfx_board/motion/motion_profile.h) — single header with the config struct `ServoMotionProfile { minUs, maxUs, centerUs, inverted, maxSpeed, maxAccel, maxJerk }` AND the runtime integrator `MotionProfile1D` (decel-lookahead + optional jerk-bounded S-curve). They live together because they're tightly coupled — config + the runtime that consumes it.
   - [`ServoActuatorRole`](../controllers/lib/sfx_board/roles/servo_actuator_role.h) owns the integrator (`setProfile(prof)` + `setTarget(us)` + `tick()`). The legacy velocity-only slew is **retired** as of Phase 2.9.
   - `RoleServicePolicy::attachServoActuator` consumes `[minUs][maxUs][maxSpeed][reversed][centerUs][maxAccel][maxJerk]` (Rule 11 append-only — old `[minUs][maxUs][maxVel][reversed]` payloads still attach with zero accel/jerk = velocity-only behaviour as before).

**Live-tuning wire surface (Phase 2.9.x).** Mechanism is **live-tunable** without re-attaching the role (re-attach would lose target / position state). Three matched packet triples on the existing role-command range:

| Slots | SET / GET pair | Carries |
|---|---|---|
| `0x4D`–`0x4F` | `SERVO_SET_PROFILE` / `SERVO_GET_PROFILE_REQ` / `SERVO_PROFILE_RESP` | `[minUs][maxUs][maxSpeed][reversed][centerUs][maxAccel][maxJerk]` — same shape as the role-attach payload tail |
| `0x65`–`0x67` | `MOTOR_SET_ELEMENT` / `MOTOR_GET_ELEMENT_REQ` / `MOTOR_ELEMENT_RESP` | `[elementMv][scaling]` (SET) + read-only `[portRailMv]` (GET response) |
| `0x73`–`0x75` | `HEATER_SET_ELEMENT` / `HEATER_GET_ELEMENT_REQ` / `HEATER_ELEMENT_RESP` | `[elementMv][scaling][drivePct][hyst_cx10]` (SET) + read-only `[portRailMv]` (GET response) |

Set commands are atomic — one packet pushes the full mechanism in one round trip; in-flight `target_us` / `target_cx10` is preserved (re-clamped into the new range). GET responses populate the role-attached read-only `portRailMv` so Studio doesn't need a separate `PORT_LIST_RESP` lookup. Go mirrors in [`app/go/protocol/roles/roles.go`](../app/go/protocol/roles/roles.go) — `CmdServoSetProfile` / `CmdServoGetProfile` / `DecodeServoProfile`, `CmdMotorSetElement` / `DecodeMotorElement`, `CmdHeaterSetElement` / `DecodeHeaterElement`. Studio's port-role row (Phase 4) drags sliders → debounce ~350 ms → push the SET packet → fire-and-confirm via the corresponding GET. New element-driven roles SHOULD follow the same triple shape (SET + GET_REQ + RESP, three slot IDs reserved together).

**Where the per-attachment config lives:**

Operators declare actuator mechanism in `/hubfx.yaml`'s `ports[]` block (the role-attach record), **never** inside an effect's YAML. Examples:

```yaml
ports:
  - port: { kind: pwm, idx: 1 }
    role: heater
    element_mv: 5000          # 5 V heater wired to the 8 V rail
    scaling:    linear        # role computes duty = 5000/8000 = 62.5 %
  - port: { kind: servo, idx: 0 }
    role: servo-actuator
    profile:
      min_us: 1100
      max_us: 1900
      center_us: 1500
      max_speed_us_per_sec:  800
      max_accel_us_per_sec2: 1600
      max_jerk_us_per_sec3:  0    # 0 = trapezoidal (no S-curve)
```

Effect configs (`/gunfx.yaml`, `/enginefx.yaml`, …) reference the port + carry **only the intent** (channel binding, target temp, RPM, …) — never actuator mechanism.

**What effects MUST NOT do:**
- Store actuator-mechanism fields (`elementMv`, `scalingMode`, `maxSpeed`, `maxAccel`, `maxJerk`, `minUs`, `maxUs`, `reversed`) on the effect's config struct. (Earlier drafts of `gunfx::SmokeConfig` had voltage fields; earlier drafts of `gunfx::GunAxis` had motion profile fields — Phase 2 / 2.9 of the GunFX rollout deleted them when this rule was distilled.)
- Compute raw duty values or run a motion-profile integrator in the effect tick. Call `role.setPct(pct)`, `role.setTarget(us)`, `role.setDuty(raw)` (raw-bypass for advanced cases).
- Read `binding.voltageMv` or `port->minMicroseconds()` from inside an effect. Hardware bounds are a role-internal detail.

**When adding a new actuator mechanism (e.g. a stepper-motor microstepping profile, a closed-loop PID for a brushed motor):** put the fields next to the port pointer in the role class, add `setConfig(...)`, and call the math helper whenever you write to the hardware. Operators set the mechanism where they set the role (the port-role attachment), not where they set the effect that uses it. Full Phase-2 context: [22-GUNFX-FEATURE-ROLLOUT.md § Phase 2](../instructions/22-GUNFX-FEATURE-ROLLOUT.md).

### 38. Multi-Band Channel Overlay (Rule 36 extension for discrete selectors)

When an effect uses a channel as a **discrete N-item selector** (gun ROF, future LightFx program selector, gear-set picker), the channel cluster from Rule 36 gets a multi-band overlay variant — replaces the single threshold-marker with N coloured non-overlapping zones, one per item, on the same live bar.

**Required visual elements:**

1. **One coloured zone per item** — `[bandLoUs, bandHiUs]` rendered as a translucent rectangle from `usToPct(lo)` to `usToPct(hi)`. Colour cycles through a small palette (`#5b9dff`, `#ffa05b`, `#5bd28b`, `#d65bd2` — 4 hues handles the 8-item cap with one wrap). Each zone shows the item's `name` centred horizontally when the zone is wide enough; tooltip carries `name · lo-hi µs · rpm`.
2. **Live µs marker** — same green vertical line as Rule 36's `.threshold-mark`, glowing 2 px wide. Sits on top of the zones.
3. **Armed-band glow** — the zone the live value falls into renders an inner accent-coloured outline (`box-shadow: inset 0 0 0 2px var(--accent)`) — "this is the item firing right now".
4. **NO-SIGNAL state** — striped track (same as Rule 34), legend reads `"NO SIGNAL"` or `"no channel bound"`.
5. **Overlap validation** — bands MUST NOT overlap. Detect with O(N²) interval check; on conflict, draw a red diagonal-stripe hatch over the whole bar (`box-shadow: inset 0 0 0 2px var(--error)` + `::after` diagonal-hatch pattern) AND mark the conflicting item rows red with an `⚠ ROF bands overlap (items #X, #Y)` row-error. The bar stays interactive (operator can still tweak); but the Apply button gates on the validator-level error per Rule 35.

**Shared component (MANDATORY 2026-05-23):** factored into [`ChannelBandCluster.svelte`](../app/go/studio/frontend/src/lib/components/ChannelBandCluster.svelte) — props: `channelLabel`, `emptyOption`, `options`, `inputId`, `bands=[{loUs,hiUs,name,meta,color,armed}]`, `overlapIndices`, `liveUs`, `liveValid`, `onInputChange`. The widget **reverse-paints** items so band #1 lands on top of later siblings — this fixes the "first ROF invisible behind a [0,0] catch-all" bug (2026-05-23). It also renders unbounded bands (`lo=0` or `hi=0`) with a diagonal-stripe overlay + `∞` tag so a catch-all item is visible even when a sibling covers it.

**Render gate:** the widget only paints the bar + legend when `inputId !== ''` AND `bands.length > 0`. When the channel isn't bound, the dropdown still renders but the bar is suppressed (no bar = no marker = no false-positive arming hint).

**Companion validation (Rule 35 + per-row commentary):** the panel calling this widget MUST surface per-item errors NEAR each item row (band overlap, inverted `hi <= lo`, RPM out of range, unbounded-with-explicit-siblings) AND aggregate a section-header chip with the error count. Reference: [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) `rofItemIssues()` + `.rof-issues` ul.

**Smart auto-populate when adding items (operator-quality-of-life, 2026-05-23):** the panel's `addItem` mutator MUST seed a NON-overlapping band for the new item (algorithm: find largest gap in `[1000, 2000]`; if no gap ≥ 100 µs, slice the widest existing band in half). Defaulting to `[0, 0]` makes every new item land on top of siblings and look invisible. Reference: [gunfx.ts `suggestNextRofBand`](../app/go/studio/frontend/src/lib/gunfx.ts).

### 39. Optional-Section Yellow Warnings (non-blocking, distinct from errors)

Effect-panel sections that are **optional** (the operator can leave them empty) but currently UNFINDABLE (no candidate port available, or other "would work if a resource existed" state) MUST surface as **yellow warnings**, NOT red errors. Yellow does NOT gate Apply (Rule 35 errors do); it just flags the issue so the operator can see it without being blocked.

**The trigger heuristic:** a section is in the warning state when:
- The section is optional (`Min: 0` in the domain slot, or no required-by-design check),
- AND no port is currently picked for it,
- AND the candidate list (`Candidates(domain, slot)` / `portsOfKind(...)` ) is empty.

If any of those is false, no warning. If all three, render warning.

**Visual:**

```svelte
<div class="section-head" class:section-warn={noFreePortOf('pwm', 'output', cfg.port)}>
    Smoke heater
    {#if noFreePortOf('pwm', 'output', cfg.port)}
        <span class="section-warn-tag">no free PWM port</span>
    {/if}
</div>
```

```css
.section-head.section-warn { color: var(--warning); border-bottom-color: var(--warning); }
.section-warn-tag           { font-size: 9px; font-weight: 700; color: var(--warning);
                              padding: 1px 6px; border: 1px solid var(--warning);
                              border-radius: 3px; letter-spacing: 0.5px; }
```

**Distinct from Rule 35 red errors:**

| | Rule 35 (red error) | Rule 39 (yellow warning) |
|---|---|---|
| Apply button | ❌ disabled | ✅ enabled |
| Source | required fields empty / missing files / invalid wiring | optional section can't find a candidate port |
| Resolution | operator MUST fix before applying | operator CAN apply; firmware just runs without the section |

**Configuration-driven, not policy-driven:** Rule 39 doesn't add a section. It just changes the visual styling for what's already an optional / could-be-empty state. Reference: [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) `noFreePortOf` helper + all five `section-warn` invocations (muzzle / recoil / smoke / yaw / pitch).

### 41. Manual Override / Puppet-Mode Panel (per-effect, optional)

Operational effect panels MAY expose a **manual override / puppet-mode** subsection that drives every firmware-side input directly from the GUI — sliders for axis inputs, hold-buttons for triggers, dropdowns for discrete selectors, on/off chips for binary states. Companion to Rule 36's channel cluster: where Rule 36 shows what the RC stream is *doing* to the firmware, Rule 41 lets the operator BECOME the RC stream for testing.

**Required mechanics:**

1. **Subscribe to verbose status on enable.** The override toggle's `on:change` calls `verboseSubscribe(id, on)` → ~10 Hz async broadcasts populate a `verbose` map keyed by effect id. The panel renders live state from that map.
2. **Send manual commands debounced.** Sliders use ~32 ms debouncing (≈30 Hz) so a fast drag doesn't flood the bus. Buttons / dropdowns fire immediately.
3. **The wire surface uses bitmask flags.** Each manual-set packet carries a `flags` field indicating which subsystems THIS call is touching — firmware leaves untouched subsystems at their prior manual value (or RC-driven for fields never manually set). See `GUN_MANUAL_SET` (gunfx_protocol.h) for the canonical shape.
4. **Auto-release safety.** Firmware reverts to RC after ~5 s of no manual-set (`kManualTimeoutMs`). Studio crash → gun returns to RC automatically. Manual `Release` button + tab unmount call `manualRelease(id)` explicitly.
5. **Gated on `dirty || hasErrors`** (Rule 35). The override toggle is `disabled` whenever the draft is dirty — pushing manual on a stale config tests the OLD firmware behaviour and looks like a bug.
6. **Two-column layout** — left column drives, right column mirror:
   - **Left "Drive" column:** sliders (yaw / pitch / axis sliders), button-or-dropdown selectors (ROF item, smoke arm/disarm), big mouse-hold fire button (`mousedown` → fire=1, `mouseup` → fire=0, `mouseleave` → fire=0 safety).
   - **Right "Live mirror" column:** color-coded rows showing every observable per-subsystem state at ~10 Hz. Green = active, dim = idle, mono font for numerics.

**Fire button visual conventions:**

```svelte
<button class="fire-btn" class:held={live?.firing}
        on:mousedown={() => fireHoldDown(id)}
        on:mouseup={() => fireHoldUp(id)}
        on:mouseleave={() => live?.firing && fireHoldUp(id)}>
    ● {live?.firing ? 'FIRING — release to stop' : 'HOLD TO FIRE'}
</button>
```

`.fire-btn.held` paints translucent red + glow. The CSS lives in [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) — copy verbatim when adding a puppet panel to another effect.

**Live mirror row pattern:**

```svelte
<div class="mirror-row">
    <span class="mlabel">firing</span>
    <span class="mval" class:m-on={live.firing}>{live.firing ? 'YES' : '—'}</span>
    <span class="mlabel">smoke</span>
    <span class="mval" class:m-on={live.smokeArmed}>{live.smokeArmed ? 'ARMED' : '—'}</span>
</div>
```

Mirror rows are READ-ONLY and group logically — don't sprinkle them. Three or four rows max per gun is plenty.

**Use cases:** bench-testing without an RC link, debugging RC-channel mapping, demoing a gun without flying. Operators on the field rarely engage puppet mode — it's a workshop tool.

Reference: [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) manual-control subsection + [gunfx.ts](../app/go/studio/frontend/src/lib/gunfx.ts) `ManualFlag` constants + `pushManual` helper.

### 43. Channel Inputs Are Named (resolved from /hubfx.yaml's `inputs:`)

Every effect that consumes an RC channel (engine throttle toggle, gun fire trigger, gun ROF selector, gun yaw / pitch input, future LightFx mode-switch …) MUST reference that channel by **name** from `/hubfx.yaml`'s `inputs:` block — NEVER by raw `port + channel` in the effect's own YAML.

**Why named, not raw:**

- **One source of truth for "where the throttle channel is."** The operator declares each channel once in `/hubfx.yaml` (`name: throttle_toggle`, source port, channel id, optional description) and every effect refers to it by name. Renumbering a channel (e.g. moving from SBUS slot 5 to slot 7) is a one-line edit in `/hubfx.yaml`; no effect file changes.
- **Decouples physical wiring from effect intent.** GunFx says "fire when `gun_trigger` is high" — it doesn't care that `gun_trigger` happens to be SBUS channel 4 on IN_1 today and might be channel 7 tomorrow. The mapping is a board-level concern, not an effect-level concern.
- **Self-documenting configs.** A reader of `/gunfx.yaml` sees `trigger: { input: gun_trigger }` and understands intent at a glance; raw `port: { kind: input, idx: 0 }, channel: 4` is opaque.
- **Studio surfaces a flat list of named channels in every effect's pickers** — the operator never sees "input port 0 channel 4" inside an effect form. (Picking port + channel happens once in the IO tab when the channel is named; effects only see the name.)

**Where the schema lives:**

```yaml
# /hubfx.yaml — board-level channel naming
inputs:
  - name: engine_toggle
    port: { kind: input, idx: 0 }
    id: 4            # 1-based channel id in the source protocol (SBUS / Jeti / RC-PWM)
    description: "stick 1, switch A — toggles the engine"
  - name: gun_trigger
    port: { kind: input, idx: 0 }
    id: 5
  - name: gun_rof
    port: { kind: input, idx: 0 }
    id: 6
  - name: gun_yaw
    port: { kind: input, idx: 0 }
    id: 7
  - name: gun_pitch
    port: { kind: input, idx: 0 }
    id: 8

# /gunfx.yaml — effect references the channels by name
guns:
  - id: 0
    trigger: { input: gun_trigger, threshold_us: 1500, hysteresis_us: 25 }
    rof:     { input: gun_rof, items: [...] }
    yaw:     { enabled: true, servo_port: { kind: servo, idx: 1 }, input: gun_yaw,   neutral_us: 1500 }
    pitch:   { enabled: true, servo_port: { kind: servo, idx: 2 }, input: gun_pitch, neutral_us: 1500 }
```

**Resolution lives in the apply translator.** Each effect's `applyXxxConfig<>(board, cfg, hubCfg)` walks the spec list and calls `findInputByName(hubCfg, name)` for every `*input` field, populating the resolved `PortRef + channelIdx` on the spec struct BEFORE handing it to the service. Unknown names log a WARN and leave the binding empty (the service skips the dispatcher subscribe). The effect's service NEVER sees the name — only the resolved port + channel. Reference: [enginefx_config.h::toEngineFxServiceConfig](../controllers/hubfx/esp32s3/src/config/enginefx_config.h), [apply_hubfx_config.h::applyGunFxConfig](../controllers/hubfx/esp32s3/src/config/apply_hubfx_config.h).

**Where Studio reads it:** the device-model surfaces both the catalog (`$deviceModel.channelFunctions` — id + label + group) AND the actual assignments (`$deviceModel.inputs[*].channels[*].function`). Every effect panel uses the same `collectChannels` helper to build a flat picker list — see [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) `chanOpts` + [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) `chanOpts` for the canonical implementation. The picker label format is `CH<N> · <human label>` so the operator sees both the channel number and what it does.

**What effects MUST NOT do:**

- Carry `port`/`portRef`/`channel` for an INPUT channel in their own YAML. Output ports (muzzle flash LED, recoil servo, smoke heater) stay as `PortRef` — those ARE per-effect wiring. Inputs are named.
- Resolve names inside the service tick (the apply translator already did the work; the service operates on PortRef + channel only).
- Provide an "advanced: raw port + channel" picker as a fallback. The IO tab is the single place to name a channel.

**When adding a new effect that consumes an RC channel:** define an input name in the effect's YAML schema (single string field), call `findInputByName(hub, name)` in the apply translator, log a WARN + leave port empty on miss. The Studio panel uses `collectChannels` and offers a `<select>` of named options.

### 44. Servo Motion Profile: Per-Servo Storage, Per-Feature UI (storage canonical in /hubfx.yaml; editing surface inline with the feature)

**The rule:** the servo motion profile (`minUs` / `maxUs` / `centerUs` / `reversed` / `maxSpeedUsPerSec` / `maxAccelUsPerSec2` / `maxJerkUsPerSec3`) lives in **`/hubfx.yaml`'s `ports[]` block next to the port + role + label**, because it's a property of the **physical servo** (min/max are mechanical end-stops; speed/accel match the spec sheet of the actuator). The **UI exposes it inline with the feature that drives the servo** (GunFx panel embeds `ServoProfileEditor` next to each axis binding) so the operator never has to context-switch to "tune" a servo — but storage stays canonical so two features can't ship inconsistent profiles for the same port.

The original Rule 42 had it right about storage (the role layer); what it got wrong was making the **edit surface** the IO tab. Rule 44 corrects only the UX: same storage, better editing flow.

**What still lives on the role layer (Rule 42 unchanged):**
- **Element voltage scaling** (`elementMv`, `scaling`) for heaters + DC motors — hardware fact (the element's rated voltage), not an effect preference. Editing surface remains the IO tab's `PortRoleConfig.svelte` (a heater's element isn't a "feature" property — it's the heater itself).
- The role-layer **math** (`MotionProfile1D` integrator, `scaleDuty()` helper).

**Storage in `/hubfx.yaml`:**

```yaml
ports:
  - kind: servo
    idx: 0
    role: servo-actuator
    label: yaw
    profile:                          # ← per-servo, canonical
      min_us: 1100
      max_us: 1900
      center_us: 1500
      max_speed_us_per_sec:  800
      max_accel_us_per_sec2: 1600
      max_jerk_us_per_sec3:  0
```

The firmware reads the profile when it loads `/hubfx.yaml` and passes it through the **role-attach payload** (Rule 11 append-only `[minUs:u16][maxUs:u16][maxSpeed:u16][reversed:u8][centerUs:u16][maxAccel:u16][maxJerk:u16]`); `RoleServicePolicy::attachServoActuator` applies it during attach so the role is ready before any effect sends `SERVO_SET_TARGET`.

**Effect-config YAMLs (`/gunfx.yaml`, `/enginefx.yaml`, …) MUST NOT carry servo profile fields.** They reference the servo by port only; the role already has the profile loaded.

**UI surface:** the GunFx panel reads the profile from `$deviceModel.ports[i].profile` (looked up by the gun's `servoPort` ref), embeds `<ServoProfileEditor profile={port.profile}>` inline, and on `change`:
1. Pushes via `ServoSetProfile()` debounced ~350 ms (live preview — operator sees movement update before saving).
2. Calls `SetPortProfile(guid, kind, idx, profile)` to update Studio's overlay and mark `/hubfx.yaml` dirty.
3. Save: `SaveHubConfig()` writes the profile back into `/hubfx.yaml`'s ports[] entry.

The IO tab's `PortRoleConfig.svelte` does **not** show a servo motion-profile editor (would create a second authoring surface for the same data). Heater / DC motor element scaling editors stay there (Rule 42 — different data, role-side).

**When two features use the same servo:** still impossible (port claim is exclusive). One profile per servo, one feature using it; no conflict.

References: [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) Turret section, [ServoProfileEditor.svelte](../app/go/studio/frontend/src/lib/components/ServoProfileEditor.svelte), [hubfx_config.h `populate()`](../controllers/hubfx/esp32s3/src/config/hubfx_config.h) profile parser, [role_service.cpp `attachServoActuator`](../controllers/lib/sfx_board/server/role_service.cpp).

### 45. Effect-Panel Header Cluster: [Enable-Button] [Apply] [dirty-flag] (no scattered toggles)

Every effect panel (EngineFx, GunFx, future LightFx / GearControl panels)
puts the **enable/disable affordance**, the **Apply button**, and the
**dirty flag** in a single contiguous header cluster — operator's eye
goes one place to (a) flip the effect on/off, (b) commit changes, and
(c) see whether the firmware reflects the draft.

**Required layout** (left-to-right, inside `.card-header > .header-actions`):

```
[ ▶ Disabled / ✓ Enabled ]   [✓ Apply]   [dirty-flag pill]   …other actions
```

**The enable affordance is a BUTTON, not a checkbox**:

- Checkboxes read like "tick this if you want it on"; an effect-enable
  is a deliberate state change with downstream consequences (Apply
  pushes it; firmware re-attaches roles; live RC bars start moving).
- A button reads as an action.  Reuses the design-system `button` class
  + a `state` modifier (`.btn-state-on` / `.btn-state-off`) — the
  toggle's current state is the LABEL, not a checkmark on a separate
  control.

```svelte
<button class="small state-toggle" class:state-on={cfg.enabled}
        on:click={() => setEnabled(!cfg.enabled)} disabled={busy}
        title={cfg.enabled ? 'Disable this effect — Apply to push' : 'Enable this effect — Apply to push'}>
    {cfg.enabled ? '✓ Enabled' : '▶ Disabled'}
</button>
<button class="small primary" on:click={onApply}
        disabled={busy || !$effectDirty || hasErrors}
        title={hasErrors ? 'Resolve validation errors first' : 'Save + reload'}>✓ Apply</button>
<span class="dirty-flag" class:on={$effectDirty} class:err={hasErrors}>
    {hasErrors ? 'resolve errors' : $effectDirty ? 'unapplied changes' : 'in sync'}
</span>
```

**Rationale for putting Apply NEXT TO the enable toggle:**

- The enable-toggle change is a draft mutation — pressing it dirties
  the panel but doesn't push to firmware.  Without an adjacent Apply,
  the operator has to hunt for the persist control (across the card,
  or scroll up to the panel header).
- Same row as the dirty flag means the operator sees the "you need to
  Apply" cue right next to the action that satisfies it.
- Matches the existing EnginePanel `status-row` pattern (Rule 35) where
  Apply lives next to the operational buttons; this rule extends the
  pattern UPWARD to the panel-header enable toggle.

**What this replaces:**

- Standalone `<label class="enable-toggle"><input type="checkbox">` rows
  scattered in panel headers (was the GunFx pattern pre-2026-05-23).
- Apply buttons hidden inside a Save dialog or pushed to the bottom of
  the card.

**Reference implementation:** [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) header `.header-actions`.  The shared `.state-toggle` button styling lives in [style.css](../app/go/studio/frontend/src/style.css) so EngineFx and any future effect panels reuse it.

### 46. Modular Config Sources: domain owns the lifecycle; panels are pure views

Studio centralises Apply / dirty-tracking / validation in one global
toolbar (`ConfigToolbar.svelte` above the tab strip).  Every persistent
config (`/hubfx.yaml`, `/enginefx.yaml`, `/gunfx.yaml`, future
`/lightfx.yaml`, `/gearcontrol.yaml`, `/lightfx_programs/*.yaml`, …)
plugs into that toolbar through a **`DirtySource`** descriptor.

**The rule:** each domain module owns a complete `DirtySource` export.
Panels register zero, validate zero, apply zero — they are pure views
on the underlying stores.

**Domain module contract** (e.g. `lib/gunfx.ts`, `lib/effects.ts`,
`lib/devicemodel.ts`, future `lib/lightfx.ts`):

1. **Stores** — `xxxConfig` (truth-from-device) + `xxxDraft` (working
   copy) + `xxxDirty` (derived diff).
2. **Loaders** — `loadXxxConfig()` (Wails download → populate stores),
   `applyXxxConfig()` (serialise → Wails upload → reload).
3. **Validation** — `xxxHasErrors: Readable<boolean>` derived from the
   draft (pure) OR a writable kept fresh by `scheduleXxxValidate()`
   (async — file-exists checks etc.).
4. **Source export** — `xxxConfigSource: DirtySource = { id, label,
   isDirty, hasErrors, apply, refresh }`.

```ts
// lib/gunfx.ts (pattern)
import type { DirtySource } from './dirty-registry'

export const gunfxDirty     = derived(...)
export const gunfxHasErrors = derived(gunfxDraft, ($d) =>
    $d.guns.some(g => detectBandOverlaps(g.rof.items).length > 0))

export const gunfxConfigSource: DirtySource = {
    id:        'gunfx',
    label:     'GunFX',
    isDirty:   gunfxDirty,
    hasErrors: gunfxHasErrors,
    apply:     saveGunFxConfig,
    refresh:   loadGunFxConfig,
}
```

**Registration** lives in `App.svelte`'s `onMount`, NOT in the panel.
Register order = apply order = dependency order (hubconfig FIRST
because effect translators resolve named inputs against
`/hubfx.yaml`).  Adding a new effect = one line in `App.svelte`:

```ts
registerDirtySource(hubConfigSource)
registerDirtySource(engineConfigSource)
registerDirtySource(gunfxConfigSource)
registerDirtySource(lightfxConfigSource)        // <-- new effect, append here
registerDirtySource(lightfxProgramsConfigSource)
registerDirtySource(gearcontrolConfigSource)
```

**Why panels don't register themselves:**

- Apply order would otherwise depend on which tab the operator opens
  first — flaky and surprising.
- Sources stay registered across tab switches; the registry doesn't
  thrash on every navigation.
- Panel components are testable in isolation (no side effects in
  `onMount`).
- New panels for an existing domain (e.g. a future "advanced GunFx"
  tab) share the SAME source — no duplicate dirty-state.

**Panel contract** (post Rule 46):

- Subscribe to draft / config / status stores.
- Wire up mutations (every `on:change` updates the draft via
  `domainStore.set(...)`).
- Show local field-level validation cues (red borders, warning tags
  per row — Rule 35 still applies for the UI).
- Render the enable-toggle (Rule 45) + operational buttons
  (Start/Stop/Test).
- **Do NOT** render an Apply button, dirty-flag, or Refresh button —
  the global toolbar owns those.
- **Do NOT** register with the dirty-registry — the domain module
  exports the source, `App.svelte` registers it.

**`useConfigSource(src)` convenience** is available for ad-hoc /
experimental panels that need their own non-startup registration — it
handles the `onMount` + return-cleanup pattern.  Production panels for
the canonical effects should not use it; pre-registration in
`App.svelte` is the standard path.

**Cross-config validation** — when an effect references something in
another file (e.g. GunFx `trigger.input: "gun_trigger"` referring to a
channel in `/hubfx.yaml` inputs[]), the validation derived store
should subscribe to BOTH `xxxDraft` AND `$deviceModel.channelFunctions`
(or equivalent) and flag missing references as errors.  The aggregate
`anyErrors` in the global toolbar then catches cross-file rot.

Reference: [dirty-registry.ts](../app/go/studio/frontend/src/lib/dirty-registry.ts), [ConfigToolbar.svelte](../app/go/studio/frontend/src/lib/layout/ConfigToolbar.svelte), [App.svelte](../app/go/studio/frontend/src/App.svelte) onMount registration block, [gunfx.ts `gunfxConfigSource`](../app/go/studio/frontend/src/lib/gunfx.ts), [effects.ts `engineConfigSource`](../app/go/studio/frontend/src/lib/effects.ts), [devicemodel.ts `hubConfigSource`](../app/go/studio/frontend/src/lib/devicemodel.ts).

### 47. Shared Sound Row + Speaker-Routing Widget (Rule 34 sub-rule)

Every Studio panel that lets the operator pick a WAV file from SD AND choose its L / R / Stereo routing — engine starting/running/stopping sounds, GunFx per-ROF sound, future LightFx mode sounds — MUST use the shared [`SoundRow.svelte`](../app/go/studio/frontend/src/lib/components/SoundRow.svelte) component instead of inlining the row. The widget owns:

- The `.field-label`-prefixed wide text input (operator can hand-type a path)
- The browse (`…`) + Clear button slots in the Rule 34 order, with the speaker button as the **rightmost** segment so the routing column always aligns across required rows (Clear → hidden `.btn-spacer`) and optional rows (Clear → real button); `.btn-slot` + `.btn-spacer` are global classes in [`style.css`](../app/go/studio/frontend/src/style.css) so the shared component inherits the dimensions
- The speaker-routing button — short label IN THE BUTTON (`L` / `R` / `L+R`, via `routeShortLabel()`) plus an inline speaker SVG (`speakerIcon()`) so the state reads at a glance without expanding the slot; cycles Stereo → Left → Right → Stereo on click; STAYS enabled even when the sound path is empty (routing is a property of the slot, not the file — the operator can pre-select where the next browsed file will play)
- An optional `<slot name="lead">` for panels that need to prefix the row (GunFx injects a `.rof-idx-pill placeholder` so the row column-aligns with the #N badge above)

**Speaker glyph + mask helpers** live in [`speaker_routing.ts`](../app/go/studio/frontend/src/lib/components/speaker_routing.ts) — `MASK_LEFT = 0x01` / `MASK_RIGHT = 0x02` / `MASK_STEREO = 0x03` (matches the firmware `AudioChannel` enum); `cycleOutputMask()`, `speakerLabel()` (long form, for tooltips/aria), `routeShortLabel()` (`L`/`R`/`L+R`, on-button), `speakerIcon()`, `speakerStateClass()`. Single source of truth so the wire mask + the user-facing label + the colour cue never drift.

**Colour cue** (matches Rule 38 / 36 legend): stereo = `--accent`, left = `--warning`, right = `--success`. Operator with both the sound rows AND a band cluster open can match a band's routing colour to the routing button's colour without re-reading.

Reference: [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) sound rows (all three bind to the engine-level `cfg.output` via `maskFromOutput`/`outputFromMask` helpers — engine plays one sound at a time so all rows share one mask), [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) per-ROF sound (per-row `outputMask`). Inlining the row markup + speaker SVG again is a Rule 47 violation; extend the component instead.

### 48. Operational Action Cluster — `.op-cluster` split-button (primary + optional picker + Stop)

Operational effect panels (GunFx Fire/Stop, EngineFx Start/Stop, future LightFx test, GearControl manual jog, …) MUST group the primary action + its optional modifier picker + the Stop / cutoff into a SINGLE visual control via the **global** `.op-cluster` class (defined once in [`style.css`](../app/go/studio/frontend/src/style.css)). Replaces a row of loose buttons + a detached `<select>` with one connected segment group so the emergency cutoff sits right next to the action that produced it — operators don't hunt across rows for the Stop.

```svelte
<!-- GunFx: Fire (auto-fire) + ROF picker + Stop as one cluster -->
<div class="op-cluster">
    <button class="oc-btn oc-primary" on:click={() => gunStartFiringWithRof(id, 0, pickedRof)}
            disabled={busy || $gunfxDirty || gun.rof.items.length === 0}>▶ Fire</button>
    <select class="oc-picker" value={pickedRof} on:change={…}>
        <option value={ROF_ARMED}>RC</option>
        {#each gun.rof.items as item, i}
            <option value={i} title="{item.name} · {item.rpm} rpm">#{i + 1}</option>
        {/each}
    </select>
    <button class="oc-btn oc-danger" on:click={() => gunStopFiring(id)} disabled={busy}>■ Stop</button>
</div>

<!-- EnginePanel: Start + Stop, no picker -->
<div class="op-cluster">
    <button class="oc-btn oc-primary" on:click={onStart} disabled={busy || $engineDirty || soundsHaveErrors}>▶ Start</button>
    <button class="oc-btn oc-danger" on:click={onStop} disabled={busy}>■ Stop</button>
</div>
```

**Required classes** (all global, all in `style.css`):

- `.op-cluster` — flex wrapper, 28 px height, shared 1 px outer border, 4 px radius, no internal gap; 1 px divider between segments via `> *:not(:first-child)`
- `.oc-btn` — segment button (no border, no radius, 11 px font, 600 weight)
- `.oc-btn.oc-primary` — `--success` text (the start/fire action)
- `.oc-btn.oc-danger` — `--error` text + red-tinted hover (the stop/cutoff)
- `.oc-picker` — narrow inline `<select>` (≥ 56 px), monospace, custom CSS chevron drawn with two linear-gradients because `appearance: none` strips the native arrow

**UX invariants** (mandatory):

- **Danger / Stop is ALWAYS the rightmost segment AND always enabled** (no `dirty` / `errors` gate). It's the safety switch — the operator must always be able to stop.
- **Primary** segment carries the Rule 35 gate (`busy || $dirty || hasErrors`) — running on a stale draft tests the OLD firmware config and looks like a bug.
- **Picker** (when present) sits BETWEEN primary and danger — it's a modifier on the primary action, visually flanked by the action and the cutoff.
- **Closed-state picker text stays short** (≤ 5 chars: `RC`, `#1`, `#2`, …). Verbose names + units live in `<option title="…">` tooltips so the cluster doesn't expand horizontally when the operator picks a verbose option.
- One cluster per panel area: don't split Fire/Stop across two clusters or stack a cluster on top of loose buttons. If the panel needs an unrelated action (Smoke On/Off, Remove), it lives OUTSIDE the cluster.

References: [GunFxPanel.svelte](../app/go/studio/frontend/src/lib/tabs/GunFxPanel.svelte) per-gun fire cluster, [EnginePanel.svelte](../app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte) status-row. New operational panels copy the markup verbatim — re-rolling a one-off button row is a Rule 48 violation.

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
- **sfx_boards/** - Board-specific client/server protocol implementations (GunFX, LightFX, GearControl)
- **esp_cdc_acm/** - Vendored ESP-IDF USB Host CDC-ACM class driver (ESP32-S3 only)

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

STATUS response = 22-byte core header `[counter:u32][uptime:u32][freeRam:u32][lastActivity_ms:u32][keepaliveCount:u32][boardState:u8][initFlags:u8]` + module callback data.

Board states: `IDLE(0x00)` — no config loaded, `STANDALONE(0x01)` — config loaded from flash, `SLAVE(0x02)` — INIT slave mode, `DIRECT(0x03)` — INIT direct/config mode.

INIT_READY payload = length-prefixed binary: `[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE][capabilities:u32LE]`

The trailing `capabilities` u32 is a Rule 11 append-only bitmask (`CoreCapability::FLASH | SD | AUDIO | USB_HOST | ENGINE | CONFIG | SLAVE_BUS`) the firmware uses to advertise which optional interfaces it exposes. Clients (CLI, Studio, file manager) gate UI and probes on these bits — e.g. Studio's `FsStorageStatus` skips the SD query when `CAP_SD` is not set, and the file manager hides the SD tab. A 0 bitmask means "legacy firmware that pre-dates the field" → fall back to probing rather than treating as "nothing supported". Wired in each `setup()` after the relevant module's `begin()` succeeds via `server.core().addCapability(...)`. Mirror is `core.Cap*` in [app/go/protocol/core/core.go](../app/go/protocol/core/core.go).

IDENTIFY (0xFE) returns the same payload as INIT_READY but without triggering init callbacks or state changes. The CLI uses IDENTIFY on connect to discover the board type:
- **HubFX** (auto-initializes on boot): IDENTIFY only — no INIT sent
- **Slave controllers**: HubFX uses IDENTIFY for discovery (marks slaves as "connected"). INIT is sent as a separate activation step via SLAVE_INIT command.
- **CLI direct connect**: IDENTIFY to detect type, then INIT to activate hardware
- **Fallback**: if IDENTIFY fails, CLI falls back to INIT (for legacy firmware)

See `controllers/lib/sfx_serial/serial/PROTOCOL.md` for full wire format.

### Go CLI (`app/go/`)

Five-package architecture: `protocol/` (wire format, per-module subpackages), `api/` (typed client SDK), `engine/` (shared command engine + handlers), `cli/` (thin terminal wrapper), `flash/` (standalone build/flash tool).

```
app/go/
├── go.mod                 - Module: scalefx
├── protocol/
│   ├── wire.go            - CRC-8/CRC-16, COBS encode/decode, packet build/parse
│   ├── types.go           - PacketType, ErrorCode types, name registry
│   ├── stream.go          - Stream protocol (chunked data, CRC-16)
│   ├── connection.go      - Serial connection, tag-correlated send/receive, stream waiters
│   ├── core/core.go       - Core packet types, generic error codes (mirrors core/core.h)
│   ├── gunfx/gunfx.go     - GunFX packet types, error codes, commands (mirrors gunfx.h)
│   ├── lightfx/lightfx.go - LightFX packet types, error codes, commands (mirrors lightfx.h)
│   ├── gearcontrol/gearcontrol.go - GearControl packets, errors, commands (mirrors gearcontrol.h)
│   └── hubfx/hubfx.go     - HubFX packet types, error codes, commands (mirrors hubfx.h)
├── api/
│   ├── result.go          - ApiResult types
│   ├── client.go          - apiClient base (wraps protocol.Connection)
│   ├── core.go            - CoreApi (init, status, reboot, identify)
│   ├── gunfx.go           - GunFxApi (trigger, servo, smoke)
│   ├── lightfx.go         - LightFxApi (LED, sequences, servo, landing lights)
│   ├── gearcontrol.go     - GearControlApi (gear, servo, yaw, calibration)
│   ├── hubfx.go           - HubFxApi (slaves, audio, engine, storage, USB)
│   └── files.go           - FileApi (SD/flash file operations)
├── engine/                - Shared command engine (used by both CLI and GUI)
│   ├── engine.go          - Core Engine struct (connection, API, dispatch, listener)
│   ├── types.go           - CmdEntry, CmdGroup, InitReadyInfo, ControllerColors
│   ├── output.go          - Output interface + ANSI terminal implementation
│   ├── helpers.go         - Shared utilities (Atoi, ParseBool, ServoSet, ServoConfig)
│   ├── parsers.go         - Common response parsers
│   ├── parsers_core.go    - Core response parsers (INIT_READY, STATUS header)
│   └── handlers/
│       ├── handlers.go        - RegisterDefaults() — registers all built-in groups
│       ├── core/handler.go    - Core commands (connect, init, status, reboot, etc.)
│       ├── gunfx/handler.go   - GunFX commands (trigger, servo, smoke)
│       ├── lightfx/
│       │   ├── handler.go     - LightFX commands (LED, sequences, servo)
│       │   └── parsers.go     - LightFX response parsers
│       ├── gearcontrol/
│       │   ├── handler.go     - GearControl commands (gear, servo, yaw)
│       │   └── parsers.go     - GearControl response parsers
│       ├── hubfx/
│       │   ├── handler.go     - HubFX commands (slaves, audio, engine, storage, USB)
│       │   ├── parsers.go     - HubFX response parsers
│       │   └── format.go      - HubFX output formatting
│       └── firmware/handler.go - Firmware release commands
├── firmware/              - Build/flash logic (shared by flash CLI)
│   ├── build.go, detect.go, firmware.go
│   ├── flash_esp32.go, flash_pico.go
│   ├── releases.go, verify.go, esptool.go
├── flash/                 - Flash CLI (standalone binary)
│   ├── main.go, commands.go, interactive.go, output.go
├── cli/                   - Thin CLI wrapper
│   ├── main.go            - Entry point, flag parsing
│   └── cli.go             - Terminal readline loop, delegates to engine
└── studio/                - ScaleFX Studio GUI (Wails v2)
    ├── app.go, console_output.go, main.go
    └── frontend/          - Svelte frontend
```

**Build:** `cd app/go && go build -o scalefx-cli.exe ./cli/` (single static binary, zero runtime deps)

## Packet Type Allocation

| Range | Module | Status | Notes |
|-------|--------|--------|-------|
| 0x01-0x2F | GunFX | Used | Trigger, servo, smoke |
| 0x30-0x3F | Reserved | - | Future expansion |
| 0x40-0x5F | LightFX | Used | LED, servo, power |
| 0x60-0x7F | GearControl | Used | Gear, servo, yaw |
| 0x80-0xAF | HubFX | Used | Slaves, audio, engine, config (0x90-0x92, 0xAC), SD, flash, files, USB diag, USB reset, tree, slave info |
| 0xA4-0xA6 | Streaming | Used | STREAM_BEGIN/DATA/END (`core/stream.h`) |
| 0xB0-0xEE | Available | Free | New controllers |
| 0xEF-0xFF | Core | Reserved | STATUS_UPDATE (0xEF), INIT, ACK, NACK, REBOOT, IDENTIFY (0xFE), LOG_MESSAGE (0xFD), DIAG_HISTORY (0xFF) |

## Platform-Specific Notes

### Pico Controllers (C++17, Arduino)
- Build with PlatformIO via Flash CLI: `app/go/scalefx-flash.exe build gunfx`
- Framework: Arduino-Pico (Earle Philhower core)
- Key libraries: Servo, Wire (I2C), SD (FatFS), USB Host (PIO-USB for HubFX)
- BOOTSEL mode: Send `BOOTSEL` command via serial for firmware updates

## Common Workflows

See the detailed workflow guides in `/instructions/`:
- **Adding a command:** `03-PROTOCOL-EXTENSION.md` + `04-CHANGE-PROPAGATION.md`
- **Creating a controller:** `02-NEW-CONTROLLER.md`
- **Building/flashing:** `05-BUILD-AND-FLASH.md`
- **Updating CLI:** `07-CLI-UPDATES.md`
- **AudioTools library:** `08-AUDIOTOOLS.md`
- **Console output schema:** `09-CONSOLE-OUTPUT.md`
- **System architecture:** `01-ARCHITECTURE.md`
