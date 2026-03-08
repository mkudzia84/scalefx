# Serial Library

Binary COBS serial communication library for ScaleFX controllers.

## Overview

This library provides serial communication for ScaleFX controllers:

- **HubFX (Client)** — RP2040 with USB Host, controls multiple server devices
- **GunFX (Server)** — RP2040 Pico, muzzle flash and recoil control
- **LightFX (Server)** — RP2040 Pico, LED and servo control
- **GearControl (Server)** — RP2040 Pico, landing gear and servo control

All communication uses binary COBS-encoded packets with CRC-8 verification.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        serial.h (umbrella)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                   serial_core.h                           │   │
│  │  CoreProtocol (COBS, CRC, endian helpers)                 │   │
│  │  SerialError namespace (generic error codes)              │   │
│  │  CommandResult struct                                     │   │
│  │  ICommandHandler interface + CommandRouter                │   │
│  │  SFX_* handler macros                                     │   │
│  │  StatusDataCallback, CorePayload                          │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
│  ┌──────────────────────────────────┐                           │
│  │     serial_bus_server.h          │                           │
│  │  BusServer (base for servers)    │  ← Server side            │
│  │  CoreCommandServer (0xF0-0xFF)   │                           │
│  └──────────────────────────────────┘                           │
│                                                                  │
│  ┌──────────────────────────────────┐                           │
│  │     serial_stream.h              │                           │
│  │  StreamProtocol (0xA4-0xA6)      │  ← Chunked streaming      │
│  │  StreamWriter (uses BusServer)   │                           │
│  └──────────────────────────────────┘                           │
│                                                                  │
│  ┌──────────────────────────────────┐                           │
│  │     serial_diag_log.h            │                           │
│  │  DiagLog (ring buffer → COBS)    │  ← Diagnostic logging     │
│  │  LOG_MESSAGE (0xFD, universal)   │                           │
│  └──────────────────────────────────┘                           │
│                                                                  │
│  ┌──────────────────────────────────┐                           │
│  │     serial_bus_client.h          │                           │
│  │  BusClient (base for clients)    │  ← Client side            │
│  └──────────────────────────────────┘                           │
│                                                                  │
│  ┌──────────────────────────────────┐                           │
│  │     serial_bus.h                 │                           │
│  │  SerialBus (COBS over USB CDC)   │  ← Client transport       │
│  │  serial_usb_host.h (PIO-USB)     │                           │
│  └──────────────────────────────────┘                           │
│                                                                  │
│  ┌──────────────────────────────────┐                           │
│  │     serial_result_queue.h        │                           │
│  │  ResultQueue (tag correlation)   │  ← Client infrastructure  │
│  └──────────────────────────────────┘                           │
│                                                                  │
│  ┌───────────────────┐ ┌──────────────────┐ ┌──────────────────┐│
│  │ serial_gunfx.h    │ │ serial_lightfx.h │ │ serial_gear-     ││
│  │ GunFxServer       │ │ LightFxServer    │ │ control.h        ││
│  │ GunFxClient       │ │ LightFxClient    │ │ GearControlServer││
│  │ GunFxPacket       │ │ LightFxPacket    │ │ GearControlClient││
│  │ GunFxError        │ │ LightFxError     │ │ GearControlError ││
│  │ GunFxSpec         │ │ LightFxSpec      │ │ GearControlSpec  ││
│  └───────────────────┘ └──────────────────┘ └──────────────────┘│
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Class Hierarchy

### Server Side

```
ICommandHandler (interface in serial_core.h)
  │ tryProcess(type, payload, len) → CommandHandleResult
  │ handlerName() → const char*
  │
  └── BusServer (serial_bus_server.h — ACK/NACK helpers, range routing)
        │ begin(Stream*) / end()
        │ sendAck() / sendNack() / sendError() / sendRawPacket()
        │
        ├── CoreCommandServer (0xF0-0xFF)
        │     INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE, STATUS_REQ, I2C_SCAN
        │     onStatusData(callback) — module status append
        │
        ├── GunFxServer (0x01-0x2F)
        ├── LightFxServer (0x40-0x5F)
        └── GearControlServer (0x60-0x7F)

StreamWriter (serial_stream.h — uses BusServer for packet output)
  │ begin(totalBytes) / write(data, len) / printf(fmt, ...) / end()
  │ Uses StreamProtocol::STREAM_BEGIN/DATA/END (0xA4-0xA6)
  └── Any BusServer subclass can instantiate StreamWriter for large responses
```

### Client Side

```
SerialBus (serial_bus.h — COBS framing over USB CDC)
  │ begin(UsbHost*, port) / process()
  │ sendPacket(type, payload, len, tag)
  │
  └── BusClient (serial_bus_client.h — INIT handshake, ACK/NACK, tag queue)
        │ sendCommand(type, payload, len) → CommandResult
        │ sendInit() / onReady() / onError()
        │ resultQueue() → ResultQueue&
        │
        ├── GunFxClient
        ├── LightFxClient
        └── GearControlClient
```

## Components

| Header | Description | Used By |
|--------|-------------|---------|
| `serial.h` | Umbrella header — includes everything | All |
| `serial_core.h` | Protocol, errors, ICommandHandler, CommandRouter, SFX macros | All |
| `serial_core.cpp` | CoreProtocol implementations, CorePayload encode/decode | All |
| `serial_bus_server.h` | BusServer base + CoreCommandServer | Server only |
| `serial_bus_server.cpp` | BusServer + CoreCommandServer implementations | Server only |
| `serial_bus_client.h` | BusClient base class (extends SerialBus) | Client only |
| `serial_bus_client.cpp` | BusClient implementation | Client only |
| `serial_bus.h` | SerialBus (USB Host COBS transport) | Client only |
| `serial_bus.cpp` | SerialBus implementation | Client only |
| `serial_usb_host.h` | UsbHost (PIO-USB manager) | Client only |
| `serial_result_queue.h` | ResultQueue (tag-correlated request/response) | Client only |
| `serial_result_queue.cpp` | ResultQueue implementation | Client only |
| `serial_stream.h` | StreamProtocol constants + StreamWriter (chunked streaming) | Server only |
| `serial_stream.cpp` | StreamWriter + CRC-16/CCITT implementation | Server only |
| `serial_diag_log.h` | DiagLog — diagnostic log output over serial protocol | All |
| `serial_diag_log.cpp` | DiagLog implementation (ring buffer, COBS flush, ingest) | All |
| `serial_gunfx.h/.cpp` | GunFxServer + GunFxClient + GunFxPacket + GunFxError + GunFxSpec | GunFX |
| `serial_lightfx.h/.cpp` | LightFxServer + LightFxClient + LightFxPacket + LightFxError | LightFX |
| `serial_gearcontrol.h/.cpp` | GearControlServer + GearControlClient + GearControlPacket + GearControlError | GearControl |

## Protocol

Binary COBS-encoded packets with CRC-8:

```
[type:u8][tag:u8][len:u16LE][payload:0-512 bytes][crc8:u8]
```

- **type** — Packet type identifier
- **tag** — Correlation tag (1-255 for request/response, 0 for async)
- **len** — Payload length, u16 little-endian (0-512)
- **payload** — Command-specific data
- **crc8** — CRC-8 (polynomial 0x07) over type+tag+len+payload

Packet Type Ranges:
- `0x01-0x2F` — GunFX commands (trigger, servo, smoke)
- `0x40-0x5F` — LightFX commands (LED, servo, power, landing lights)
- `0x60-0x7F` — GearControl commands (gear, servo, yaw, calibration)
- `0x80-0xA3` — HubFX commands (slaves, audio, engine, config, SD, files)
- `0xA4-0xA6` — Streaming protocol (STREAM_BEGIN/DATA/END) — see `serial_stream.h`
- `0xF0-0xFF` — Core system commands (INIT, ACK, NACK, STATUS, etc.)

See [PROTOCOL.md](PROTOCOL.md) for detailed protocol documentation.

## Usage

### Server (Pico — using PicoServer)

All Pico server controllers use `PicoServer` to handle common boilerplate:

```cpp
#include <serial.h>
#include "pico_server.h"

PicoServer server;
GunFxServer gunfxServer;

void setup() {
    server.begin("GunFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]()     { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });

    gunfxServer.begin(&Serial);
    gunfxServer.onTriggerOn([](uint16_t rpm) -> uint8_t {
        startFiring(rpm);
        return SerialError::OK;
    });
    gunfxServer.onServoSet([](uint8_t id, uint16_t pulse_us) -> uint8_t {
        return setServoPosition(id, pulse_us);
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
    delay(1);
}
```

### Client (HubFX)

```cpp
#include <serial.h>

UsbHost usbHost;
GunFxClient gunfx;

void setup() {
    usbHost.begin();

    gunfx.begin(&usbHost, 0);
    gunfx.onReady([](const char* name) {
        Serial.printf("Connected: %s\n", name);
    });
    gunfx.onError([](uint8_t code, const char* msg) {
        Serial.printf("Error %02X: %s\n", code, msg);
    });
}

void loop() {
    usbHost.process();
    gunfx.process();

    // Send commands
    gunfx.triggerOn(600);
    gunfx.setServoPosition(1, 1500);
    gunfx.setSmokeHeater(true);
}
```

## Build Configuration

### SCALEFX_SERVER Macro

Define `SCALEFX_SERVER` to exclude USB Host code from server builds:

```ini
# platformio.ini
[env:pico]
build_flags = -DSCALEFX_SERVER
```

This guards `serial_bus.h/.cpp` and USB Host includes so they are only compiled for the HubFX client.

## Error Handling

Error codes are organized by namespace:

- **`SerialError`** (serial_core.h) — Generic errors (0x00-0x1F) and system errors (0xF0-0xFF)
- **`GunFxError`** (serial_gunfx.h) — GunFX-specific errors (0x20-0x4F)
- **`LightFxError`** (serial_lightfx.h) — LightFX-specific errors (0x50-0x5F)
- **`GearControlError`** (serial_gearcontrol.h) — GearControl-specific errors (0x60-0x6F)

Server callbacks return error codes:

```cpp
gunfxServer.onServoSet([](uint8_t id, uint16_t pulse_us) -> uint8_t {
    if (id < 1 || id > 3) return GunFxError::SERVO_INVALID_ID;
    if (pulse_us < 500 || pulse_us > 2500) return GunFxError::SERVO_PULSE_RANGE;
    setServo(id, pulse_us);
    return SerialError::OK;
});
```

Client commands return `CommandResult`:

```cpp
CommandResult result = gunfx.triggerOn(600);
if (!result.success) {
    Serial.printf("Error %02X: %s\n", result.errorCode, result.message);
}
```
