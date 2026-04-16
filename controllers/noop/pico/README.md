# NoOp Pico Controller

Minimal Raspberry Pi Pico controller that implements only the base protocol layer. Useful for:

- Testing binary COBS serial protocol without hardware dependencies
- Template for new controller implementations
- Protocol development and debugging

## Architecture

Uses `PicoServer` component for common server boilerplate (serial init, device naming, indicator LEDs, core protocol, connection management). No module-specific command handler — core protocol only.

## Features

Implements only the core system commands via binary COBS protocol:

| Packet Type | Name | Description |
|-------------|------|-------------|
| 0xF0 | INIT | Initialize connection, returns INIT_READY (optional payload: mode, flags) |
| 0xF1 | SHUTDOWN | Safe shutdown |
| 0xF8 | REBOOT | System reboot |
| 0xF9 | BOOTSEL | Enter USB bootloader mode |
| 0xFA | STATUS_REQ | Request status |

## Status Response

Binary status response (0xF4) payload:
- Core header (22 bytes): `[counter:u32LE][uptime:u32LE][freeRam:u32LE][lastActivity_ms:u32LE][keepaliveCount:u32LE][boardState:u8][initFlags:u8]`
- No module data (core header only)

## Hardware

- **Board**: Raspberry Pi Pico (RP2040)
- **LED**: Onboard LED (GP25) for status indication
  - 3 quick blinks on boot
  - 2 blinks on successful INIT
  - Heartbeat every 2 seconds when connected

## Building

```bash
cd controllers/noop/pico
pio run
```

## Flashing

1. Hold BOOTSEL button while plugging in Pico
2. Copy `.pio/build/pico/firmware.uf2` to RPI-RP2 drive

Or use the serial BOOTSEL command if firmware is already running:
```
INIT
BOOTSEL
```

## Serial Settings

- **Baud**: 1000000 (1Mbps)
- **Protocol**: Binary COBS with CRC-8
- **Packet format**: `[type:u8][len:u16LE][payload:0-512][crc8:u8]` (COBS encoded)

## Version History

- **v0.2.0** - Binary-only protocol
  - Removed text protocol support
  - Using new serial library (CoreCommandServer + CommandRouter)
  - COBS framing with CRC-8 validation

- **v0.1.0** - Initial release
  - Text-based protocol
