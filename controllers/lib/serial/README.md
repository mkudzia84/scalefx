# Serial Library

Binary COBS serial communication library for ScaleFX controllers.

## Overview

This library provides serial communication for ScaleFX controllers:

- **HubFX (Master)** - RP2040 with USB Host, controls multiple slave devices
- **GunFX (Slave)** - RP2040 Pico, muzzle flash and recoil control
- **LightFX (Slave)** - RP2040 Pico, LED and servo control

All communication uses binary COBS-encoded packets with CRC-8 verification.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        serial.h (umbrella)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────────────────────────┐  ┌─────────────────┐  │
│  │           serial_core.h              │  │ serial_error.h  │  │
│  │  CoreProtocol (COBS, CRC, types)     │  │ Error codes     │  │
│  │  ISerialCore interface               │  └─────────────────┘  │
│  │  CoreCommandHandler (slave-side)     │                       │
│  └──────────────────────────────────────┘                       │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                    serial_bus.h (Master only)             │  │
│  │  UsbHost - PIO-USB CDC host                               │  │
│  │  SerialBus - COBS protocol over USB (implements ISerialCore) │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              serial_command_handler.h (Slave only)        │  │
│  │  ICommandHandler - Chain of Responsibility interface      │  │
│  │  CommandRouter - Routes packets to handlers               │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────┐  ┌────────────────────┐                  │
│  │  serial_gunfx.h   │  │  serial_lightfx.h  │                  │
│  │  GunFxMaster      │  │  LightFxMaster     │                  │
│  │  GunFxSlave       │  │  LightFxSlave      │                  │
│  └───────────────────┘  └────────────────────┘                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Components

| Header | Description | Used By |
|--------|-------------|---------|
| `serial.h` | Umbrella header - includes all | All |
| `serial_error.h` | Error codes and CommandResult | All |
| `serial_core.h` | Protocol (COBS, CRC), ISerialCore, CoreCommandHandler | All |
| `serial_bus.h` | UsbHost, SerialBus (USB Host) | Master only |
| `serial_command_handler.h` | ICommandHandler, CommandRouter | Slave only |
| `serial_gunfx.h` | GunFxMaster, GunFxSlave | GunFX |
| `serial_lightfx.h` | LightFxMaster, LightFxSlave | LightFX |

## Protocol

Binary COBS-encoded packets with CRC-8:

```
[type:u8][len:u8][payload:0-64 bytes][crc8:u8]
```

Packet Type Ranges:
- `0x01-0x2F` - GunFX commands (trigger, servo, smoke)
- `0x40-0x5F` - LightFX commands (LED, servo, power)
- `0xF0-0xFF` - Universal system commands (INIT, ACK, NACK, etc.)

See [PROTOCOL.md](PROTOCOL.md) for detailed protocol documentation.

## Usage

### Master (HubFX)

```cpp
#include <serial.h>

UsbHost usbHost;
GunFxMaster gunfx;

void setup() {
    usbHost.begin();
    
    gunfx.begin(&usbHost, 0);
    gunfx.onReady([](const char* name) {
        Serial.printf("Connected: %s\n", name);
    });
    gunfx.onError([](uint8_t code, const char* msg) {
        Serial.printf("Error %02X: %s\n", code, msg);
    });
}

void loop() {
    usbHost.process();
    gunfx.process();
    
    // Send commands
    gunfx.triggerOn(600);                    // Fire at 600 RPM
    gunfx.setServoPosition(1, 1500);         // Center servo
    gunfx.setSmokeHeater(true);              // Enable smoke
}
```

### Slave (GunFX Pico)

```cpp
#include <serial_gunfx.h>

CoreCommandHandler coreHandler;
GunFxSlave gunfxSlave;
CommandRouter router;

void setup() {
    Serial.begin(115200);
    
    // Core system commands
    coreHandler.begin(&Serial);
    coreHandler.setBoardInfo("GunFX", "1.0.0", "RP2040", 125, 200000);
    coreHandler.onReboot([]() { rp2040.reboot(); });
    coreHandler.onBootsel([]() { rp2040.rebootToBootloader(); });
    
    // GunFX commands
    gunfxSlave.begin(&Serial);
    gunfxSlave.onTriggerOn([](uint16_t rpm) -> uint8_t {
        startFiring(rpm);
        return GunFxError::OK;
    });
    gunfxSlave.onServoSet([](uint8_t id, uint16_t us) -> uint8_t {
        return setServoPosition(id, us);
    });
    
    // Command routing
    router.begin(&Serial, [](uint8_t err, uint8_t type) {
        gunfxSlave.sendNack(err);
    });
    router.addHandler(&coreHandler);  // System commands first
    router.addHandler(&gunfxSlave);   // Then GunFX commands
}

void loop() {
    router.process();
}
```

## Build Configuration

### GUNFX_SLAVE Macro

Define `GUNFX_SLAVE` to exclude USB Host code from slave builds:

```ini
# platformio.ini
[env:gunfx]
build_flags = -DGUNFX_SLAVE
```

## Error Handling

Commands return `CommandResult` with success/error status:

```cpp
CommandResult result = gunfx.triggerOn(600);
if (!result.success) {
    Serial.printf("Error %02X: %s\n", result.errorCode, result.message);
}
```

Slave callbacks return error codes:

```cpp
gunfxSlave.onServoSet([](uint8_t id, uint16_t us) -> uint8_t {
    if (id < 1 || id > 3) return GunFxError::SERVO_INVALID_ID;
    if (us < 500 || us > 2500) return GunFxError::SERVO_PULSE_RANGE;
    setServo(id, us);
    return GunFxError::OK;
});
