# sfx_serial — Wire Protocol + Client Side

Binary COBS serial communication library — the wire layer plus the
master-side framework. Server-side framework lives in
[`sfx_board`](../../sfx_board/).

**Used by:** every controller (master + slave) and the diagnostics CLI.

## Files

### Wire layer (used by both sides)

| File | Purpose |
|------|---------|
| `serial.h` | Umbrella include — core + client + components |
| `wire.{h,cpp}` | `SfxWire::` — CRC-8, COBS, encode / parse, endian helpers, framing constants |
| `diag_log.{h,cpp}` | `DiagLog` ring-buffered logging singleton (COBS `LOG_MESSAGE` 0xFD packets) |
| `core/core.{h,cpp}` | `CorePacket` (0xEE..0xFF), `SerialError`, `CommandResult`, `CommandHandleResult`, INIT_READY payload encode/decode, `SFX_*` macros, `STATUS_CORE_HEADER_SIZE`, `packetTypeToText()` |

### Generic-expander wire protocol

| File | Purpose |
|------|---------|
| `components/components.h` | `ComponentPacket` (0x01..0x7F), `ComponentError`, runtime callbacks |
| `components/component_kind.h` | `ComponentKind` enum (servo / pwm / led / battery / motor / …) — runtime fingerprint |
| `components/led_status.h` | Wire-format helpers for the LED status broadcast |

### Client layer (master-side)

| File | Purpose |
|------|---------|
| `client/bus.{h,cpp}` | `SerialBus` — COBS framing over USB CDC |
| `client/bus_client.{h,cpp}` | `BusClient` — INIT handshake, tag correlation, `sendCommand` (instant ACK/NACK), `sendQuery` (typed response capture), version-compatibility check |
| `client/result_queue.{h,cpp}` | `ResultQueue` — tag stash + blocking `waitForTag` |

### Archived domain protocols (controllers/archive/sfx_serial_legacy/)

All board-specific wire-protocol headers have been moved out of the
shared library:

| File | Range | Status |
|------|-------|--------|
| `gunfx/gunfx.h` | 0x01-0x2F | archived — replaced by `components/components.h` |
| `lightfx/lightfx.h` | 0x40-0x5F | archived — replaced by `components/components.h` |
| `gearcontrol/gearcontrol.h` | 0x60-0x7F | archived — replaced by `components/components.h` |
| `hubfx/hubfx.h` | 0x80-0xAF | archived — Go mirror at [`app/go/protocol/hubfx/hubfx.go`](../../../../app/go/protocol/hubfx/) is the live source of truth; firmware-side definitions will live in the HubFX controller |

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                       serial.h (umbrella)                          │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  ┌───────────────────────────┐  ┌──────────────────────────────┐   │
│  │ core/core.h               │  │ wire.{h,cpp}                 │   │
│  │ CorePacket  SerialError   │  │ SfxWire:: crc8 / cobsEncode  │   │
│  │ CommandResult             │  │           cobsDecode /       │   │
│  │ CommandHandleResult       │  │           encodePacket /     │   │
│  │ STATUS_CORE_HEADER_SIZE   │  │           parsePacket /      │   │
│  │ packetTypeToText()        │  │           endian helpers     │   │
│  │ SFX_REQUIRE_LEN / VALIDATE│  │ Framing constants            │   │
│  │ SFX_DISPATCH macros       │  │                              │   │
│  └───────────────────────────┘  └──────────────────────────────┘   │
│                                                                    │
│  ┌───────────────────────────┐  ┌──────────────────────────────┐   │
│  │ diag_log.{h,cpp}          │  │ components/                  │   │
│  │ DiagLog ring buffer       │  │ ComponentPacket (0x01..0x7F) │   │
│  │ SFX_LOG_DEBUG/INFO/WARN/  │  │ ComponentKind  led_status    │   │
│  │   ERROR macros            │  │ (generic-expander wire)      │   │
│  │ LOG_MESSAGE (0xFD) packets│  │                              │   │
│  └───────────────────────────┘  └──────────────────────────────┘   │
│                                                                    │
│  ┌───────────────────────────┐  ┌──────────────────────────────┐   │
│  │ client/bus.{h,cpp}        │  │ client/result_queue.{h,cpp}  │   │
│  │ SerialBus                 │  │ ResultQueue                  │   │
│  │ (USB CDC transport)       │  │ (tag-correlated stash)       │   │
│  ├───────────────────────────┤  └──────────────────────────────┘   │
│  │ client/bus_client.{h,cpp} │                                     │
│  │ BusClient                 │                                     │
│  │  sendCommand / sendQuery  │                                     │
│  │  onModulePacket()         │                                     │
│  └───────────────────────────┘                                     │
└────────────────────────────────────────────────────────────────────┘
```

## Wire format

Packet (before COBS encoding):

```
[type:u8] [tag:u8] [len:u16LE] [payload:0..MAX_PAYLOAD_SIZE bytes] [crc8:u8]
```

- **COBS framing** — each encoded packet is followed by a `0x00` frame
  delimiter byte.
- **CRC-8** — polynomial `0x07` over `type + tag + len(2 bytes) + payload`.
- **Tag** — `0x00 = TAG_ASYNC` (unsolicited), `0x01..0xFF` = client-assigned
  correlation ID.
- **Endianness** — little-endian throughout (`SfxWire::putU16LE` /
  `getU32LE` / …).

### Packet type ranges

| Range | Owner | Notes |
|-------|-------|-------|
| 0x01..0x0F | `ComponentPacket` identity / enumeration | Generic-expander wire (`components/components.h`) |
| 0x10..0x2F | `ComponentPacket` servo control | |
| 0x30..0x4F | `ComponentPacket` PWM control | |
| 0x50..0x7F | `ComponentPacket` LED control | |
| 0x80..0xAF | HubFX master commands | wire format is in the Go mirror; firmware-side header parked under `controllers/archive/` |
| 0xA4..0xA6 | `StreamProtocol` BEGIN / DATA / END | `StreamWriter` in `sfx_board/server/stream.h` |
| 0xB0..0xED | Available | |
| 0xEE..0xFF | Universal system commands | Handled by `BoardServicePolicy` (in `sfx_board`) |

## Master-side composition

```cpp
class MyClient : public BusClient {
public:
    CommandResult triggerFire(uint8_t channel) {
        uint8_t buf[1] = { channel };
        return sendCommand(MyPacket::TRIGGER_FIRE, buf, sizeof buf);
    }

    CommandResult readSensor(uint8_t id, SensorReading& out) {
        uint8_t   req[1] = { id };
        SerialPacket resp;
        auto cr = sendQuery(MyPacket::SENSOR_READ_REQ, req, sizeof req,
                            MyPacket::SENSOR_READ_RESP, resp);
        if (!cr.success) return cr;
        // decode `resp.payload` (len = resp.len) into `out`...
        return CommandResult::Ack();
    }

protected:
    void onModulePacket(uint8_t type, uint8_t /*tag*/,
                        const uint8_t* payload, size_t len) override {
        // route async events (TAG_ASYNC)
    }
};
```

## Server-side composition (server side lives in sfx_board)

Server controllers compose service policies via `BoardServer<...UserPolicies>`
from `sfx_board`:

```cpp
using MyBoard = sfx_core::BoardServer<
    AudioServicePolicy<Mixer>,
    StorageServicePolicy<Esp32StoragePolicy>>;

MyBoard board;

void setup() { board.begin("MyBoard", FIRMWARE_VERSION, BUILD_NUMBER); }
void loop()  { board.process(); }
```

See [sfx_board/README.md](../../sfx_board/README.md) for the full
server-side framework.
