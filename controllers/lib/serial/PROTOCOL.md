# ScaleFX Serial Protocol

## Overview

Serial communication protocol for ScaleFX controllers (HubFX, GunFX).

**Version:** 0.3.0  
**Binary Framing:** COBS encoding with 0x00 delimiter  
**CRC:** CRC-8 polynomial 0x07 over type+len+payload  

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
[type:u8][len:u8][payload:len bytes][crc:u8]
```

### Text Protocol

Human-readable line-based format:
```
COMMAND_NAME key=value key2=value2\n
```

## Universal Packet Types (0xF0-0xFF)

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

### INIT Command

**Binary packet** (type 0xF0), no payload.

**Behavior:**
1. Slave resets any existing connection state
2. Slave performs safe shutdown of all active operations
3. Slave responds with INIT_READY containing device info

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
```

**Fields:**
- `name` - Device name with unique ID (e.g., "GunFX-A4B2")
- `version` - Firmware version without "v" prefix (e.g., "0.2.0")
- `platform` - Hardware platform (e.g., "RP2040")
- `cpuMHz` - CPU frequency in MHz (u32LE)
- `freeRam` - Free RAM in bytes at boot (u32LE)
- `buildNum` - Build number, incremented with each build (u32LE)

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
**Response:** None (fire-and-forget, device enters BOOTSEL mode, RPI-RP2 drive appears)

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

**Core Header (12 bytes, always present):**
```
[counter:u32LE][uptime:u32LE][freeRam:u32LE]
```

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

**NoOp:** Core header only (12 bytes, no module data).

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
| `CoreProtocol` | `serial_core.h` | COBS encoding, CRC-8, packet building/parsing |
| `ISerialCore` | `serial_core.h` | Abstract interface for serial I/O |
| `CoreCommandServer` | `serial_core.h` | Server-side system command handler (INIT, STATUS, REBOOT, etc.) |
| `ICommandHandler` | `serial_command_handler.h` | Handler interface (`tryProcess()`) |
| `CommandRouter` | `serial_command_handler.h` | Routes packets to registered handlers |
| `GunFxServer` | `serial_gunfx.h` | GunFX command handler (server) |
| `GunFxClient` | `serial_gunfx.h` | GunFX command sender (client/hub) |
| `LightFxServer` | `serial_lightfx.h` | LightFX command handler (server) |
| `LightFxClient` | `serial_lightfx.h` | LightFX command sender (client/hub) |
| `GearControlServer` | `serial_gearcontrol.h` | GearControl command handler (server) |
| `GearControlClient` | `serial_gearcontrol.h` | GearControl command sender (client/hub) |
| `SerialBus` | `serial_bus.h` | Client-side serial bus (USB host) |
| `UsbHost` | `serial_bus.h` | PIO-USB host for HubFX |

### Server-Side Pattern

```cpp
CommandRouter commandRouter;
CoreCommandServer coreServer;
GunFxServer gunfxServer;

void setup() {
    Serial.begin(115200);
    
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

## Changes in v0.3.0

- **Binary-only protocol**: Removed text protocol mode; all packets are binary COBS
- **Binary INIT_READY**: INIT_READY now uses binary length-prefixed payload instead of text
  - Format: `[nameLen:u8][name][verLen:u8][ver][platLen:u8][plat][cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE]`
- **Rich STATUS**: STATUS response now contains 12-byte core header + module-specific data
  - Core header: `[counter:u32LE][uptime:u32LE][freeRam:u32LE]`
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
