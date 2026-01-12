# Serial Library

Combined serial communication library for ScaleFX controllers (HubFX, GunFX).

## Overview

This library provides all serial communication functionality for ScaleFX:

- **Protocol** - COBS framing, CRC-8, packet types
- **SerialInitHandler** - Protocol negotiation (always text-based)
- **USB Host** - PIO-USB host for CDC devices (RP2040)
- **SerialBus** - Binary protocol master implementation
- **SerialBusText** - Human-readable text protocol for testing
- **IGunFxMaster/IGunFxSlave** - Protocol-agnostic interfaces
- **GunFxSerialMaster/Slave** - Binary protocol implementations
- **GunFxSerialMasterText/SlaveText** - Text protocol implementations

## Protocol-Agnostic Design

Both binary and text implementations share the same interface, allowing
controller code to work with either protocol transparently:

```cpp
// Controller code works with either protocol
IGunFxSlave* slave;  // Points to binary or text implementation

// Same callbacks work for both protocols
slave->onTriggerOn([](uint16_t rpm) { startFiring(rpm); });
slave->onServoSet([](uint8_t id, uint16_t us) { setServo(id, us); });

// Same methods work for both protocols
slave->sendStatus(status);
slave->process();
```

## Components

### Headers

| Header | Description |
|--------|-------------|
| `serial.h` | Umbrella header - includes all components |
| `serial_protocol.h` | Protocol constants, packet types, helpers |
| `serial_init.h` | Protocol negotiation handler |
| `serial_gunfx_types.h` | Shared types, callbacks, and interfaces |
| `serial_gunfx.h` | Binary GunFX master/slave |
| `serial_gunfx_text.h` | Text GunFX master/slave |

### Interfaces

| Interface | Description |
|-----------|-------------|
| `IGunFxMaster` | Abstract interface for GunFX master implementations |
| `IGunFxSlave` | Abstract interface for GunFX slave implementations |

### Classes

| Class | Implements | Protocol |
|-------|------------|----------|
| `GunFxSerialMaster` | `IGunFxMaster` | Binary COBS |
| `GunFxSerialMasterText` | `IGunFxMaster` | Text |
| `GunFxSerialSlave` | `IGunFxSlave` | Binary COBS |
| `GunFxSerialSlaveText` | `IGunFxSlave` | Text |

### Shared Types

All callback types are shared between binary and text implementations:

```cpp
// Slave callbacks (from master)
using GunFxTriggerOnCallback = std::function<void(uint16_t rpm)>;
using GunFxTriggerOffCallback = std::function<void(uint16_t fanDelayMs)>;
using GunFxServoSetCallback = std::function<void(uint8_t servoId, uint16_t pulseUs)>;
using GunFxServoSettingsCallback = std::function<void(const GunFxServoConfig& config)>;
using GunFxSmokeHeatCallback = std::function<void(bool on)>;

// Master callbacks (from slave)
using GunFxStatusCallback = std::function<void(const GunFxStatus& status)>;
using GunFxReadyCallback = std::function<void(const char* moduleName)>;
using GunFxErrorCallback = std::function<void(uint8_t errorCode, const char* message)>;
```

## Usage

### Quick Start

```cpp
#include <serial.h>  // Include everything
```

### Protocol-Agnostic Slave

Use `IGunFxSlave*` to make controller code protocol-independent:

```cpp
#include <serial.h>

SerialInitHandler initHandler;
GunFxSerialSlave binarySlave;
GunFxSerialSlaveText textSlave;

// Protocol-agnostic pointer
IGunFxSlave* activeSlave = nullptr;

void setup() {
    Serial1.begin(115200);
    
    initHandler.begin(&Serial1, "GunFX-1234");
    initHandler.setBoardInfo("0.1.0", "RP2040", 120, rp2040.getFreeHeap());
    
    initHandler.onInitComplete([](ProtocolMode mode) {
        if (mode == ProtocolMode::Binary) {
            binarySlave.begin(&Serial1, "GunFX-1234");
            activeSlave = &binarySlave;
        } else {
            textSlave.begin(&Serial1, "GunFX-1234");
            activeSlave = &textSlave;
        }
        
        // Same callbacks for either protocol!
        activeSlave->onTriggerOn([](uint16_t rpm) {
            startFiring(rpm);
        });
        
        activeSlave->onTriggerOff([](uint16_t fanDelayMs) {
            stopFiring(fanDelayMs);
        });
        
        activeSlave->onServoSet([](uint8_t id, uint16_t pulseUs) {
            setServoPosition(id, pulseUs);
        });
        
        activeSlave->onSmokeHeat([](bool on) {
            setSmokeHeater(on);
        });
    });
    
    initHandler.onReboot([]() { rp2040.reboot(); });
    initHandler.onBootsel([]() { rp2040.rebootToBootloader(); });
}

void loop() {
    // InitHandler watches for INIT (handles reconnection)
    if (initHandler.process()) {
        return;
    }
    
    // Process with active protocol
    if (activeSlave) {
        activeSlave->process();
        
        // Send status (works with either protocol)
        static unsigned long lastStatus = 0;
        if (millis() - lastStatus > 100) {
            GunFxStatus status;
            status.firing = isFiring();
            status.servoUs[0] = getServoPosition(0);
            activeSlave->sendStatus(status);
            lastStatus = millis();
        }
    }
}
```

### Protocol-Agnostic Master

```cpp
#include <serial.h>

// Protocol-agnostic pointer
IGunFxMaster* activeMaster = nullptr;

GunFxSerialMaster binaryMaster;
GunFxSerialMasterText textMaster;

void setup() {
    // Choose protocol based on configuration
    if (useBinaryProtocol) {
        binaryMaster.begin(&usbHost, 0);
        activeMaster = &binaryMaster;
    } else {
        textMaster.begin(&Serial1);
        activeMaster = &textMaster;
    }
    
    // Same callbacks for either protocol!
    activeMaster->onReady([](const char* name) {
        Serial.printf("Connected to: %s\n", name);
    });
    
    activeMaster->onStatus([](const GunFxStatus& status) {
        Serial.printf("Firing: %d\n", status.firing);
    });
}

void loop() {
    activeMaster->process();
    
    // Commands work the same for either protocol
    activeMaster->triggerOn(600);
    activeMaster->setServoPosition(1, 1500);
    activeMaster->setSmokeHeater(true);
}
```

## Protocol Negotiation

The INIT handshake is **always text-based**, regardless of the final protocol mode:

```
Master                              Slave
  |  INIT protocol=binary\n           |
  |---------------------------------->|
  |                                   | (switches to binary)
  |  INIT_READY name=... version=...\n|
  |<----------------------------------|
  |  [Binary COBS packets]            |
  |<=================================>|
```

## Build Configuration

### GUNFX_SLAVE Macro

Define `GUNFX_SLAVE` to exclude USB HOST code from slave builds:

```ini
# platformio.ini
[env:gunfx]
build_flags = -DGUNFX_SLAVE
```

## Protocol Details

See [PROTOCOL.md](PROTOCOL.md) for binary protocol documentation.  
See [docs/TEXT_COMMANDS.md](docs/TEXT_COMMANDS.md) for text protocol command reference.

## API Reference

### IGunFxSlave Interface

```cpp
class IGunFxSlave {
public:
    // Lifecycle
    virtual bool begin(Stream* serial, const char* moduleName = "GunFX") = 0;
    virtual void setBoardInfo(const char* version, const char* platform,
                              uint32_t cpuMHz, uint32_t ramBytes) = 0;
    virtual void end() = 0;
    virtual int process() = 0;

    // Status transmission
    virtual int sendStatus(const GunFxStatus& status) = 0;
    virtual int sendError(uint8_t code, const char* msg = nullptr) = 0;
    virtual int sendAck() = 0;
    virtual int sendNack(const char* reason = nullptr) = 0;

    // Callbacks
    virtual void onTriggerOn(GunFxTriggerOnCallback cb) = 0;
    virtual void onTriggerOff(GunFxTriggerOffCallback cb) = 0;
    virtual void onServoSet(GunFxServoSetCallback cb) = 0;
    virtual void onServoSettings(GunFxServoSettingsCallback cb) = 0;
    virtual void onSmokeHeat(GunFxSmokeHeatCallback cb) = 0;

    // State
    virtual bool isInitialized() const = 0;
    virtual bool isMasterConnected() const = 0;
    virtual void setConnectionTimeout(unsigned long ms) = 0;
};
```

### IGunFxMaster Interface

```cpp
class IGunFxMaster {
public:
    // Lifecycle
    virtual void end() = 0;
    virtual int process() = 0;

    // Commands
    virtual int triggerOn(uint16_t rpm) = 0;
    virtual int triggerOff(uint16_t fanDelayMs = 0) = 0;
    virtual int setServoPosition(uint8_t id, uint16_t pulseUs) = 0;
    virtual int setServoConfig(const GunFxServoConfig& config) = 0;
    virtual int setRecoilJerk(uint8_t id, uint16_t jerkUs, uint16_t varianceUs = 0) = 0;
    virtual int setSmokeHeater(bool on) = 0;

    // Callbacks
    virtual void onStatus(GunFxStatusCallback cb) = 0;
    virtual void onReady(GunFxReadyCallback cb) = 0;
    virtual void onError(GunFxErrorCallback cb) = 0;

    // State
    virtual const GunFxStatus& lastStatus() const = 0;
    virtual bool isSlaveReady() const = 0;
    virtual const char* slaveName() const = 0;
    virtual const GunFxBoardInfo& boardInfo() const = 0;
    virtual bool isConnected() const = 0;
};
```
