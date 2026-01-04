# ScaleFX Serial Protocol

## Overview

COBS-framed serial communication protocol for ScaleFX controllers (HubFX, GunFX).

**Version:** 0.1.0  
**Framing:** COBS encoding with 0x00 delimiter  
**CRC:** CRC-8 polynomial 0x07 over type+len+payload  

## Packet Format

Before COBS encoding:
```
[type:u8][len:u8][payload:len bytes][crc:u8]
```

## Universal Packet Types (0xF0-0xFF)

System-level commands common to all ScaleFX devices:

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `SFX_PKT_INIT` | 0xF0 | Master→Slave | Initialize slave device |
| `SFX_PKT_SHUTDOWN` | 0xF1 | Master→Slave | Shutdown slave device |
| `SFX_PKT_KEEPALIVE` | 0xF2 | Master→Slave | Connection keepalive |
| `SFX_PKT_INIT_READY` | 0xF3 | Slave→Master | Slave ready response |
| `SFX_PKT_STATUS` | 0xF4 | Slave→Master | Status telemetry |
| `SFX_PKT_ERROR` | 0xF5 | Slave→Master | Error notification |
| `SFX_PKT_ACK` | 0xF6 | Slave→Master | Acknowledgment |
| `SFX_PKT_NACK` | 0xF7 | Slave→Master | Negative acknowledgment |
| `SFX_PKT_REBOOT` | 0xF8 | Master→Slave | Reboot slave device |
| `SFX_PKT_BOOTSEL` | 0xF9 | Master→Slave | Enter BOOTSEL mode (firmware upload) |

### INIT_READY Response

When a slave device receives `SFX_PKT_INIT` (0xF0), it responds with `SFX_PKT_INIT_READY` (0xF3) containing board information.

**Payload Format:** Pipe-delimited string
```
name|version|platform|cpuMHz|ramBytes
```

**Example:**
```
GunFX-A4B2|v0.1.0|RP2040|120|221624
```

**Fields:**
- `name` - Device name with unique ID (e.g., "GunFX-A4B2")
- `version` - Firmware version (e.g., "v0.1.0")
- `platform` - Hardware platform (e.g., "RP2040")
- `cpuMHz` - CPU frequency in MHz (e.g., "120")
- `ramBytes` - Free RAM in bytes (e.g., "221624")

**C++ Master (receiving):**
```cpp
GunFxSerialMaster master;
master.onReady([](const char* name) {
    const GunFxBoardInfo& info = master.boardInfo();
    Serial.printf("Connected: %s (v%s, %s, %luMHz, %lu bytes RAM)\n",
                  info.deviceName, info.firmwareVersion, 
                  info.platform, info.cpuFrequencyMHz, info.freeRamBytes);
});
```

**C++ Slave (sending):**
```cpp
serialSlave.begin(&Serial, "GunFX-1234");
serialSlave.setBoardInfo("v0.1.0", "RP2040", 120, rp2040.getFreeHeap());
// When INIT received, automatically sends INIT_READY with board info
```

### Reboot Command

Sends `SFX_PKT_REBOOT` (0xF8) to slave device, triggering a software reset via `rp2040.reboot()`.

**Payload:** None  
**Response:** None (device reboots)  

**Example Usage (C++ Master):**
```cpp
GunFxSerialMaster master;
master.sendReboot();  // Slave will reboot
```

**Example Handler (C++ Slave):**
```cpp
serialSlave.onReboot([]() {
    Serial.println("Rebooting...");
    rp2040.reboot();
});
```

### BOOTSEL Command

Sends `SFX_PKT_BOOTSEL` (0xF9) to slave device, triggering entry to BOOTSEL mode for firmware upload via `rp2040.rebootToBootloader()`.

**Payload:** None  
**Response:** None (device enters BOOTSEL mode, RPI-RP2 drive appears)  

**Example Usage (C++ Master):**
```cpp
GunFxSerialMaster master;
master.sendBootsel();  // Slave enters BOOTSEL mode
// Wait for RPI-RP2 drive to appear
// Copy firmware.uf2 to drive
```

**Example Handler (C++ Slave):**
```cpp
serialSlave.onBootsel([]() {
    Serial.println("Entering BOOTSEL mode...");
    Serial.flush();
    delay(500);
    rp2040.rebootToBootloader();
});
```

## GunFX-Specific Packet Types (0x01-0x2F)

Commands specific to GunFX controllers:

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `GUNFX_PKT_TRIGGER_ON` | 0x01 | Master→Slave | Start firing (payload: rpm:u16) |
| `GUNFX_PKT_TRIGGER_OFF` | 0x02 | Master→Slave | Stop firing (payload: fanDelayMs:u16) |
| `GUNFX_PKT_SRV_SET` | 0x10 | Master→Slave | Set servo position |
| `GUNFX_PKT_SRV_SETTINGS` | 0x11 | Master→Slave | Configure servo profile |
| `GUNFX_PKT_SRV_RECOIL_JERK` | 0x12 | Master→Slave | Configure recoil jerk |
| `GUNFX_PKT_SMOKE_HEAT` | 0x20 | Master→Slave | Control smoke heater |

## Firmware Upload Workflow

### Manual BOOTSEL (Traditional)
1. Hold BOOTSEL button on Pico
2. Press RESET button
3. RPI-RP2 drive appears
4. Copy `firmware.uf2` to drive
5. Device auto-reboots with new firmware

### Serial-Triggered BOOTSEL (New)
1. Send `SFX_PKT_BOOTSEL` (0xF9) via serial
2. Slave enters BOOTSEL mode automatically
3. RPI-RP2 drive appears
4. Copy `firmware.uf2` to drive
5. Device auto-reboots with new firmware

**Automation Example (PowerShell):**
```powershell
# Send bootsel command
python -c "import serial; s = serial.Serial('COM10', 115200); s.write(b'bootsel\r\n'); s.close()"

# Wait for drive
while (-not (Get-Volume | Where-Object { $_.FileSystemLabel -eq 'RPI-RP2' })) { 
    Start-Sleep -Milliseconds 500 
}

# Get drive letter and copy firmware
$drive = (Get-Volume | Where-Object { $_.FileSystemLabel -eq 'RPI-RP2' }).DriveLetter
Copy-Item "firmware.uf2" "${drive}:\"
```

## Implementation

**Base Classes:**
- `SerialBus` (serial_common) - Master-side packet send/receive
- `GunFxSerialMaster` (serial_gunfx) - Master-side GunFX protocol
- `GunFxSerialSlave` (serial_gunfx) - Slave-side GunFX protocol

**Devices:**
- **HubFX Pico** - Master controller, sends commands to slaves
- **GunFX Pico** - Slave controller, receives commands from HubFX

## Changes in v0.1.0

- Added `SFX_PKT_REBOOT` (0xF8) for software reset
- Added `SFX_PKT_BOOTSEL` (0xF9) for remote firmware upload
- Added `sendReboot()` and `sendBootsel()` to `SerialBus` class
- Added `onReboot()` and `onBootsel()` callbacks to `GunFxSerialSlave`
- Updated firmware versions to v0.1.0 (development)
