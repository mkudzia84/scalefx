# NoOp ESP32-S3 Controller

Minimal ESP32-S3 controller that implements the base protocol layer plus servo control. Useful for:

- Testing binary COBS serial protocol on ESP32-S3 hardware
- Verifying 6Mbps UART communication via USB-UART bridge
- Testing servos on ESP32-S3 GPIO
- Template for new ESP32-S3 controller implementations
- Protocol development and debugging

## Architecture

Uses `SfxServer` component for common server boilerplate (serial init, device naming, indicator LEDs, core protocol, connection management). Includes a minimal `NoOpServoHandler` for direct servo positioning using the same `SERVO_SET` (0x64) packet as GearControl.

## Features

### Core Commands (via SfxServer)

| Packet Type | Name | Description |
|-------------|------|-------------|
| 0xFE | IDENTIFY | Board type discovery (no state change) |
| 0xF0 | INIT | Initialize connection, returns INIT_READY |
| 0xF1 | SHUTDOWN | Safe shutdown (centers servos) |
| 0xF8 | REBOOT | System reboot |
| 0xFA | STATUS_REQ | Request status (core + servo positions) |
| 0xF5 | KEEPALIVE | Connection keepalive |
| 0xF6 | I2C_SCAN | I2C bus diagnostics |
| 0xFD | LOG_MESSAGE | Diagnostic log retrieval |
| 0xFF | DIAG_HISTORY | Diagnostic history dump |

### Module Commands

| Packet Type | Name | Payload | Description |
|-------------|------|---------|-------------|
| 0x64 | SERVO_SET | `[servo_id:u8][pulse_us:u16LE]` | Set servo position (500-2500µs) |

## Status Response

Binary status response (0xF4) payload:
- Core header (20 bytes): `[counter:u32LE][uptime:u32LE][freeRam:u32LE][lastActivity_ms:u32LE][keepaliveCount:u32LE]`
- Module data (14 bytes): `[servo0_us:u16LE] ... [servo6_us:u16LE]`

## Hardware

- **Board**: ESP32-S3-DevKitC-1 (WROOM-1 N8R8)
- **CPU**: Dual Xtensa LX7 @ 240 MHz
- **Flash**: 8MB QIO
- **PSRAM**: 8MB OPI
- **LED**: Onboard RGB LED (GP48) for connection status
- **Serial**: UART0 via USB-UART bridge @ 6Mbps

## GPIO Pin Mapping

| Pin | Function |
|-----|----------|
| GP1 | Servo 0 |
| GP2 | Servo 1 |
| GP3 | Servo 2 |
| GP4 | I2C SDA |
| GP5 | I2C SCL |
| GP6 | Servo 3 |
| GP7 | Servo 4 |
| GP8 | Servo 5 |
| GP9 | Servo 6 |
| GP48 | Connection LED |

## Building

```bash
# Build
cd controllers/noop/esp32s3
python -m platformio run -e esp32s3

# Build and flash via centralized script
python scripts/build_and_flash.py noop-esp
```

## Flashing

```bash
# Via esptool (automatic)
pio run -t upload

# Via centralized script
python scripts/build_and_flash.py noop-esp --port COM16
```

## Serial Settings

- **Baud**: 6000000 (6Mbps)
- **Protocol**: Binary COBS with CRC-8
- **Packet format**: `[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]` (COBS encoded)

## Version History

| Version | Build | Date | Changes |
|---------|-------|------|---------|
| 0.1.0 | 1 | 2026-04-04 | Initial release. Core protocol + servo control on ESP32-S3-DevKitC-1. Same command set as NoOp Pico but targeting ESP32-S3 with UART0 serial, ESP-IDF log capture, and FreeRTOS task yielding. |
