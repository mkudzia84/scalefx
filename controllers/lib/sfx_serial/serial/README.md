# sfx_serial — Serial Protocol Library

Binary COBS serial communication library for ScaleFX controllers. Provides the complete protocol stack from wire encoding to domain-specific command handlers.

**Used by:** All controllers (GunFX, LightFX, GearControl, NoOp as servers; HubFX ESP32-S3 as client).

## Files

### Core Layer

| File | Lines | Purpose |
|------|-------|---------|
| `serial.h` | 75 | Umbrella include (everything except hubfx.h) |
| `core/core.h` | 666 | `CoreProtocol` aliases, `SerialError`, `CommandResult`, `ICommandHandler`, `CommandRouter`, SFX_* macros |
| `core/core.cpp` | 119 | `packetTypeToText()`, `CorePayload` encode/decode |
| `core/bus_server.h` | 332 | `BusServer` base class + `CoreCommandServer` (0xF0-0xFF) |
| `core/bus_server.cpp` | 318 | Server implementations (ACK/NACK, STATUS, I2C_SCAN, DIAG_HISTORY) |
| `core/stream.h` | 204 | `StreamProtocol` (0xA4-0xA6), `StreamWriter` for chunked data |
| `core/stream.cpp` | 118 | StreamWriter + CRC-16/CCITT implementation |

### Client Layer (HubFX only)

| File | Lines | Purpose |
|------|-------|---------|
| `client/bus.h` | 115 | `SerialBus` — COBS framing over USB CDC |
| `client/bus.cpp` | 156 | SerialBus implementation (guarded by `#ifndef SCALEFX_SERVER`) |
| `client/bus_client.h` | 193 | `BusClient` base — INIT handshake, tag correlation, version check |
| `client/bus_client.cpp` | 155 | BusClient implementation (ACK/NACK/INIT_READY/LOG_MESSAGE parsing) |
| `client/result_queue.h` | 176 | `ResultQueue` — tag-correlated command/response matching |
| `client/result_queue.cpp` | 134 | ResultQueue implementation (blocking wait, async callbacks, stash) |

### Module Handlers

| File | Lines | Purpose |
|------|-------|---------|
| `gunfx/gunfx.h` | 335 | `GunFxServer`/`GunFxClient`, `GunFxPacket` (0x01-0x2F), `GunFxError`, `GunFxSpec` |
| `gunfx/gunfx.cpp` | 230 | GunFX server switch/client methods (trigger, servo, smoke) |
| `lightfx/lightfx.h` | 458 | `LightFxServer`/`LightFxClient`, `LightFxPacket` (0x40-0x5F), `LightFxError`, `LightFxSpec` |
| `lightfx/lightfx.cpp` | 543 | LightFX server switch/client methods (LED sequences, landing lights) |
| `gearcontrol/gearcontrol.h` | 643 | `GearControlServer`/`GearControlClient`, `GearControlPacket` (0x60-0x7F), `GearControlError`, `GearControlSpec` |
| `gearcontrol/gearcontrol.cpp` | 498 | GearControl server switch/client methods (gear, calibration, yaw) |
| `hubfx/hubfx.h` | ~460 | `HubFxAudioClient`/`HubFxStorageClient`, `HubFxPacket` (0x80-0xA8), `HubFxError` (no .cpp — server impl lives in ESP32-S3 controller) |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    serial.h (umbrella)                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              core/core.h                              │  │
│  │  ┌─────────────────────────────────────────────────┐  │  │
│  │  │  CoreProtocol namespace                         │  │  │
│  │  │  (re-exports SfxWire from sfx_platform)         │  │  │
│  │  │  crc8, cobsEncode/Decode, buildPacket,          │  │  │
│  │  │  encodePacket, parsePacket, endian helpers      │  │  │
│  │  └─────────────────────────────────────────────────┘  │  │
│  │  SerialError      CommandResult     CorePacket        │  │
│  │  ICommandHandler  CommandRouter     SFX_* macros      │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌─────────────────────┐    ┌──────────────────────────┐   │
│  │  core/bus_server.h  │    │  core/stream.h           │   │
│  │  BusServer base     │    │  StreamProtocol 0xA4-A6  │   │
│  │  CoreCommandServer  │    │  StreamWriter             │   │
│  │  (server side)      │    │  CRC-16/CCITT            │   │
│  └─────────────────────┘    └──────────────────────────┘   │
│                                                             │
│  ┌─────────────────────┐    ┌──────────────────────────┐   │
│  │  client/bus.h       │    │  client/result_queue.h   │   │
│  │  SerialBus          │    │  ResultQueue             │   │
│  │  (USB CDC transport)│    │  (tag correlation)       │   │
│  ├─────────────────────┤    └──────────────────────────┘   │
│  │  client/bus_client.h│                                    │
│  │  BusClient base     │                                    │
│  │  (client side)      │                                    │
│  └─────────────────────┘                                    │
│                                                             │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌─────────┐ │
│  │  gunfx/   │  │ lightfx/  │  │ gearctrl/ │  │ hubfx/  │ │
│  │  Server   │  │  Server   │  │  Server   │  │ Client  │ │
│  │  Client   │  │  Client   │  │  Client   │  │  only   │ │
│  │ 0x01-0x2F │  │ 0x40-0x5F │  │ 0x60-0x7F │  │0x80-0xA8│ │
│  └───────────┘  └───────────┘  └───────────┘  └─────────┘ │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Layering

```
  Application Layer    Module handlers (GunFxServer, LightFxClient, etc.)
                       ↕ callbacks, CommandResult
  Protocol Layer       BusServer / BusClient (ACK/NACK, tag routing)
                       CoreCommandServer (INIT, STATUS, REBOOT)
                       CommandRouter (handler chain, packet dispatch)
                       StreamWriter (chunked data streaming)
                       ↕ sendRawPacket(), tryProcess()
  Encoding Layer       CoreProtocol → SfxWire (sfx_platform)
                       CRC-8, COBS encode/decode, endian helpers
                       ↕ raw bytes
  Transport Layer      Stream* (server) / UsbHost CDC (client)
```

## Wire Format

```
Packet: [type:u8][tag:u8][len:u16LE][payload:0-512 bytes][crc8:u8]
Frame:  COBS-encoded packet + 0x00 delimiter
```

| Field | Size | Description |
|-------|------|-------------|
| type | 1 | Packet type identifier |
| tag | 1 | Correlation tag (0=async, 1-255=request/response) |
| len | 2 | Payload length (little-endian) |
| payload | 0-512 | Command-specific data |
| crc8 | 1 | CRC-8 (poly 0x07) over type+tag+len+payload |

### Packet Type Ranges

| Range | Module | Server Class | Client Class |
|-------|--------|-------------|--------------|
| 0x01-0x2F | GunFX | `GunFxServer` | `GunFxClient` |
| 0x40-0x5F | LightFX | `LightFxServer` | `LightFxClient` |
| 0x60-0x7F | GearControl | `GearControlServer` | `GearControlClient` |
| 0x80-0xA8 | HubFX | (in ESP32-S3 firmware) | `HubFxAudioClient`, `HubFxStorageClient` |
| 0xA4-0xA6 | Streaming | `StreamWriter` | — |
| 0xF0-0xFF | Core | `CoreCommandServer` | `BusClient` (base) |

See [PROTOCOL.md](PROTOCOL.md) for full wire format documentation.

## Class Hierarchy

### Server Side

```
ICommandHandler (interface)
  │  tryProcess(type, payload, len) → CommandHandleResult
  │  handlerName() → const char*
  │
  └── BusServer (base: range check, ACK/NACK, sendRawPacket)
        │  begin(Stream*) / end()
        │  Pure virtual: handleModulePacket(), moduleRangeLow/High()
        │
        ├── CoreCommandServer (0xF0-0xFF)
        │     INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE,
        │     STATUS_REQ, IDENTIFY, I2C_SCAN, DIAG_HISTORY
        │
        ├── GunFxServer (0x01-0x2F)
        ├── LightFxServer (0x40-0x5F)
        └── GearControlServer (0x60-0x7F)

StreamWriter (uses BusServer for packet output)
  │  begin(totalBytes) / write(data, len) / printf(fmt, ...) / end()
  └── Uses StreamProtocol::STREAM_BEGIN/DATA/END (0xA4-0xA6)
```

### Client Side

```
SerialBus (COBS transport over USB CDC via UsbHost)
  │  begin(deviceIndex) / process()
  │  sendPacket(type, payload, len, tag)
  │
  └── BusClient (INIT handshake, ACK/NACK/LOG handling, ResultQueue)
        │  sendCommand(type, payload, len) → CommandResult
        │  Virtual: onModulePacket(), onServerReady()
        │
        ├── GunFxClient
        ├── LightFxClient
        ├── GearControlClient
        ├── HubFxAudioClient
        └── HubFxStorageClient
```

## Key Patterns

### SFX Handler Macros

All server `handleModulePacket()` cases use these macros to eliminate boilerplate:

```cpp
case GunFxPacket::TRIGGER_ON:
    SFX_REQUIRE_LEN(2);                    // NACK MISSING_PARAMETER if len < 2
    SFX_VALIDATE(rpm > 0, GunFxError::TRIGGER_INVALID_RPM);  // NACK if invalid
    SFX_DISPATCH(_onTriggerOn, rpm);        // Call callback, ACK/NACK on result
    break;

case GunFxPacket::SRV_SET:
    SFX_HANDLE_CHANNEL_CMD(id, GunFxError::SERVO_INVALID_ID, _onServoSet);
    break;
```

### Tag Correlation (Command → Response)

```
Client                          Server
sendCommand(type, payload)
  → tag = resultQueue.nextTag()
  → sendPacket(type, tag, payload)
  → waitForTag(tag, processFunc)     →  tryProcess(type, payload, len)
                                         tag stored via setCurrentTag()
                                         handleModulePacket()
                                     ←   sendAck(tag) or sendNack(tag, error)
  ← resultQueue.resolve(tag, result)
  return CommandResult
```

### Response Categories

| Category | Server Pattern | Client Pattern | Tag Resolution |
|----------|---------------|----------------|----------------|
| **Instant** | `SFX_DISPATCH` → ACK/NACK | `sendCommand()` returns result | Automatic (BusClient base) |
| **Query** | Custom data response packet | `sendCommand()` + `onModulePacket()` | Manual (resolve in `onModulePacket()`) |
| **Long-Running** | Immediate ACK + later async | `sendCommand()` + callbacks | ACK = automatic, completion = async callback |

### CommandRouter Handler Chain

```cpp
CommandRouter _router;
_router.addHandler(&coreServer);    // Priority 1 (always first)
_router.addHandler(&gunfxServer);   // Priority 2

// On each packet: tries coreServer first, then gunfxServer
// First handler returning Handled wins; NotMyCommand = try next
```

### Streaming Protocol (Large Data)

```
STREAM_BEGIN  → [totalBytes:u32LE]
STREAM_DATA   → [seqNum:u16LE][crc16:u16LE][data:0-508]  (repeats)
STREAM_END    → [totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]
```

Used for file downloads, diagnostic dumps, or any data exceeding MAX_PAYLOAD_SIZE (platform-specific: 512 on Pico, 2048 on ESP32).

## Error Code Ranges

| Range | Namespace | Scope |
|-------|-----------|-------|
| 0x00-0x0F | `SerialError` | General (OK, UNKNOWN, INVALID_COMMAND, BUSY, ...) |
| 0x10-0x1F | `SerialError` | Parameter validation (INVALID_PARAM, OUT_OF_RANGE, ...) |
| 0x20-0x4F | `GunFxError` | Servo (0x20-0x23), Smoke (0x30-0x34), Trigger (0x40-0x42) |
| 0x50-0x5F | `LightFxError` | LED/Servo (0x50-0x56) |
| 0x60-0x6F | `GearControlError` | Gear/Motor/Servo/Yaw (0x60-0x6B) |
| 0x80-0x8F | `HubFxError` | Audio/Storage/Config/File (0x80-0x8F) |
| 0xF0-0xFF | `SerialError` | System/transport (INTERNAL, TIMEOUT, CRC, FRAMING, ...) |

Each error namespace provides a `getMessage()` (C++) / `name()` (Python) function for human-readable names.

## Usage

### Server (Pico — using SfxServer)

```cpp
#include <serial/serial.h>
#include <server/sfx_server.h>

SfxServer server;
GunFxServer gunfxServer;

void setup() {
    server.begin("GunFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]()     { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });

    gunfxServer.begin(&Serial, server.deviceName());
    gunfxServer.onTriggerOn([](uint16_t rpm) -> uint8_t {
        startFiring(rpm);
        return SerialError::OK;
    });

    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        buf[0] = firingState;
        return 1;
    });

    server.addModuleHandler(&gunfxServer);  // Core auto-registered first
}

void loop() {
    server.loop();
    updateHardware();
    SFX_DELAY_MS(1);
}
```

### Client (HubFX)

```cpp
#include <serial/serial.h>

GunFxClient gunfx;

void setup() {
    UsbHost::instance().begin();
    gunfx.begin(0);
    gunfx.onReady([](const char* name) { /* Connected */ });
}

void loop() {
    UsbHost::instance().process();
    gunfx.process();

    CommandResult result = gunfx.triggerOn(600);
    if (!result.success) {
        // result.errorCode, result.message()
    }
}
```

## Build Configuration

### SCALEFX_SERVER Macro

Define `SCALEFX_SERVER` to exclude USB Host client code from server builds:

```ini
# platformio.ini (server controllers)
build_flags = -DSCALEFX_SERVER
```

This guards `client/bus.cpp` so it only compiles for the HubFX client.

## Buffer Sizes

| Constant | Value | Location |
|----------|-------|----------|
| `MAX_PAYLOAD_SIZE` | 512 (Pico) / 2048 (ESP32) | SfxWire (sfx_platform) |
| `HEADER_SIZE` | 4 | SfxWire |
| `MAX_PACKET_SIZE` | 517/2053 | SfxWire (4 + payload + 1) |
| `COBS_BUFFER_SIZE` | ~527/~2063 | SfxWire (derived from MAX_PACKET_SIZE) |
| `MAX_CHUNK_DATA` | 508 | StreamProtocol (512 - 4 byte chunk header) |
| `MAX_HANDLERS` | 8 | CommandRouter |
| `MAX_PENDING` | 64 | ResultQueue |
| `CommandResult::errorMessage` | 64 chars | core.h |
| `CoreBoardInfo::deviceName` | 32 chars | core.h |

## Cross-Platform Compatibility

### Platform-Specific Code

The library has **minimal platform-specific code** — only one `#ifdef` guard in the entire codebase:

| File | Guard | Purpose |
|------|-------|---------|
| `client/bus.cpp` | `#ifndef SCALEFX_SERVER` | Excludes client transport from server builds |

All other code compiles identically on all platforms.

### API Usage

| API | Where Used | Platform-Safe? |
|-----|-----------|----------------|
| `millis()` | `core.h` (CommandRouter), `bus_server.cpp`, `bus.cpp`, `result_queue.cpp` | Yes — safe on all platforms per Rule 16 |
| `SFX_DELAY_MS()` | `result_queue.cpp` (busy-wait) | Yes — platform abstraction |
| `SFX_LOG_*` | `bus.cpp` (DiagLog) | Yes — via sfx_platform |
| `Stream*` | All server classes | Yes — Arduino Stream API |

**No `delay()`, `Serial.print()`, `Wire`, `volatile`, or raw SDK calls found.**

### Wire Encoding Split

Raw wire encoding (CRC-8, COBS, endian helpers) lives in `SfxWire` namespace in `sfx_platform`, not in sfx_serial. This prevents circular dependencies: DiagLog (sfx_platform) needs wire encoding to format log packets, but sfx_serial depends on sfx_platform. The split allows both to use encoding without cycles.

`CoreProtocol` in `core/core.h` re-exports everything via `using SfxWire::*` declarations for backward compatibility.

### `std::atomic` Usage

Only in `ResultQueue`:
- `std::atomic<uint8_t> _waitingTag{0}` — relaxed ordering
- `std::atomic<bool> _waitResolved{false}` — release/acquire ordering

Same-core usage; atomic for correctness policy, not cross-core need.

## ESP32 Analysis — No Platform Optimizations Needed

The library is **purely platform-agnostic**. Every platform concern is delegated:

| Concern | Delegation |
|---------|-----------|
| Wire encoding (CRC, COBS, endian) | `SfxWire` in sfx_platform |
| Delays | `SFX_DELAY_MS()` in sfx_platform |
| Logging | `DiagLog` in sfx_platform |
| USB Host transport | `UsbHost` in sfx_usb |
| Serial I/O | Arduino `Stream*` (universal) |
| Timestamps | `millis()` (safe everywhere) |

**No ESP32-specific APIs, optimizations, or conditional compilation are required.**

## Dependencies

| Dependency | Reason |
|------------|--------|
| `sfx_platform` | `SfxWire` (CRC, COBS, endian), `DiagLog`, `sfx_platform.h` (SFX_DELAY_MS) |
| `Arduino` | `Stream*` (serial I/O), `millis()`, `<Arduino.h>` (types) |

**Runtime (client-only):** `sfx_usb` — `UsbHost::instance()` for USB CDC transport.

**No dependency on:** sfx_server, sfx_storage, sfx_audio, sfx_peripherals.
