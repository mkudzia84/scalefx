# LightFX Controller - Raspberry Pi Pico

Lighting effects controller for scale models - manages 8 LED channels with sequence animations, 3 servos, and power monitoring.

**Version:** 0.2.0  
**Protocol:** Binary COBS with CRC-8  
**Baud Rate:** 115200

---

## Hardware

- **MCU**: Raspberry Pi Pico (RP2040)
- **Core**: earlephilhower/arduino-pico

### Pin Assignments

#### LED Output Channels (PWM)

| Channel | GPIO | Description |
|---------|------|-------------|
| CH1     | 28   | LED Channel 1 |
| CH2     | 27   | LED Channel 2 |
| CH3     | 26   | LED Channel 3 |
| CH4     | 25   | LED Channel 4 |
| CH5     | 24   | LED Channel 5 |
| CH6     | 23   | LED Channel 6 |
| CH7     | 22   | LED Channel 7 |
| CH8     | 21   | LED Channel 8 |

#### Status LEDs

| LED    | GPIO | Function |
|--------|------|----------|
| Blue   | 13   | Connection status - slow blink when disconnected |
| Yellow | 14   | Activity indicator |

#### Servos

| Servo   | GPIO | Description |
|---------|------|-------------|
| Servo 1 | 1    | General purpose servo |
| Servo 2 | 2    | General purpose servo |
| Servo 3 | 3    | General purpose servo |

#### I2C (INA226 Power Monitor)

| Signal | GPIO |
|--------|------|
| SDA    | 4    |
| SCL    | 5    |

---

## Protocol

### Packet Format

Binary COBS-encoded packets terminated by 0x00 delimiter:

```
[type:u8][len:u8][payload:0-64 bytes][crc8:u8]
```

CRC-8 polynomial 0x07 computed over type + len + payload.

### Packet Type Ranges

| Range | Module | Description |
|-------|--------|-------------|
| 0x40-0x5F | LightFX | Controller-specific commands |
| 0xF0-0xFF | Core | Universal system commands |

---

## System Commands (0xF0-0xFF)

| Type | Name | Payload | Response | Description |
|------|------|---------|----------|-------------|
| 0xF0 | INIT | (none) | INIT_READY | Initialize connection |
| 0xF1 | SHUTDOWN | (none) | ACK | Safe shutdown, outputs off |
| 0xF2 | KEEPALIVE | (none) | ACK | Reset watchdog timer |
| 0xF3 | INIT_READY | (response) | — | Device info response |
| 0xF5 | STATUS_REQ | (none) | STATUS | Request status |
| 0xF6 | ACK | (none) | — | Command success |
| 0xF7 | NACK | code:u8, reason:str | — | Command failure |
| 0xF8 | REBOOT | (none) | — | Reboot (fire-and-forget) |
| 0xF9 | BOOTSEL | (none) | — | Enter bootloader |

---

## LightFX Commands (0x40-0x5F)

### LED Direct Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x40 | LED_SET | ch:u8, brightness:u8 | Set LED channel brightness (0-255) |
| 0x41 | LED_OFF | ch:u8 (0=all) | Turn off LED(s) |

### LED Sequences

Each LED channel has its own sequence that can hold up to 24 events.

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x42 | LED_SEQ_CLEAR | ch:u8 | Clear sequence for channel |
| 0x43 | LED_SEQ_ADD | ch:u8, event... | Add event to sequence |
| 0x44 | LED_SEQ_START | ch:u8 (0=all) | Start sequence playback |
| 0x45 | LED_SEQ_STOP | ch:u8 (0=all) | Stop sequence playback |

#### LED_SEQ_ADD Event Types

| Type | Name | Parameters | Description |
|------|------|------------|-------------|
| 0x00 | ON | duration:u16le, brightness:u8 | Constant on |
| 0x01 | OFF | duration:u16le | Constant off |
| 0x02 | FLASH | interval:u16le, duration:u16le, brightness:u8, duty:u8 | On/off flashing |
| 0x03 | FADE_IN | duration:u16le, brightness:u8 | Fade from off to on |
| 0x04 | FADE_OUT | duration:u16le, brightness:u8 | Fade from on to off |
| 0x05 | FADING | cycle:u16le, duration:u16le, min:u8, max:u8 | Sinusoidal breathing |

### Servo Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x50 | SERVO_SET | id:u8, pulse:u16le | Set servo position (500-2500µs) |
| 0x51 | SERVO_SETTINGS | id:u8, min:u16le, max:u16le, speed:u16le, accel:u16le, decel:u16le | Configure servo |

### Power Monitoring

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x58 | POWER_STATUS | (none) | Request power readings |

**POWER_STATUS Response:**

| Field | Type | Description |
|-------|------|-------------|
| voltage_mv | u16le | Bus voltage in millivolts |
| current_ma | i16le | Current in milliamps (signed) |
| power_mw | u16le | Power in milliwatts |
| available | u8 | 1 if INA226 detected, 0 otherwise |

---

## INA226 Power Monitor

The INA226 is a high-precision current/power monitor from Texas Instruments.

### I2C Address Configuration

Default address: 0x40 (A0=GND, A1=GND)

| A1    | A0    | Address |
|-------|-------|---------|
| GND   | GND   | 0x40 |
| GND   | VS+   | 0x41 |
| VS+   | GND   | 0x44 |
| VS+   | VS+   | 0x45 |

### Register Map

| Register | Address | Description |
|----------|---------|-------------|
| Configuration | 0x00 | Averaging, conversion time, mode |
| Shunt Voltage | 0x01 | Shunt voltage measurement |
| Bus Voltage | 0x02 | Bus voltage measurement |
| Power | 0x03 | Calculated power |
| Current | 0x04 | Calculated current |
| Calibration | 0x05 | Sets current LSB |
| Manufacturer ID | 0xFE | Returns 0x5449 ("TI") |

---

## LED Event Sequences

Each LED channel has its own `LedEventSeq` that can play a sequence of events:

| Event | Parameters | Description |
|-------|------------|-------------|
| LedOn | duration_ms, brightness | Constant on for duration |
| LedOff | duration_ms | Constant off for duration |
| LedFlash | interval_ms, duration_ms, brightness, duty% | On/off flashing |
| LedFadeIn | duration_ms, target_brightness | Fade from off to target |
| LedFadeOut | duration_ms, start_brightness | Fade from start to off |
| LedFading | cycle_ms, duration_ms, min, max | Sinusoidal breathing |

**Example Sequence (strobe beacon):**
```
1. LedFadeIn(500, 255)    # Fade in over 500ms
2. LedOn(1000, 255)       # Hold at full for 1s
3. LedFlash(50, 2000, 255, 50)  # Strobe for 2s
4. LedFadeOut(500, 255)   # Fade out over 500ms
5. LedOff(1000)           # Off for 1s
# Sequence loops
```

---

## Error Codes

### Generic Errors (SerialError namespace)

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x03 | INVALID_COMMAND | Unknown command |
| 0x04 | MISSING_PARAMETER | Required parameter missing |
| 0x10 | INVALID_PARAM | Generic invalid parameter |
| 0x12 | INVALID_ID | Invalid device/channel ID |

### LightFX-Specific Errors (0x30-0x3F)

| Code | Name | Description |
|------|------|-------------|
| 0x30 | INVALID_CHANNEL | Channel number out of range (1-8) |
| 0x31 | SEQ_FULL | Sequence already has 24 events |
| 0x32 | INVALID_EVENT | Unknown event type |
| 0x33 | INVALID_PARAM | Event parameter out of range |
| 0x38 | SERVO_INVALID_ID | Servo ID out of range (1-3) |
| 0x39 | SERVO_PULSE_RANGE | Pulse width outside 500-2500µs |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     CommandRouter                            │
│  Routes COBS packets to handlers in priority order          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐  │
│  │  CoreCommandHandler │  │      LightFxSlave           │  │
│  │  (Priority 1)       │  │      (Priority 2)           │  │
│  ├─────────────────────┤  ├─────────────────────────────┤  │
│  │ INIT, SHUTDOWN      │  │ LED_SET, LED_OFF            │  │
│  │ REBOOT, BOOTSEL     │  │ LED_SEQ_* commands          │  │
│  │ KEEPALIVE, STATUS   │  │ SERVO_SET, SERVO_SETTINGS   │  │
│  └─────────────────────┘  │ POWER_STATUS                │  │
│                           └─────────────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## Build & Upload

### Using PlatformIO

```bash
cd controllers/lightfx/pico

# Build
pio run

# Build and upload
pio run -t upload

# Clean build
pio run -t clean
```

### Manual BOOTSEL Method

1. Hold BOOTSEL button on Pico while connecting USB
2. RPI-RP2 drive appears
3. Copy `.pio/build/pico/firmware.uf2` to drive
4. Device auto-reboots with new firmware

---

## Version History

- **v0.2.0** - Binary-only protocol
  - Removed text protocol support
  - Using new serial library (CoreCommandHandler + LightFxSlave)
  - COBS framing with CRC-8 validation

- **v0.1.0** - Initial release
  - 8-channel LED output with PWM
  - LED event sequences
  - 3 servo outputs with motion profiling
  - INA226 power monitoring
  - Status LEDs
