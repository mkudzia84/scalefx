# Serial GunFX Library

Master/Slave communication classes for GunFX functionality.

## Overview

This library provides specialized subclasses of `SerialBus` from `serial_common` for GunFX-specific communication:

- **GunFxSerialMaster** - For HubFX Pico (sends commands, receives status)
- **GunFxSerialSlave** - For GunFX Pico (receives commands, sends status)

These classes abstract the packet format details and provide a clean, type-safe API.

## Usage

### Master Side (HubFX Pico)

```cpp
#include <serial_gunfx.h>

UsbHost usbHost;
GunFxSerialMaster gunSerial;

void setup() {
    usbHost.begin();
    usbHost.startTask();
    
    // Initialize master serial
    gunSerial.begin(&usbHost, 0);  // Device index 0
    
    // Set up callbacks
    gunSerial.onReady([](const char* name) {
        Serial.printf("GunFX ready: %s\n", name);
    });
    
    gunSerial.onStatus([](const GunFxStatus& status) {
        Serial.printf("Firing: %d, RPM: %d\n", status.firing, status.rateOfFireRpm);
    });
    
    // Send init command
    gunSerial.sendInit();
}

void loop() {
    gunSerial.process();
    gunSerial.processKeepalive();
    
    // Fire at 600 RPM
    gunSerial.triggerOn(600);
    
    // Set servo position
    gunSerial.setServoPosition(1, 1500);
    
    // Configure servo motion profile
    GunFxServoConfig config;
    config.servoId = 1;
    config.minUs = 1000;
    config.maxUs = 2000;
    config.maxSpeedUsPerSec = 500;
    gunSerial.setServoConfig(config);
    
    // Control smoke heater
    gunSerial.setSmokeHeater(true);
    
    // Stop firing
    gunSerial.triggerOff(3000);  // 3 second fan delay
}
```

### Slave Side (GunFX Pico)

```cpp
#include <serial_gunfx.h>

GunFxSerialSlave gunSerial;

void setup() {
    Serial1.begin(115200);
    
    // Initialize slave serial
    gunSerial.begin(&Serial1, "GunFX-Turret");
    
    // Set up command callbacks
    gunSerial.onInit([]() {
        Serial.println("Master sent INIT");
        // Reset state, prepare for operation
    });
    
    gunSerial.onTriggerOn([](uint16_t rpm) {
        Serial.printf("Fire at %d RPM\n", rpm);
        startFiring(rpm);
    });
    
    gunSerial.onTriggerOff([](uint16_t fanDelayMs) {
        Serial.printf("Cease fire, fan delay %d ms\n", fanDelayMs);
        stopFiring(fanDelayMs);
    });
    
    gunSerial.onServoSet([](uint8_t servoId, uint16_t pulseUs) {
        setServoPosition(servoId, pulseUs);
    });
    
    gunSerial.onServoSettings([](const GunFxServoConfig& config) {
        configureServo(config);
    });
    
    gunSerial.onSmokeHeat([](bool on) {
        setSmokeHeater(on);
    });
    
    gunSerial.onShutdown([]() {
        // Safe shutdown
        stopFiring(0);
        setSmokeHeater(false);
    });
}

void loop() {
    // Process incoming commands
    gunSerial.process();
    
    // Send periodic status
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 100) {
        GunFxStatus status;
        status.firing = isFiring();
        status.servoUs[0] = getServoPosition(1);
        gunSerial.sendStatus(status);
        lastStatus = millis();
    }
}
```

## API Reference

### GunFxSerialMaster

**Commands:**
- `triggerOn(rpm)` - Start firing at specified rounds per minute
- `triggerOff(fanDelayMs)` - Stop firing, optionally delay fan shutoff
- `setServoPosition(id, pulseUs)` - Set servo position
- `setServoConfig(config)` - Configure servo motion profile
- `setRecoilJerk(id, jerkUs, varianceUs)` - Configure recoil jerk effect
- `setSmokeHeater(on)` - Control smoke heater

**Callbacks:**
- `onStatus(callback)` - Receive status updates from slave
- `onReady(callback)` - Notification when slave is ready
- `onError(callback)` - Receive error notifications

### GunFxSerialSlave

**Transmission:**
- `sendStatus(status)` - Send current status to master
- `sendInitReady()` - Respond to master INIT
- `sendError(code, message)` - Report error to master
- `sendAck()` / `sendNack(reason)` - Acknowledge commands

**Callbacks:**
- `onInit(callback)` - Master initialization request
- `onShutdown(callback)` - Master shutdown request
- `onTriggerOn(callback)` - Start firing command
- `onTriggerOff(callback)` - Stop firing command
- `onServoSet(callback)` - Servo position command
- `onServoSettings(callback)` - Servo configuration command
- `onSmokeHeat(callback)` - Smoke heater control

## Dependencies

- `serial_common` - Base serial communication library

## Packet Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| TRIGGER_ON | 0x01 | M→S | Start firing |
| TRIGGER_OFF | 0x02 | M→S | Stop firing |
| SRV_SET | 0x10 | M→S | Set servo position |
| SRV_SETTINGS | 0x11 | M→S | Configure servo |
| SRV_RECOIL_JERK | 0x12 | M→S | Configure recoil jerk |
| SMOKE_HEAT | 0x20 | M→S | Control heater |
| STATUS | 0xF4 | S→M | Status update |
| INIT_READY | 0xF3 | S→M | Slave ready response |
| ERROR | 0xF5 | S→M | Error report |
