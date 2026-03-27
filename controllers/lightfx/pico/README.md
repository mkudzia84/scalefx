# LightFX Controller - Raspberry Pi Pico

Lighting effects controller for scale models - manages 8 LED channels with sequence animations and 3 servos.

**Version:** 0.6.0  
**Protocol:** Binary COBS with CRC-8  
**Baud Rate:** 1000000 (1Mbps)

---

## Hardware

- **MCU**: Raspberry Pi Pico (RP2040)
- **Core**: earlephilhower/arduino-pico

### Pin Assignments

#### LED Output Channels (PWM)

| Channel | GPIO | Description |
|---------|------|-------------|
| CH1     | 0    | LED Channel 1 |
| CH2     | 1    | LED Channel 2 |
| CH3     | 2    | LED Channel 3 |
| CH4     | 3    | LED Channel 4 |
| CH5     | 4    | LED Channel 5 |
| CH6     | 5    | LED Channel 6 |
| CH7     | 6    | LED Channel 7 |
| CH8     | 7    | LED Channel 8 |

#### Status LEDs

| LED    | GPIO | Function |
|--------|------|----------|
| Blue   | 24   | Connection status - slow blink when disconnected |
| Yellow | 25   | Error/warning indicator |

#### Battery Sensing

| Pin  | GPIO | Function |
|------|------|----------|
| VSYS | 29   | Battery voltage ADC (÷5.1 resistor divider) |

#### Servos

| Servo   | GPIO | Description |
|---------|------|-------------|
| Servo 1 | 8    | General purpose servo |
| Servo 2 | 9    | General purpose servo |
| Servo 3 | 10   | General purpose servo |

---

## Protocol

### Packet Format

Binary COBS-encoded packets terminated by 0x00 delimiter:

```
[type:u8][len:u16LE][payload:0-512 bytes][crc8:u8]
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
| 0x40 | LED_SET | ch:u8, brightness:u8 | Set LED channel brightness (0-100%) |
| 0x41 | LED_OFF | ch:u8 (0=all) | Turn off LED(s) |
| 0x4A | LED_MASTER_BRIGHTNESS | pct:u8 (0-100) | Set master brightness for all LEDs |

### LED Sequences

Each LED channel has its own sequence that can hold up to 24 events.

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x42 | LED_SEQ_CLEAR | ch:u8 | Clear sequence for channel |
| 0x43 | LED_SEQ_ADD | ch:u8, event... | Add event to sequence |
| 0x44 | LED_SEQ_START | ch:u8 (0=all) | Start sequence playback |
| 0x45 | LED_SEQ_STOP | ch:u8 (0=all) | Stop sequence playback |
| 0x46 | LED_SEQ_RESTART | ch:u8 | Restart sequence from beginning |
| 0x47 | LED_SEQ_STATUS | ch:u8 | Query sequence status → LED_SEQ_STATUS_RESP |
| 0x48 | LED_STATUS | (none) | Query all LED channels → LED_STATUS_RESP |
| 0x49 | LED_SEQ_QUEUE | ch:u8 | Query sequence event queue → LED_SEQ_QUEUE_RESP |

#### Response Packets

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x5A | LED_STATUS_RESP | [ch, brightness, playing, count]×N | Status of all channels |
| 0x5B | LED_SEQ_STATUS_RESP | ch:u8, playing:u8, count:u8, index:u8, loops:u32le | Sequence status |
| 0x5D | LED_SEQ_QUEUE_RESP | ch:u8, count:u8, index:u8, playing:u8, [events...] | Event queue listing |

**LED_SEQ_QUEUE_RESP Event Format:** Each event is 4 bytes: `type:u8, duration:u16le, param1:u8`

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

### Landing Light Control

Coordinates a retract servo with a landing light LED channel. Up to 3 landing light slots.

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x52 | LANDING_LIGHT_BIND | slot:u8, servo_id:u8, led_ch:u8, deploy_us:u16le, retract_us:u16le, brightness:u8 | Bind servo + LED as landing light pair |
| 0x53 | LANDING_LIGHT_UNBIND | slot:u8 (0=all) | Unbind landing light slot |
| 0x54 | LANDING_LIGHT_DEPLOY | slot:u8 (0=all) | Deploy gear, light on when arrived |
| 0x55 | LANDING_LIGHT_RETRACT | slot:u8 (0=all) | Light off immediately, then retract gear |
| 0x56 | LANDING_LIGHT_STATUS | slot:u8, phase:u8, finished:u8 | Deploy/retract progress (server→client, echoes request tag) |

#### LANDING_LIGHT_STATUS (Deploy/Retract Progress)

Emitted by the server during landing light deploy/retract to report progress. Uses the tag from the original LANDING_LIGHT_DEPLOY or LANDING_LIGHT_RETRACT request.

**Wire format (3 bytes, server→client, echoes request tag):**
```
[slot:u8][phase:u8][finished:u8]
```

| Field | Type | Description |
|-------|------|-------------|
| slot | u8 | Landing light slot (1-3) |
| phase | u8 | LandingLightPhase enum value |
| finished | u8 | 1 if transition complete, 0 otherwise |

**LandingLightPhase enum:**
| Value | Name | Description |
|-------|------|-------------|
| 0 | RETRACTED | Servo retracted, light off |
| 1 | DEPLOYING | Servo moving to deploy position, light off |
| 2 | DEPLOYED | Servo at deploy position, light on |
| 3 | RETRACTING | Light off, servo moving to retract position |

**Emission points:**
1. Deploy command → phase=DEPLOYING
2. Servo reaches deploy target → phase=DEPLOYED, finished=1
3. Retract command → phase=RETRACTING
4. Servo reaches retract target → phase=RETRACTED, finished=1

**Tag correlation:** The client resolves the pending tag when `finished=1`, providing the equivalent of a deferred ACK for the long-running operation.

**State Machine:**
```
UNCONFIGURED → (bind)    → RETRACTED
RETRACTED    → (deploy)  → DEPLOYING    [servo moving, light OFF]
DEPLOYING    → (arrived) → DEPLOYED     [light ON]
DEPLOYED     → (retract) → RETRACTING   [light OFF immediately, servo moving]
RETRACTING   → (arrived) → RETRACTED
```

**Sequencing:**
- **Deploy:** Servo moves to deploy position → light turns ON when servo reaches target
- **Retract:** Light turns OFF immediately → servo moves to retract position

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
1. LedFadeIn(500, 100)    # Fade in over 500ms
2. LedOn(1000, 100)       # Hold at full for 1s
3. LedFlash(50, 2000, 100, 50)  # Strobe for 2s
4. LedFadeOut(500, 100)   # Fade out over 500ms
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
| 0x55 | INVALID_SLOT | Invalid landing light slot (1-3) |

---

## Architecture

Uses `PicoServer` component for common server boilerplate (serial init, device naming, indicator LEDs, core protocol, connection management). Module-specific logic is handled by `LightFxServer`.

```
┌─────────────────────────────────────────────────────────────┐
│                      PicoServer                              │
│  Serial init, device name, indicators, connection timeout   │
├─────────────────────────────────────────────────────────────┤
│                     CommandRouter                            │
│  Routes COBS packets to handlers in priority order          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐  │
│  │  CoreCommandServer  │  │     LightFxServer            │  │
│  │  (Priority 1)       │  │      (Priority 2)           │  │
│  ├─────────────────────┤  ├─────────────────────────────┤  │
│  │ INIT, SHUTDOWN      │  │ LED_SET, LED_OFF            │  │
│  │ REBOOT, BOOTSEL     │  │ LED_SEQ_* commands          │  │
│  │ KEEPALIVE, STATUS   │  │ SERVO_SET, SERVO_SETTINGS   │  │
│  └─────────────────────┘  │ LANDING_LIGHT_* commands    │  │
│                            └─────────────────────────────┘  │
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

- **v0.6.0** - Landing light progress reporting
  - New: LANDING_LIGHT_STATUS (0x56) emitted during deploy/retract sequences
  - Reports phase transitions (RETRACTED → DEPLOYING → DEPLOYED, and reverse)
  - Uses tag correlation from original DEPLOY/RETRACT request
  - Client auto-resolves pending tag when finished=1

- **v0.5.0** - Brightness 0-100 scale
  - Changed brightness from 0-255 PWM to 0-100% human-readable scale
  - Applies to: LED_SET, LED_SEQ_ADD events, LANDING_LIGHT_BIND brightness
  - LedControl component converts 0-100 → 0-255 PWM at hardware output
  - Master brightness (0-100%) combined with channel brightness at output
  - All LED events (LedOn, LedFlashing, LedFadeIn, etc.) use 0-100 scale

- **v0.4.0** - Master brightness control
  - New `LedControl::setMasterBrightness_pct()` in components library
  - Global 0-100% brightness scaling applied at PWM output
  - New command: LED_MASTER_BRIGHTNESS (0x4A)
  - Reset to 100% on SHUTDOWN/INIT
  - STATUS response extended to 19 bytes (added master brightness)

- **v0.3.0** - Landing light sequencer
  - New LandingLight class: binds retract servo with LED channel
  - Light activates when deployment completes (servo at target)
  - Light deactivates before retraction starts
  - Up to 3 landing light slots
  - New commands: LANDING_LIGHT_BIND/UNBIND/DEPLOY/RETRACT (0x52-0x55)
  - STATUS response extended to 18 bytes (added landing light states)

- **v0.2.0** - Binary-only protocol
  - Removed text protocol support
  - Using new serial library (CoreCommandServer + LightFxServer)
  - COBS framing with CRC-8 validation
  - Removed INA226 power monitoring (board redesign)

- **v0.1.0** - Initial release
  - 8-channel LED output with PWM
  - LED event sequences
  - 3 servo outputs with motion profiling
  - Status LEDs
