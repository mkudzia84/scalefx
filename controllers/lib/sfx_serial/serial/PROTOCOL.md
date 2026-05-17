# ScaleFX Serial Protocol

## Overview

Serial communication protocol for ScaleFX controllers (HubFX, GunFX).

**Version:** 0.5.0  
**Binary Framing:** COBS encoding with 0x00 delimiter  
**CRC:** CRC-8 polynomial 0x07 over type+tag+len(2)+payload  

## Protocol

All communication uses **binary COBS-encoded packets**, including INIT and INIT_READY.

### Connection Flow

```
Master                              Slave
  |                                   |
  |  INIT (0xF0, binary COBS)         |
  |---------------------------------->|
  |                                   |  (resets state)
  |  INIT_READY (0xF3, binary COBS)   |
  |<----------------------------------|
  |                                   |
  |  [Binary COBS packets]            |
  |<=================================>|
```

### Reconnection

If the master sends a new INIT command (e.g., after disconnect/reconnect), the slave:
1. Resets connection state
2. Performs safe shutdown
3. Responds with INIT_READY containing device info

This allows seamless reconnection without requiring slave reboot.

## Packet Formats

### Binary Protocol

Before COBS encoding:
```
[type:u8][tag:u8][len:u16LE][payload:0-512 bytes][crc:u8]
```

Header is 4 bytes (type + tag + len_lo + len_hi). MAX_PAYLOAD_SIZE is platform-specific (512 on Pico, 2048 on ESP32).

### Text Protocol

Human-readable line-based format:
```
COMMAND_NAME key=value key2=value2\n
```

## Universal Packet Types (0xEF-0xFF)

System-level commands common to all ScaleFX devices:

| Type | Value | Text Command | Direction | Description |
|------|-------|--------------|-----------|-------------|
| `CorePacket::INIT` | 0xF0 | `INIT` | Master→Slave | Initialize slave device |
| `CorePacket::SHUTDOWN` | 0xF1 | `SHUTDOWN` | Master→Slave | Shutdown slave device |
| `CorePacket::KEEPALIVE` | 0xF2 | `KEEPALIVE` | Master→Slave | Connection keepalive |
| `CorePacket::INIT_READY` | 0xF3 | `INIT_READY` | Slave→Master | Slave ready response |
| `CorePacket::STATUS` | 0xF4 | `STATUS` | Slave→Master | Status telemetry |
| `CorePacket::ERROR` | 0xF5 | `ERROR` | Slave→Master | Error notification |
| `CorePacket::ACK` | 0xF6 | `ACK` | Slave→Master | Acknowledgment |
| `CorePacket::NACK` | 0xF7 | `NACK` | Slave→Master | Negative acknowledgment |
| `CorePacket::REBOOT` | 0xF8 | `REBOOT` | Master→Slave | Reboot slave device |
| `CorePacket::BOOTSEL` | 0xF9 | `BOOTSEL` | Master→Slave | Enter BOOTSEL mode |
| `CorePacket::STATUS_REQ` | 0xFA | — | Master→Slave | Request status |
| `CorePacket::I2C_SCAN` | 0xFB | — | Master→Slave | Trigger I2C bus scan |
| `CorePacket::I2C_SCAN_RESULT` | 0xFC | — | Slave→Master | I2C scan results |
| `CorePacket::LOG_MESSAGE` | 0xFD | — | Slave→Master | Diagnostic log message |
| `CorePacket::IDENTIFY` | 0xFE | `IDENTIFY` | Master→Slave | Query board info (no state change) |
| `CorePacket::STATUS_UPDATE` | 0xEF | — | Slave→Master | Async status telemetry (verbose mode) |

### INIT Command

**Binary packet** (type 0xF0), optional payload `[mode:u8][flags:u8]`.

**Payload (optional, backward-compatible):**
```
[mode:u8][flags:u8]
```

| Field | Type | Values | Default |
|-------|------|--------|---------|
| mode | u8 | 0x00=SLAVE, 0x01=DIRECT | 0x00 (SLAVE) |
| flags | u8 | Bit 0: VERBOSE (enable STATUS_UPDATE) | 0x00 (NONE) |

- **SLAVE mode (0x00):** Keep-alive required (15s timeout). Used when HubFX controls the board.
- **DIRECT mode (0x01):** No keep-alive timeout. Used for standalone CLI/GUI configuration.
- **Backward compatible:** If payload is empty (len=0), defaults to SLAVE mode with no flags.

**Behavior:**
1. Slave resets any existing connection state
2. Slave performs safe shutdown of all active operations
3. Slave responds with INIT_READY containing device info

### IDENTIFY Command

**Binary packet** (type 0xFE), no payload.

**Behavior:**
1. Slave responds with IDENTIFY (0xFE) containing device info
2. No state changes — does NOT trigger init callbacks, connection state, or watchdog
3. Same payload format as INIT_READY

Use IDENTIFY to discover the board type before deciding whether to send INIT.
HubFX auto-initializes on boot so only needs IDENTIFY; slave controllers need
INIT after IDENTIFY to start up.

### INIT_READY Response

**Binary packet** (type 0xF3) with length-prefixed strings and u32LE fields.

**Payload format:**
```
[nameLen:u8][name:N bytes]
[verLen:u8][version:N bytes]
[platLen:u8][platform:N bytes]
[cpuMHz:u32LE]
[freeRam:u32LE]
[buildNum:u32LE]
[capabilities:u32LE]   ← Rule 11 append-only field; 0 on legacy firmware
```

**Fields:**
- `name` - Device name with unique ID (e.g., "GunFX-A4B2")
- `version` - Firmware version without "v" prefix (e.g., "0.2.0")
- `platform` - Hardware platform (e.g., "RP2040")
- `cpuMHz` - CPU frequency in MHz (u32LE)
- `freeRam` - Free RAM in bytes at boot (u32LE)
- `buildNum` - Build number, incremented with each build (u32LE)
- `capabilities` - Bitmask of optional interfaces the firmware exposes
  (u32LE, append-only field — absent on firmware that pre-dates it →
  decoded as `0`, which clients should treat as "unknown, fall back
  to probing").

**Capability bits (`CoreCapability`, mirrored in [app/go/protocol/core/core.go](../../../../app/go/protocol/core/core.go) as `core.Cap*`):**

| Bit  | Constant      | Meaning                                              |
|------|---------------|------------------------------------------------------|
| 0    | `FLASH`       | LittleFS flash storage commands available            |
| 1    | `SD`          | SD card storage commands available (slot present)    |
| 2    | `AUDIO`       | AudioMixer + audio playback commands available       |
| 3    | `USB_HOST`    | USB host stack + device enumeration available        |
| 4    | `ENGINE`      | Sound engine commands available                      |
| 5    | `CONFIG`      | YAML config store commands available                 |
| 6    | `SLAVE_BUS`   | Master can enumerate / route to slaves               |

`CAP_SD` advertises slot presence, **not** that a card is currently
mounted — clients still need `SD_STATUS_REQ` to learn whether a card is
inserted and its remaining capacity. `CAP_FLASH` is set after
`FlashModule::begin()` succeeds (storage actually mounted).

**Example** (38 bytes for LightFX-521C v0.2.0):
```
0C "LightFX-521C" 05 "0.2.0" 06 "RP2040" 85000000 50C00300 02000000
│                  │          │           │        │        └─ build=2
│                  │          │           │        └─ freeRam=245840
│                  │          │           └─ cpuMHz=133
│                  │          └─ platform (6 chars)
│                  └─ version (5 chars)
└─ name (12 chars)
```

**Note:** Version should NOT include "v" prefix (use "0.2.0" not "v0.2.0").

### STATUS_UPDATE

Async telemetry packet emitted by any controller when VERBOSE flag is set during INIT.
Uses `TAG_ASYNC` (0x00) — unsolicited, no request to correlate.

**Type:** 0xEF  
**Direction:** Slave → Master  
**Tag:** Always `TAG_ASYNC` (0x00)

**Payload:**
```
[source:u8][updateType:u8][data:variable]
```

| Field | Type | Description |
|-------|------|-------------|
| source | u8 | Module source identifier (matches packet range base) |
| updateType | u8 | Type of status update |
| data | variable | Type-specific payload |

**Source identifiers:**

| Source | Value | Module |
|--------|-------|--------|
| GUNFX | 0x01 | GunFX |
| LIGHTFX | 0x40 | LightFX |
| GEARCONTROL | 0x60 | GearControl |
| HUBFX | 0x80 | HubFX |
| CORE | 0xF0 | Core system |

**Update types:**

| Type | Value | Data Format | Description |
|------|-------|-------------|-------------|
| SERVO_POSITION | 0x01 | `[id:u8][pos_us:u16LE]...` | Servo position(s) |
| VOLTAGE | 0x02 | `[ch:u8][mV:u16LE]...` | Voltage reading(s) |
| CURRENT | 0x03 | `[ch:u8][mA:u16LE]...` | Current reading(s) |
| TEMPERATURE | 0x04 | `[sensor:u8][tenths_C:u16LE]...` | Temperature reading(s) |

Only emitted when `InitFlags::VERBOSE` (bit 0) is set in the INIT payload.
Controllers call `sendStatusUpdate(source, updateType, data, dataLen)` which
checks the verbose flag before sending.

### SHUTDOWN Command

Requests graceful shutdown of the slave device. The slave performs a safe shutdown
(stops firing, disables heater/fan, resets servos to neutral) but remains running.

**Binary Payload:** None  
**Response:** ACK

**Safe Shutdown Actions (GunFX):**
- Stops firing (rate = 0)
- Disables smoke heater
- Disables smoke fan
- Turns off nozzle flash LED
- Resets all servos to neutral position (1500µs)

### KEEPALIVE Command

Connection heartbeat to maintain connection state.

**Binary Payload:** None  
**Response:** ACK

### REBOOT Command

Triggers software reset via `rp2040.reboot()`. Performs safe shutdown before rebooting.

**Binary Payload:** None  
**Response:** None (fire-and-forget, device reboots)

### BOOTSEL Command

Triggers entry to BOOTSEL mode via `rp2040.rebootToBootloader()`. Performs safe shutdown before entering bootloader.

**Binary Payload:** None  
**Response (Pico):** None (fire-and-forget, device enters BOOTSEL mode, RPI-RP2 drive appears)  
**Response (ESP32):** NACK `NOT_SUPPORTED` (0x06) — ESP32 has no UF2 bootloader, use esptool instead

### LOG_MESSAGE

Async diagnostic log message emitted by any board via `DiagLog`. Universal across all
controllers (GunFX, LightFX, GearControl, HubFX). HubFX relays slave log messages into
its own buffer before flushing to the PC client.

**Binary Payload:**
```
[level:u8][millis:u32LE][message:str]
```

| Field | Type | Description |
|-------|------|-------------|
| level | u8 | 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR |
| millis | u32LE | Board uptime in milliseconds |
| message | str | UTF-8 log message (no null terminator) |

**Tag:** Always `TAG_ASYNC` (0x00) — unsolicited, no request to correlate.

## GunFX-Specific Packet Types (0x01-0x2F)

Commands specific to GunFX controllers:

| Type | Value | Text Command | Direction | Description |
|------|-------|--------------|-----------|-------------|
| `GUNFX_PKT_TRIGGER_ON` | 0x01 | `TRIGGER_ON` | M→S | Start firing |
| `GUNFX_PKT_TRIGGER_OFF` | 0x02 | `TRIGGER_OFF` | M→S | Stop firing |
| `GUNFX_PKT_SRV_SET` | 0x10 | `SERVO_SET` | M→S | Set servo position |
| `GUNFX_PKT_SRV_SETTINGS` | 0x11 | `SERVO_CONFIG` | M→S | Configure servo profile |
| `GUNFX_PKT_SRV_RECOIL_JERK` | 0x12 | `SERVO_RECOIL_JERK` | M→S | Configure recoil jerk |
| `GUNFX_PKT_SMOKE_HEAT` | 0x20 | `SMOKE_HEAT` | M→S | Control smoke heater |

## Streaming Protocol (0xA4-0xA6)

Reusable chunked data streaming for responses exceeding `MAX_PAYLOAD_SIZE` (platform-specific).
Defined in `core/stream.h` / `StreamProtocol` namespace. Any `BusServer` subclass can
use `StreamWriter` to stream data to the client using these packet types.

**C++:** `StreamProtocol::STREAM_BEGIN/DATA/END` in `core/stream.h`
**Python:** `StreamPacket.STREAM_BEGIN/DATA/END` in `packets.py`

### Packet Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `STREAM_BEGIN` | 0xA4 | Server→Client | Announce start of stream |
| `STREAM_DATA`  | 0xA5 | Server→Client | Data chunk with per-chunk CRC-16 |
| `STREAM_END`   | 0xA6 | Server→Client | End of stream with verification |

### Wire Format

**STREAM_BEGIN (0xA4):**
```
[totalBytes:u32LE]
```
- `totalBytes` — Expected total data size in bytes (0 = unknown size)

**STREAM_DATA (0xA5):**
```
[seqNum:u16LE][crc16:u16LE][data:0-508 bytes]
```
- `seqNum` — Segment sequence number (0-based, incrementing)
- `crc16` — CRC-16/CCITT (poly 0x1021, init 0xFFFF) over `data` portion only
- `data` — Chunk payload, max `MAX_PAYLOAD_SIZE - 4` bytes (508 on Pico, 2044 on ESP32)

**STREAM_END (0xA6):**
```
[totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]
```
- `totalSegs` — Total number of DATA segments sent
- `totalBytes` — Actual total data bytes sent (sum of all chunk data)
- `crc16All` — CRC-16/CCITT over ALL data bytes across all segments (running CRC)

### CRC-16/CCITT

- **Polynomial:** 0x1021 (CCITT standard)
- **Initial value:** 0xFFFF
- **Scope:** Per-chunk CRC in `STREAM_DATA`, full-stream CRC in `STREAM_END`
- **Implementation:** `StreamProtocol::crc16()` (C++), `crc16_ccitt()` (Python)

### Flow Example

```
Server                                Client
  |                                     |
  |  STREAM_BEGIN [totalBytes=1200]     |
  |------------------------------------>|
  |                                     |
  |  STREAM_DATA [seq=0][crc][508 B]    |
  |------------------------------------>|
  |                                     |
  |  STREAM_DATA [seq=1][crc][508 B]    |
  |------------------------------------>|
  |                                     |
  |  STREAM_DATA [seq=2][crc][184 B]    |
  |------------------------------------>|
  |                                     |
  |  STREAM_END [segs=3][1200][crc_all] |
  |------------------------------------>|
  |                                     |
  Client verifies totalBytes and crc_all
```

All packets in a stream share the same **tag** for correlation. The client
matches STREAM_BEGIN/DATA/END by tag to associate them with the original request.

### StreamWriter Usage (C++)

```cpp
#include <server/stream.h>

// In a policy handler — uses default STREAM_BEGIN/DATA/END types:
StreamWriter stream(*_ctx, currentTag());
stream.begin(totalSize);                // Send STREAM_BEGIN (0 = unknown)
stream.write(data, len);               // Auto-chunks at 508 bytes
stream.printf("%-9lu  %s\n", sz, name); // Printf-style (256 char limit)
stream.end();                           // Flush + send STREAM_END
```

### Stream Receiving (Python)

```python
from tests.framework import StreamPacket
from tests.framework.protocol import read_u16_le, read_u32_le, crc16_ccitt

data = bytearray()
while True:
    pkt = wait_for_tag(tag)
    if pkt.packet_type == StreamPacket.STREAM_BEGIN:
        total = read_u32_le(pkt.payload, 0)
    elif pkt.packet_type == StreamPacket.STREAM_DATA:
        seq = read_u16_le(pkt.payload, 0)
        crc = read_u16_le(pkt.payload, 2)
        chunk = pkt.payload[4:]
        assert crc16_ccitt(chunk) == crc  # Per-chunk verify
        data.extend(chunk)
    elif pkt.packet_type == StreamPacket.STREAM_END:
        total_segs  = read_u16_le(pkt.payload, 0)
        total_bytes = read_u32_le(pkt.payload, 2)
        crc_all     = read_u16_le(pkt.payload, 6)
        assert crc16_ccitt(data) == crc_all  # Full-stream verify
        break
```

## HubFX Packet Types (0x80-0xA3)

Commands specific to HubFX controllers. Defined in `hubfx/hubfx.h` (not auto-included by `serial.h`).

### Audio Control (0x84-0x8B)

| Type | Value | Payload | Response |
|------|-------|---------|----------|
| `AUDIO_PLAY` | 0x84 | `[ch:u8][vol:u8][outputChannels:u8][loopMode:u8][loopCount:u16LE][pathLen:u8][path:str]` | ACK/NACK |
| `AUDIO_STOP` | 0x85 | `[ch:u8]` (0xFF=all) | ACK/NACK |
| `AUDIO_VOLUME` | 0x86 | `[ch:u8][vol:u8]` (ch 0xFF=master) | ACK/NACK |
| `AUDIO_FADE` | 0x87 | `[ch:u8]` | ACK/NACK |
| `AUDIO_QUEUE` | 0x88 | `[ch:u8][vol:u8][loopCount:u16LE][behavior:u8][pathLen:u8][path:str]` | ACK/NACK |
| `AUDIO_QUEUE_CLEAR` | 0x89 | `[ch:u8]` (0xFF=all) | ACK/NACK |
| `AUDIO_STATUS_REQ` | 0x8A | (none) | AUDIO_STATUS_RESP |
| `AUDIO_STATUS_RESP` | 0x8B | v3 format, see below | — |

**AUDIO_STATUS_RESP v3 format:**
```
[initialized:u8][codecName:16 bytes][sampleRate:u32LE][bitDepth:u8][numChannels:u8]
[masterVol:u8][ringSize:u32LE][ringAvail:u32LE][underruns:u32LE]
[consumeLoops:u32LE][consumeFrames:u32LE]
Per channel × 8:
  [playing:u8][volume:u8][looping:u8][remaining_ms:u32LE][outputChannels:u8]
  [loopCount:u16LE][totalSamples:u32LE]
  [wavSampleRate:u32LE][wavChannels:u8][wavBitsPerSample:u8]
  [filenameLen:u8][filename:str]
```

### Config Management (0x90-0x92, 0xAC)

| Type | Value | Payload | Response |
|------|-------|---------|----------|
| `CONFIG_RELOAD` | 0x90 | `[]` or `[pathLen:u8][path:str]` | ACK/NACK |
| `CONFIG_STATUS` | 0x91 | (none) | CONFIG_STATUS_RESP |
| `CONFIG_STATUS_RESP` | 0x92 | `[loaded:u8][size:u16LE][validOk:u8]` | — |
| `CONFIG_SAVE` | 0xAC | `[]` or `[pathLen:u8][path:str]` | ACK/NACK |

### SD Card Management (0x93-0x95)

| Type | Value | Payload | Response |
|------|-------|---------|----------|
| `SD_INIT` | 0x93 | `[speed_mhz:u8]` | ACK/NACK |
| `SD_STATUS_REQ` | 0x94 | (none) | SD_STATUS_RESP |
| `SD_STATUS_RESP` | 0x95 | `[initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE][freeSpace_MB:u32LE][fatType:u8]` | — |

### Flash Management (0x99)

| Type | Value | Payload | Response |
|------|-------|---------|----------|
| `FLASH_STATUS_REQ` | 0x99 | (none) | FLASH_STATUS_RESP (same type) |

**FLASH_STATUS_RESP (0x99) payload:**
```
[initialized:u8][totalBytes:u32LE][usedBytes:u32LE][freeBytes:u32LE]
```

### File Transfer Protocol

HubFX provides two file transfer modes for SD card and flash access, both built
on top of the streaming protocol infrastructure.

**Storage target selection:** File commands accept an optional `[target:u8]` at the
end of the payload. If omitted, defaults to SD card (backward-compatible).
Values: `0` = SD card, `1` = Flash (onboard LittleFS).

#### Download (Server→Client)

The server pushes file data using `StreamWriter`. The client reassembles from
`STREAM_BEGIN/DATA/END` packets. Fire-and-forget — no per-chunk ACK.

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `FILE_LIST` | 0x9A | `[pathLen:u8][path:str][target:u8?]` | Streamed: POSIX-like text listing |
| `FILE_DOWNLOAD` | 0x9F | `[pathLen:u8][path:str][target:u8?]` | Streamed: raw file bytes |

**FILE_LIST output format** (one line per entry):
```
d          -  sounds/
-       1234  config.yaml
```
Format: `[type:1]  [size:9]  [name]` — `d` for directory, `-` for file.

#### Upload (Client→Server)

The client drives the transfer by sending chunks with individual ACK/NACK
responses, enabling per-chunk retry on CRC failure.

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `FILE_UPLOAD_BEGIN` | 0xA0 | `[size:u32LE][pathLen:u8][path:str][target:u8?]` | ACK |
| `FILE_UPLOAD_DATA` | 0xA1 | `[seqNum:u16LE][crc16:u16LE][data:N]` | ACK / NACK(CRC_ERROR) |
| `FILE_UPLOAD_END` | 0xA2 | (none) | ACK / NACK |
| `FILE_UPLOAD_CANCEL` | 0xA3 | (none) | ACK |

**Upload flow:**
```
Client                                Server
  |                                     |
  |  FILE_UPLOAD_BEGIN [1200][path]      |
  |------------------------------------>|  Opens file for writing
  |  ACK                                |
  |<------------------------------------|
  |                                     |
  |  FILE_UPLOAD_DATA [seq=0][crc][508B]|
  |------------------------------------>|  Verify CRC, write chunk
  |  ACK                                |
  |<------------------------------------|
  |                                     |
  |  FILE_UPLOAD_DATA [seq=1][crc][508B]|
  |------------------------------------>|  CRC mismatch!
  |  NACK(CRC_ERROR)                    |
  |<------------------------------------|
  |                                     |
  |  FILE_UPLOAD_DATA [seq=1][crc][508B]|  (retry same seq)
  |------------------------------------>|  CRC OK, write chunk
  |  ACK                                |
  |<------------------------------------|
  |                                     |
  |  FILE_UPLOAD_END                    |
  |------------------------------------>|  Verify total size, close file
  |  ACK                                |
  |<------------------------------------|
```

**Upload chunk format** — reuses `StreamProtocol::CHUNK_HEADER_SIZE` (4 bytes):
```
[seqNum:u16LE][crc16:u16LE][data:0-508 bytes]
```

**Upload error conditions:**
- `UPLOAD_IN_PROGRESS` (0x8E) — Another upload is already active
- `NO_UPLOAD_ACTIVE` (0x8F) — No upload to send data to or end
- `CRC_ERROR` (0xF4) — Chunk CRC mismatch, client should retry
- `FILE_IO_ERROR` (0x8C) — SD card or flash write error, upload aborted

### Download vs Upload Design

| Aspect | Download (STREAM_*) | Upload (FILE_UPLOAD_*) |
|--------|---------------------|----------------------|
| **Direction** | Server → Client | Client → Server |
| **Packet types** | `StreamProtocol` (0xA4-0xA6) | `HubFxPacket` (0xA0-0xA3) |
| **Owner** | Reusable library (`core/stream.h`) | Module-specific (`hubfx/hubfx.h`) |
| **Flow control** | Fire-and-forget stream | Per-chunk ACK/NACK |
| **CRC verification** | Per-chunk + full-stream (passive) | Per-chunk (active, retry on fail) |
| **Shared infrastructure** | `StreamWriter`, `StreamProtocol` | `StreamProtocol::CHUNK_HEADER_SIZE`, `crc16()` |

#### Other File Operations

| Command | Value | Payload | Response |
|---------|-------|---------|----------|
| `FILE_DELETE` | 0x9B | `[pathLen:u8][path:str][target:u8?][flags:u8?]` — flags bit 0 = RECURSIVE (delete non-empty dirs). Legacy default (flags byte absent) = recursive for back-compat. | ACK / NACK |
| `FILE_MKDIR` | 0x9C | `[pathLen:u8][path:str][target:u8?][flags:u8?]` — flags bit 0 = PARENTS (mkdir `-p`: creates missing ancestors, idempotent when target exists) | ACK / NACK |
| `FILE_INFO` | 0x9D | `[pathLen:u8][path:str][target:u8?]` | FILE_INFO_RESP |
| `FILE_INFO_RESP` | 0x9E | `[exists:u8][isDir:u8][size:u32LE]` | — |

### TRIGGER_ON

**Binary Payload:** `[rpm:u16]`  
**Text Format:** `TRIGGER_ON rpm=600`

### TRIGGER_OFF

**Binary Payload:** `[fanDelayMs:u16]`  
**Text Format:** `TRIGGER_OFF fanDelayMs=3000`

### SERVO_SET

**Binary Payload:** `[servoId:u8][pulseUs:u16]`  
**Text Format:** `SERVO_SET id=1 pulseUs=1500`

### SERVO_CONFIG

**Binary Payload:** `[servoId:u8][minUs:u16][maxUs:u16][maxSpeedUsPerSec:u16][maxAccelUsPerSec2:u16][maxDecelUsPerSec2:u16]`  
**Text Format:** `SERVO_CONFIG id=1 minUs=1000 maxUs=2000 maxSpeedUsPerSec=500 maxAccelUsPerSec2=1000 maxDecelUsPerSec2=1000`

### SERVO_RECOIL_JERK

**Binary Payload:** `[servoId:u8][jerkUs:u16][varianceUs:u16]`  
**Text Format:** `SERVO_RECOIL_JERK id=1 jerkUs=50 varianceUs=10`

### SMOKE_HEAT

**Binary Payload:** `[on:u8]` (0=off, 1=on)  
**Text Format:** `SMOKE_HEAT on=1`

### STATUS

**Core Header (22 bytes, always present):**
```
[counter:u32LE][uptime:u32LE][freeRam:u32LE][lastActivity_ms:u32LE][keepaliveCount:u32LE]
[boardState:u8][initFlags:u8]
```

Board states: IDLE(0x00), STANDALONE(0x01), SLAVE(0x02), DIRECT(0x03)

**GunFX Module Data (20 bytes, appended to core header):**
```
[flags:u8][fanSpeed:u8][fanOffMs:u16][servo0:u16][servo1:u16][servo2:u16]
[rpm:u16][shots:u32][heaterMs:u32]
```

Flags:
- Bit 0: firing
- Bit 1: flashActive
- Bit 2: flashFading
- Bit 3: heaterOn
- Bit 4: fanOn
- Bit 5: fanSpindown

**LightFX Module Data (22 bytes, appended to core header):**
```
[ledBrightness:u8×8][ledSeqFlags:u8]
[servo0:u16][servo1:u16][servo2:u16]
[voltage:u16(mV)][current:i16(mA)][power:u16(mW)][powerAvail:u8]
```

- `ledSeqFlags`: Bit N = channel N+1 sequence playing
- `powerAvail`: 1 if INA226 detected, 0 otherwise

**NoOp:** Core header only (22 bytes, no module data).

### ERROR

**Binary Payload:** `[errorCode:u8][message:string]`  
**Text Format:** `ERROR code=1 msg=Servo timeout`

## Firmware Upload Workflow

### Manual BOOTSEL (Traditional)
1. Hold BOOTSEL button on Pico
2. Press RESET button
3. RPI-RP2 drive appears
4. Copy `firmware.uf2` to drive
5. Device auto-reboots with new firmware

### Serial-Triggered BOOTSEL
1. Send INIT (binary) and receive INIT_READY
2. Send BOOTSEL packet (0xF9, binary COBS, no payload)
3. Slave enters BOOTSEL mode automatically
4. RPI-RP2 drive appears
5. Copy `firmware.uf2` to drive
6. Device auto-reboots with new firmware

> **Note:** Use `python scripts/build_and_flash.py <controller>` for automated build+flash.

## Implementation Classes

| Class | Header | Description |
|-------|--------|-------------|
| `CoreProtocol` | `core/core.h` | COBS encoding, CRC-8, packet building/parsing |
| `SerialError` | `core/core.h` | Generic error code constants |
| `ICommandHandler` | `core/core.h` | Handler interface (`tryProcess()`) |
| `CommandRouter` | `core/core.h` | Routes packets to registered handlers |
| `BusServer` | `core/bus_server.h` | Base class for server command handlers |
| `CoreCommandServer` | `core/bus_server.h` | Server-side system command handler (INIT, STATUS, REBOOT, etc.) |
| `BusClient` | `client/bus_client.h` | Base class for client controllers |
| `ResultQueue` | `client/result_queue.h` | Tag-correlated command/response matching |
| `StreamWriter` | `core/stream.h` | Chunked data streaming with CRC-16 integrity |
| `StreamProtocol` | `core/stream.h` | Stream constants (0xA4-0xA6), CRC-16/CCITT |
| `GunFxServer` | `gunfx/gunfx.h` | GunFX command handler (server, extends BusServer) |
| `GunFxClient` | `gunfx/gunfx.h` | GunFX command sender (client, extends BusClient) |
| `LightFxServer` | `lightfx/lightfx.h` | LightFX command handler (server, extends BusServer) |
| `LightFxClient` | `lightfx/lightfx.h` | LightFX command sender (client, extends BusClient) |
| `GearControlServer` | `gearcontrol/gearcontrol.h` | GearControl command handler (server, extends BusServer) |
| `GearControlClient` | `gearcontrol/gearcontrol.h` | GearControl command sender (client, extends BusClient) |
| `HubFxAudioServer` | `hubfx/hubfx.h` | Audio mixer command handler (server, extends BusServer) |
| `HubFxAudioClient` | `hubfx/hubfx.h` | Audio command sender (client, extends BusClient) |
| `HubFxStorageServer` | `hubfx/hubfx.h` | SD/flash/config/file command handler (server, extends BusServer) |
| `HubFxStorageClient` | `hubfx/hubfx.h` | Storage command sender (client, extends BusClient) |
| `SerialBus` | `client/bus.h` | Client-side serial bus (USB host) |
| `UsbHost` | `client/usb_host.h` | PIO-USB host for HubFX |

### Server-Side Pattern

```cpp
CommandRouter commandRouter;
CoreCommandServer coreServer;
GunFxServer gunfxServer;

void setup() {
    Serial.begin(1000000);
    
    // Configure core handler
    coreServer.begin(&Serial);
    coreServer.setBoardInfo("GunFX-A4B2", FIRMWARE_VERSION, "RP2040",
                            rp2040.f_cpu() / 1000000, rp2040.getFreeHeap(),
                            BUILD_NUMBER);
    coreServer.onInit(performSafeInit);
    coreServer.onShutdown(performSafeShutdown);
    coreServer.onReboot([]() { rp2040.reboot(); });
    coreServer.onBootsel([]() { rp2040.rebootToBootloader(); });
    
    // Register module status callback (appended to core STATUS header)
    coreServer.onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        // Write module-specific status bytes, return count written
        return writeModuleStatus(buf, maxLen);
    });
    
    // Configure module handler with callbacks
    gunfxServer.begin(&Serial);
    gunfxServer.onTriggerOn(handleTriggerOn);
    // ... register other callbacks
    
    // Register handlers with router (order = priority)
    // CRITICAL: coreServer MUST be first!
    commandRouter.begin(&Serial, [](uint8_t err, uint8_t type) {
        gunfxServer.sendNack(err);
    });
    commandRouter.addHandler(&coreServer);   // Priority 1: core commands
    commandRouter.addHandler(&gunfxServer);   // Priority 2: module commands
}

void loop() {
    commandRouter.poll();
    coreServer.updateFreeRam(rp2040.getFreeHeap());  // Keep RAM reading current
}
```

## Changes in v0.5.0

- **INIT parametrized**: INIT now accepts optional `[mode:u8][flags:u8]` payload (backward-compatible, defaults to SLAVE/NONE if empty)
- **InitMode**: SLAVE (0x00) = keep-alive required, DIRECT (0x01) = no keep-alive timeout
- **InitFlags**: VERBOSE (bit 0) = enable async STATUS_UPDATE emissions
- **STATUS_UPDATE (0xEF)**: New async packet type for real-time telemetry — `[source:u8][updateType:u8][data:variable]`
- **Core range expanded**: CoreCommandServer now handles 0xEF-0xFF (was 0xF0-0xFF) to include STATUS_UPDATE
- **Flash storage on Pico**: LightFX and GearControl now support onboard LittleFS flash for standalone config

## Changes in v0.4.0

- **len field widened to u16LE**: Packet header is now 4 bytes `[type:u8][tag:u8][len:u16LE]` (was 3 bytes with `len:u8`)
- **MAX_PAYLOAD_SIZE increased to 512**: Previously 64. Provides headroom for rich STATUS payloads (GearControl: 53 bytes, GunFX: 40 bytes) and future expansion
- **CRC scope updated**: CRC-8 is computed over `type + tag + len(2 bytes) + payload`
- **FUTURE**: If payloads > 512 are needed, packet sequencing/fragmentation should be added rather than increasing the buffer further. This is a known design debt item.

## Changes in v0.3.0

- **Binary-only protocol**: Removed text protocol mode; all packets are binary COBS
- **Binary INIT_READY**: INIT_READY now uses binary length-prefixed payload instead of text
  - Format: `[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]`
- **Rich STATUS**: STATUS response now contains 22-byte core header + module-specific data
  - Core header: `[counter:u32LE][uptime:u32LE][freeRam:u32LE][lastActivity_ms:u32LE][keepaliveCount:u32LE][boardState:u8][initFlags:u8]`
  - GunFX appends 20 bytes (flags, fan, servos, RPM, shots, heater)
  - LightFX appends 22 bytes (LEDs, servos, power readings)
  - GearControl appends 26 bytes (gear states, motor current, servos, LEDs, battery)
- **StatusDataCallback**: Modules register `onStatusData()` on CoreCommandServer
- **updateFreeRam()**: CoreCommandServer tracks live free RAM for STATUS
- **Removed**: `SerialInitHandler`, `SerialBusText`, text protocol classes
- **Removed**: `GunFxSerialMaster/Slave`, `GunFxSerialMasterText/SlaveText`
- **Refactored**: Handler chain uses `ICommandHandler` + `CommandRouter` pattern
- **SFX_* macros**: `SFX_REQUIRE_LEN`, `SFX_VALIDATE`, `SFX_DISPATCH` reduce handler boilerplate

## Changes in v0.2.0

- Added `CorePacket::REBOOT` (0xF8) for software reset
- Added `CorePacket::BOOTSEL` (0xF9) for remote firmware upload
- Added build number to INIT_READY
- Safe shutdown on all system commands and connection loss
