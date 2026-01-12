# GunFX Pico Binary Protocol Test Suite

Functional test suite for the GunFX Pico board using the binary serial protocol with COBS encoding.

## Prerequisites

- Python 3.8+
- pytest
- pyserial
- GunFX board connected via USB (typically `/dev/ttyACM0` on Linux or `COM3` on Windows)

## Installation

```bash
pip install pytest pyserial
```

## Configuration

Set the `GUNFX_PORT` environment variable to specify the serial port:

```bash
# Linux
export GUNFX_PORT=/dev/ttyACM0

# Windows
set GUNFX_PORT=COM3
```

Default port is `COM3` if not specified.

## Running Tests

### Run binary protocol tests
```bash
cd controllers/gunfx/pico
pytest tests/binary/ -v
```

### Run specific test file
```bash
pytest tests/binary/test_trigger.py -v
pytest tests/binary/test_servo.py -v
```

### Run by marker
```bash
# Only hardware tests
pytest tests/binary/ -m hardware -v

# Skip firing tests
pytest tests/binary/ -m "not firing" -v

# Skip destructive tests (REBOOT, BOOTSEL)
pytest tests/binary/ -m "not destructive" -v

# Only binary protocol tests
pytest tests/binary/ -m binary -v
```

### Quick validation
```bash
pytest tests/binary/ -m "not slow and not destructive" -v
```

## Binary Protocol Overview

### Packet Format

Before COBS encoding:
```
[type:u8][len:u8][payload:len bytes][crc:u8]
```

- **type**: Packet type (see below)
- **len**: Payload length in bytes
- **payload**: Command-specific data
- **crc**: CRC-8 polynomial 0x07 over type+len+payload

After encoding, packets are COBS-encoded and terminated with `0x00`.

### Packet Types

| Type | Value | Description |
|------|-------|-------------|
| `GUNFX_PKT_TRIGGER_ON` | 0x01 | Start firing |
| `GUNFX_PKT_TRIGGER_OFF` | 0x02 | Stop firing |
| `GUNFX_PKT_SRV_SET` | 0x10 | Set servo position |
| `GUNFX_PKT_SRV_SETTINGS` | 0x11 | Configure servo |
| `GUNFX_PKT_SRV_RECOIL_JERK` | 0x12 | Configure recoil jerk |
| `GUNFX_PKT_SMOKE_HEAT` | 0x20 | Control heater |
| `SFX_PKT_SHUTDOWN` | 0xF1 | Clean shutdown |
| `SFX_PKT_KEEPALIVE` | 0xF2 | Heartbeat |
| `SFX_PKT_STATUS` | 0xF4 | Telemetry |
| `SFX_PKT_ACK` | 0xF6 | Acknowledgment |
| `SFX_PKT_NACK` | 0xF7 | Negative ack |
| `SFX_PKT_REBOOT` | 0xF8 | Reboot device |
| `SFX_PKT_BOOTSEL` | 0xF9 | Enter bootloader |

### Payload Formats

| Command | Payload |
|---------|---------|
| TRIGGER_ON | `[rpm:u16]` |
| TRIGGER_OFF | `[fanDelayMs:u16]` |
| SERVO_SET | `[id:u8][pulseUs:u16]` |
| SERVO_CONFIG | `[id:u8][minUs:u16][maxUs:u16][speed:u16][accel:u16][decel:u16]` |
| SERVO_RECOIL_JERK | `[id:u8][jerkUs:u16][varianceUs:u16]` |
| SMOKE_HEAT | `[enable:u8]` (0=off, 1=on) |
| STATUS | `[flags:u8][fanOff:u16][srv0:u16][srv1:u16][srv2:u16][rpm:u16]` |
| NACK | `[code:u8][reason:string]` |

### STATUS Flags Byte

| Bit | Flag |
|-----|------|
| 0 | firing |
| 1 | flashActive |
| 2 | flashFading |
| 3 | heaterOn |
| 4 | fanOn |
| 5 | fanSpindown |

## Test Files

| File | Description |
|------|-------------|
| `conftest.py` | Test configuration, COBS/CRC helpers, `GunFxBinaryConnection` class |
| `test_system.py` | INIT, SHUTDOWN, KEEPALIVE, REBOOT, BOOTSEL tests |
| `test_trigger.py` | TRIGGER_ON, TRIGGER_OFF command tests |
| `test_servo.py` | SERVO_SET, SERVO_CONFIG, SERVO_RECOIL_JERK tests |
| `test_smoke.py` | SMOKE_HEAT command tests |
| `test_status.py` | STATUS telemetry format and content tests |
| `test_errors.py` | Error handling, NACK responses, CRC validation tests |

## GunFxBinaryConnection Class

The `GunFxBinaryConnection` class provides a convenient interface for binary protocol:

```python
from conftest import GunFxBinaryConnection

conn = GunFxBinaryConnection(port="COM3")
if conn.connect():
    # High-level commands
    success, _ = conn.trigger_on(rpm=600)
    success, _ = conn.servo_set(servo_id=1, pulse_us=1500)
    success, _ = conn.smoke_heat(enable=True)
    
    # Wait for STATUS telemetry
    status = conn.wait_for_status(timeout=2.0)
    print(f"Firing: {status.get('firing')}, RPM: {status.get('rpm')}")
    
    # Low-level packet send/receive
    conn.send_packet(GUNFX_PKT_TRIGGER_OFF, struct.pack('<H', 0))
    pkt_type, payload = conn.receive_packet()
    
    conn.close()
```

## COBS Encoding

COBS (Consistent Overhead Byte Stuffing) eliminates 0x00 bytes from packets, allowing 0x00 to be used as delimiter:

```python
from conftest import cobs_encode, cobs_decode

# Encode data
encoded = cobs_encode(b'\x01\x02\x00\x03')  # -> removes zeros

# Decode received data (without delimiter)
decoded = cobs_decode(encoded[:-1])  # Remove trailing 0x00
```

## CRC-8 Calculation

```python
from conftest import crc8

# Calculate CRC over packet header + payload
data = bytes([pkt_type, length]) + payload
checksum = crc8(data)
```

## Test Markers

| Marker | Description |
|--------|-------------|
| `hardware` | Requires connected GunFX hardware |
| `binary` | Tests using binary protocol |
| `firing` | Tests that trigger gun effects |
| `servo` | Tests that move servo motors |
| `smoke` | Tests that control smoke generator |
| `slow` | Tests that take longer than 5 seconds |
| `destructive` | Tests that reboot/reset the device |

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x01 | UNKNOWN_COMMAND | Command not recognized |
| 0x02 | INVALID_PARAMETER | Parameter value invalid |
| 0x03 | MISSING_PARAMETER | Required parameter missing |
| 0x20 | SERVO_INVALID_ID | Servo ID out of range (1-3) |
| 0x21 | SERVO_PULSE_RANGE | Pulse width out of range |
| 0x22 | SERVO_MIN_MAX | Min/max configuration error |
| 0x40 | INVALID_RPM | RPM out of range (60-3000) |

## Comparing Text vs Binary Tests

Both test suites (`tests/` for text, `tests/binary/` for binary) test the same GunFX functionality but use different protocols:

| Aspect | Text Protocol | Binary Protocol |
|--------|---------------|-----------------|
| INIT | `INIT protocol=text\n` | `INIT protocol=binary\n` |
| Commands | `TRIGGER_ON rpm=600\n` | COBS packet with payload |
| Responses | `ACK\n`, `NACK code=...\n` | COBS packet 0xF6/0xF7 |
| STATUS | `STATUS firing=1 rpm=600...\n` | COBS packet with flags byte |
| Overhead | Higher (text parsing) | Lower (compact binary) |
| Debugging | Easier (human readable) | Harder (requires decoder) |

## Troubleshooting

### "No device found"
- Check `GUNFX_PORT` environment variable
- Verify device is connected

### "CRC error"
- Check for data corruption
- Verify COBS encoding is correct

### "NACK received"
- Check error code in payload
- Verify parameter values are in valid ranges

### "Timeout waiting for response"
- Device may have disconnected
- Try reconnecting
