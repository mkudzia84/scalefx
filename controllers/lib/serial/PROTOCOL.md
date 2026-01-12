# ScaleFX Serial Protocol

## Overview

Serial communication protocol for ScaleFX controllers (HubFX, GunFX).

**Version:** 0.2.1  
**Binary Framing:** COBS encoding with 0x00 delimiter  
**CRC:** CRC-8 polynomial 0x07 over type+len+payload  

## Protocol Negotiation

The INIT handshake is **always text-based** regardless of the final protocol mode.
This ensures reliable connection establishment before switching to binary mode.

### Connection Flow

```
Master                              Slave
  |                                   |
  |  INIT protocol=binary\n           |
  |---------------------------------->|
  |                                   |  (switches to binary mode)
  |  INIT_READY name=... version=...\n|
  |<----------------------------------|
  |                                   |
  |  [Binary COBS packets]            |
  |<=================================>|
```

### Reconnection

If the master sends a new INIT command (e.g., after disconnect/reconnect), the slave:
1. Resets connection state
2. Processes the new INIT command
3. Switches to the requested protocol mode
4. Responds with INIT_READY

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
| `SFX_PKT_INIT` | 0xF0 | `INIT` | Master→Slave | Initialize slave device |
| `SFX_PKT_SHUTDOWN` | 0xF1 | `SHUTDOWN` | Master→Slave | Shutdown slave device |
| `SFX_PKT_KEEPALIVE` | 0xF2 | `KEEPALIVE` | Master→Slave | Connection keepalive |
| `SFX_PKT_INIT_READY` | 0xF3 | `INIT_READY` | Slave→Master | Slave ready response |
| `SFX_PKT_STATUS` | 0xF4 | `STATUS` | Slave→Master | Status telemetry |
| `SFX_PKT_ERROR` | 0xF5 | `ERROR` | Slave→Master | Error notification |
| `SFX_PKT_ACK` | 0xF6 | `ACK` | Slave→Master | Acknowledgment |
| `SFX_PKT_NACK` | 0xF7 | `NACK` | Slave→Master | Negative acknowledgment |
| `SFX_PKT_REBOOT` | 0xF8 | `REBOOT` | Master→Slave | Reboot slave device |
| `SFX_PKT_BOOTSEL` | 0xF9 | `BOOTSEL` | Master→Slave | Enter BOOTSEL mode |

### INIT Command

**Always sent as text**, even when requesting binary protocol mode.

**Format:**
```
INIT protocol=binary keepalive=30000\n
INIT protocol=text keepalive=off\n
```

**Parameters:**
- `protocol` - Protocol mode for subsequent communication: `binary` or `text`
- `keepalive` - Keepalive interval in milliseconds, or `off` to disable (optional, defaults to `off`)

**Keepalive Behavior:**
- **Master side:** Emits a KEEPALIVE if no other message has been sent within the interval.
  This ensures at least one message (of any type) is sent per interval.
- **Slave side:** Monitors for ANY incoming message within 1.5× the keepalive interval.
  If no message is received within this timeout, the slave treats it as connection loss
  and performs a safe shutdown (stops firing, disables heater, etc.).

**Example:**
- `keepalive=30000` → Master sends at least one message every 30s, slave times out after 45s of silence.
- `keepalive=off` → No keepalive monitoring (use fallback timeout).

**Behavior:**
1. Slave resets any existing connection state
2. Slave stores the negotiated keepalive interval
3. Slave switches to the requested protocol mode
4. Slave responds with INIT_READY (always text)
5. All subsequent communication uses the negotiated protocol

### INIT_READY Response

**Always sent as text**, regardless of negotiated protocol mode.

**Format:**
```
INIT_READY name=GunFX-A4B2 version=0.1.0 build=42 platform=RP2040 cpuMHz=120 ramBytes=221624
```

**Fields:**
- `name` - Device name with unique ID (e.g., "GunFX-A4B2")
- `version` - Firmware version without "v" prefix (e.g., "0.1.0")
- `build` - Build number (incremented with each build)
- `platform` - Hardware platform (e.g., "RP2040")
- `cpuMHz` - CPU frequency in MHz
- `ramBytes` - Free RAM in bytes

**Note:** Version should NOT include "v" prefix (use "0.1.0" not "v0.1.0").

### SHUTDOWN Command

Requests graceful shutdown of the slave device. The slave performs a safe shutdown
(stops firing, disables heater/fan, resets servos to neutral) but remains running.

**Text Format:** `SHUTDOWN\n`  
**Binary Payload:** None  
**Response:** None (fire-and-forget)

**Safe Shutdown Actions (GunFX):**
- Stops firing (rate = 0)
- Disables smoke heater
- Disables smoke fan
- Turns off nozzle flash LED
- Resets all servos to neutral position (1500µs)

### KEEPALIVE Command

Connection heartbeat to maintain connection state.

**Text Format:** `KEEPALIVE\n`  
**Binary Payload:** None

**Usage:**
- Sent automatically by master if no other message was sent within the keepalive interval.
- Resets the slave's connection timeout timer (just like any other message).
- If keepalive is disabled (`keepalive=off` in INIT), this command is not sent automatically,
  but the slave may still use a fallback timeout based on other message activity.

### REBOOT Command

Triggers software reset via `rp2040.reboot()`. Performs safe shutdown before rebooting.

**Text Format:** `REBOOT\n`  
**Binary Payload:** None  
**Response:** None (fire-and-forget, device reboots)

### BOOTSEL Command

Triggers entry to BOOTSEL mode via `rp2040.rebootToBootloader()`. Performs safe shutdown before entering bootloader.

**Text Format:** `BOOTSEL\n`  
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

**Binary Payload:**
```
[flags:u8][fanOffRemainingMs:u16][servo0Us:u16][servo1Us:u16][servo2Us:u16][rpm:u16]
```

Flags:
- Bit 0: firing
- Bit 1: flashActive
- Bit 2: flashFading
- Bit 3: heaterOn
- Bit 4: fanOn
- Bit 5: fanSpindown

**Text Format:**
```
STATUS firing=1 flashActive=0 flashFading=0 heaterOn=1 fanOn=1 fanSpindown=0 fanOffRemainingMs=0 servo0=1500 servo1=1500 servo2=1500 rpm=600
```

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
1. Send `BOOTSEL\n` via serial (text command, works in any mode)
2. Slave enters BOOTSEL mode automatically
3. RPI-RP2 drive appears
4. Copy `firmware.uf2` to drive
5. Device auto-reboots with new firmware

## Implementation Classes

| Class | Description |
|-------|-------------|
| `SerialInitHandler` | Protocol negotiation (always text), handles INIT/INIT_READY |
| `SerialBus` | Binary COBS protocol over USB CDC |
| `SerialBusText` | Text protocol over Stream |
| `GunFxSerialMaster` | GunFX master (binary protocol) |
| `GunFxSerialSlave` | GunFX slave (binary protocol) |
| `GunFxSerialMasterText` | GunFX master (text protocol) |
| `GunFxSerialSlaveText` | GunFX slave (text protocol) |

### SerialInitHandler Pattern

The `SerialInitHandler` class handles protocol negotiation separately from the
protocol-specific handlers. This allows:

1. **Clean separation** - Init logic is reusable across different protocols
2. **Reconnection support** - New INIT resets state and re-negotiates
3. **Protocol switching** - Slave can switch between text/binary based on INIT

**Slave-side usage:**
```cpp
SerialInitHandler initHandler;
GunFxSerialSlave binarySlave;
GunFxSerialSlaveText textSlave;
IGunFxSlave* activeSlave = nullptr;

void performSafeShutdown() {
    // Stop firing, disable heater/fan, reset servos to neutral
}

void setup() {
    Serial.begin(115200);
    
    initHandler.begin(&Serial, "GunFX-1234");
    initHandler.setBoardInfo("0.1.0", 1, "RP2040", 120, freeRam);  // version, build, platform, MHz, RAM
    
    initHandler.onInitComplete([](ProtocolMode mode) {
        if (mode == ProtocolMode::Binary) {
            binarySlave.begin(&Serial, "GunFX-1234");
            activeSlave = &binarySlave;
        } else {
            textSlave.begin(&Serial, "GunFX-1234");
            activeSlave = &textSlave;
        }
        // Register command callbacks on activeSlave...
    });
    
    initHandler.onInitReset([]() {
        performSafeShutdown();
        if (activeSlave) activeSlave->end();
        activeSlave = nullptr;
    });
    
    initHandler.onShutdown([]() { performSafeShutdown(); });
    initHandler.onReboot([]() { performSafeShutdown(); rp2040.reboot(); });
    initHandler.onBootsel([]() { performSafeShutdown(); rp2040.rebootToBootloader(); });
    initHandler.onConnectionLoss([]() { performSafeShutdown(); });
}

void loop() {
    // InitHandler always processes first (watches for INIT, system commands)
    if (initHandler.process()) {
        return; // INIT was handled, protocol may have switched
    }
    
    // Process with active protocol
    if (activeSlave) {
        activeSlave->process();
    }
}
```

**Master-side usage:**
```cpp
// Master sends INIT with protocol preference
void connectSlave(Stream* serial, bool useBinary) {
    // Always send text INIT
    serial->print("INIT protocol=");
    serial->println(useBinary ? "binary" : "text");
    
    // Wait for INIT_READY (text response)
    // Then switch to appropriate protocol handler
}
```

## Changes in v0.2.1

- **Build number**: Added `build` field to INIT_READY response
- **System commands**: SHUTDOWN, REBOOT, BOOTSEL are all fire-and-forget (no ACK)
- **Safe shutdown**: All system commands and connection loss trigger safe shutdown
  - Stops firing, disables heater/fan, resets servos to neutral
- **Simplified slave interface**: Removed `setBoardInfo()` from `IGunFxSlave` interface
  - Board info is only set on `SerialInitHandler` which sends INIT_READY
- **Connection loss callback**: Added `onConnectionLoss()` to `SerialInitHandler`

## Changes in v0.2.0

- **Protocol negotiation**: INIT command now specifies `protocol=text|binary`
- **Always-text handshake**: INIT and INIT_READY are always text, regardless of mode
- **Reconnection support**: New INIT resets connection and re-negotiates protocol
- **SerialInitHandler**: New class for protocol negotiation (factored out)
- **Version format**: INIT_READY version field no longer includes "v" prefix

## Changes in v0.1.0

- Combined `serial_common` and `serial_gunfx` into unified `serial` library
- Added text protocol implementations for testing
- Added `SFX_PKT_REBOOT` (0xF8) for software reset
- Added `SFX_PKT_BOOTSEL` (0xF9) for remote firmware upload
- Added `TextParse` namespace for parsing key=value arguments
