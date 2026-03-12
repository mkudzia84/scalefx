# Text Protocol Commands Reference

This document lists all text protocol commands for manual testing via serial terminal.

**Protocol:** Line-based text commands at 1Mbps baud  
**Format:** `COMMAND_NAME key=value key2=value2\n`  
**Note:** Parameters are space-separated key=value pairs. Order doesn't matter.

**ACK/NACK:** All commands return either `ACK` on success or `NACK code=N reason=...` on error.

---

## Error Codes

Error codes are defined in two layers:
- **Generic errors** (`SerialError` namespace in `core/core.h`): 0x00-0x1F, 0xF0-0xFF
- **Domain-specific errors** (e.g., `GunFxError` namespace): 0x20-0x4F

### Generic Errors (All Modules)

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | No error (ACK) |
| 0x01 | UNKNOWN | Unknown error |
| 0x02 | NOT_INITIALIZED | Module not initialized |
| 0x03 | INVALID_COMMAND | Unknown command |
| 0x04 | MISSING_PARAMETER | Required parameter missing |
| 0x05 | BUSY | Module busy, try again |
| 0x06 | NOT_SUPPORTED | Command not supported |
| 0x07 | PERMISSION_DENIED | Operation not permitted |
| 0x10 | INVALID_PARAM | Generic invalid parameter |
| 0x11 | PARAM_OUT_OF_RANGE | Parameter value out of range |
| 0x12 | INVALID_ID | Invalid device/channel ID |
| 0x13 | INVALID_VALUE | Invalid parameter value |
| 0x14 | PARAM_TOO_LONG | Parameter string too long |
| 0xF0 | INTERNAL_ERROR | Internal error |
| 0xF1 | TIMEOUT | Operation timed out |
| 0xF2 | COMM_ERROR | Communication error |
| 0xF3 | BUFFER_OVERFLOW | Buffer overflow |
| 0xF4 | CRC_ERROR | CRC check failed |
| 0xF5 | FRAMING_ERROR | Framing/decode error |

### Domain-Specific Error Code Ranges

| Range | Module | Description |
|-------|--------|-------------|
| 0x20-0x4F | GunFX | Servo, smoke, trigger errors |
| 0x50-0x6F | EngineFX | Reserved for engine module |
| 0x70-0x7F | HubFX | Reserved for hub module |

---

## System Commands

These commands work in any state and are handled by `SerialInitHandler`.

### INIT

Initialize connection and negotiate protocol mode.

```
INIT protocol=text
INIT protocol=binary
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `protocol` | string | `text` or `binary` (default: `text`) |

**Response:** `INIT_READY name=... version=... platform=... cpuMHz=... ramBytes=...`

**Note:** INIT command is always text-based, even when requesting binary mode.

---

### SHUTDOWN

Perform safe shutdown of all outputs.

```
SHUTDOWN
```

No parameters. Returns `ACK` on success.

---

### REBOOT

Reboot the microcontroller.

```
REBOOT
```

No parameters. **Fire-and-forget** - no ACK expected (device reboots immediately).

---

### BOOTSEL

Enter firmware upload mode (USB bootloader).

```
BOOTSEL
```

No parameters. **Fire-and-forget** - no ACK expected (device enters bootloader immediately).

---

### KEEPALIVE

Keep connection alive (reset watchdog timer).

```
KEEPALIVE
```

No parameters. Returns `ACK` on success.

---

## Response Commands

These are sent from slave to master.

### INIT_READY

Response to INIT command.

```
INIT_READY name=GunFX-1A2B version=0.2.0 platform=RP2040 cpuMHz=133 ramBytes=200000
```

| Field | Description |
|-------|-------------|
| `name` | Device name with unique ID suffix |
| `version` | Firmware version (no "v" prefix) |
| `platform` | Hardware platform |
| `cpuMHz` | CPU frequency in MHz |
| `ramBytes` | Free RAM in bytes |

---

### ACK / NACK

Command acknowledgment responses.

**ACK** - Command executed successfully:
```
ACK
```

**NACK** - Command failed with error:
```
NACK code=32 reason=Invalid servo ID (use 1-3)
NACK code=64 reason=Invalid RPM (use 1-3000)
```

| Field | Type | Description |
|-------|------|-------------|
| `code` | uint8 | Error code (see Error Codes table) |
| `reason` | string | Human-readable error message |

---

### ERROR

Asynchronous error notification (not in response to a command).

```
ERROR code=1 msg=Connection timeout
```

| Field | Description |
|-------|-------------|
| `code` | Error code (uint8) |
| `msg` | Human-readable message (optional) |

---

## Module-Specific Commands

See the documentation for each module:
- [GunFX Commands](../../gunfx/pico/docs/COMMANDS.md) - Gun effects commands
- HubFX Commands - Hub controller commands (coming soon)
