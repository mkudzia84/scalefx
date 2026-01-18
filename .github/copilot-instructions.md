# ScaleFX Project Instructions

> **AI AGENTS:** Read the `/instructions/` folder for complete development guidelines.

## Quick Reference

This is a modular scale model effects system with:
- **Protocol:** Binary COBS with CRC-8, 115200 baud
- **Packet:** `[type:u8][len:u8][payload:0-64][crc8:u8]`
- **Endianness:** Little-endian for all multi-byte values

## Key Documents

| Task | Document |
|------|----------|
| Understand architecture | `/instructions/01-ARCHITECTURE.md` |
| Create new controller | `/instructions/02-NEW-CONTROLLER.md` |
| Add commands to controller | `/instructions/03-PROTOCOL-EXTENSION.md` |
| Verify all files updated | `/instructions/04-CHANGE-PROPAGATION.md` |
| Build and flash firmware | `/instructions/05-BUILD-AND-FLASH.md` |
| Run or write tests | `/instructions/06-TEST-SUITE.md` |
| Update CLI | `/instructions/07-CLI-UPDATES.md` |

## Critical Rules

1. **Always sync constants** between C++ (`lib/serial/`) and Python (`tests/framework/packets.py`)
2. **Always update CLI** when adding commands (`tests/cli/interactive.py`)
3. **Always build before testing** - use `scripts/build_and_flash.py`
4. **Use little-endian** for all multi-byte values

## File Locations

```yaml
Serial_Library: "controllers/lib/serial/"
Controllers: "controllers/{name}/pico/"
Python_Tests: "tests/"
CLI: "tests/cli/interactive.py"
```

## Packet Type Ranges

| Range | Module | Status |
|-------|--------|--------|
| 0x01-0x2F | GunFX | Used |
| 0x40-0x5F | LightFX | Used |
| 0x60-0xEF | Available | Free |
| 0xF0-0xFF | Core | Reserved |
