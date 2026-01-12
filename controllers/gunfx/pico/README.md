# GunFX Controller - Raspberry Pi Pico

Slave microcontroller for gun FX hardware control. Receives commands from the main HubFX board over USB serial and drives:
- Nozzle flash LED (PWM)
- Smoke heater and fan
- Turret servos (pitch, yaw, retract)
- Status LEDs (heater indicator, firing status)

## Hardware

The GunFX board connects to the main ScaleFX Hub board via a custom USB-C cable. The USB-C cable provides both power and data communication to the Pico.

### Pico Pinout

| GPIO | Function |
|------|----------|
| 1 | Servo 1 |
| 2 | Servo 2 |
| 3 | Servo 3 |
| 13 | Blue LED (status) |
| 14 | Yellow LED (heater indicator) |
| 16 | Smoke Fan Motor (PWM) |
| 17 | Smoke Heater |
| 25 | Nozzle Flash (PWM) |

### Status LED Indicators

**Yellow LED (GPIO 14):**
- **Solid ON**: Smoke heater is active
- **OFF**: Heater is off

**Blue LED (GPIO 13):**
- **OFF**: All OK (idle, normal operation)
- **Synced with nozzle flash**: Blinks at firing rate when shooting
- **Slow blink (1s on / 2s off)**: No signal from main board (watchdog timeout)

---

## Protocol Overview

GunFX supports two communication protocols, negotiated via the `INIT` command:

| Protocol | Format | Use Case |
|----------|--------|----------|
| **Binary** | COBS-encoded packets | Production use (efficient) |
| **Text** | Line-based key=value | Testing/debugging via serial terminal |

**Baud Rate:** 115200

### ACK/NACK Response Protocol

All commands return a response:
- **ACK** - Command executed successfully
- **NACK** - Command failed with error code and reason

Exceptions (fire-and-forget, no response expected):
- `REBOOT` - Device reboots immediately
- `BOOTSEL` - Device enters bootloader immediately

### Error Codes

Error codes are defined in two layers:

**Generic Errors** (all modules, `SerialError` namespace):
| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x01 | UNKNOWN | Unknown error |
| 0x02 | NOT_INITIALIZED | Module not initialized |
| 0x03 | INVALID_COMMAND | Unknown command |
| 0x04 | MISSING_PARAMETER | Required parameter missing |
| 0x05 | BUSY | Module busy, try again |
| 0x06 | NOT_SUPPORTED | Command not supported |
| 0x10 | INVALID_PARAM | Generic invalid parameter |
| 0x11 | PARAM_OUT_OF_RANGE | Parameter value out of range |
| 0x12 | INVALID_ID | Invalid device/channel ID |
| 0xF0 | INTERNAL_ERROR | Internal error |
| 0xF1 | TIMEOUT | Operation timed out |
| 0xF2 | COMM_ERROR | Communication error |

**GunFX-Specific Errors** (`GunFxError` namespace, 0x20-0x4F):
| Code | Name | Description |
|------|------|-------------|
| 0x20 | SERVO_INVALID_ID | Servo ID out of range (1-3) |
| 0x21 | SERVO_PULSE_RANGE | Pulse width outside 500-2500µs |
| 0x22 | SERVO_MIN_MAX | minUs >= maxUs |
| 0x23 | SERVO_NOT_CONFIGURED | Servo not configured |
| 0x30 | HEATER_SAFETY | Heater safety interlock |
| 0x31 | FAN_NOT_RUNNING | Fan must be running for heater |
| 0x32 | INVALID_FAN_SPEED | Invalid fan speed value |
| 0x40 | INVALID_RPM | RPM out of range (1-3000) |
| 0x41 | ALREADY_FIRING | Already firing |
| 0x42 | NOT_FIRING | Not currently firing |

---

## Binary Protocol

**Format:** COBS-encoded packets terminated by 0x00 delimiter

Packet format (before COBS encoding):
```
[type:u8][len:u8][payload:len bytes][crc8:u8]
```
CRC-8 polynomial 0x07 computed over type + len + payload.

### System Commands (0xF0-0xFF)

| Type | Name | Payload | Response | Description |
|------|------|---------|----------|-------------|
| 0xF0 | INIT | protocol:u8 (0=text, 1=binary) | INIT_READY | Initialize connection |
| 0xF1 | SHUTDOWN | (none) | ACK | Safe shutdown, outputs off |
| 0xF2 | KEEPALIVE | (none) | ACK | Reset watchdog timer |
| 0xF6 | ACK | (none) | — | Command success response |
| 0xF7 | NACK | code:u8, reason:string | — | Command failure response |
| 0xF8 | REBOOT | (none) | — | Reboot device (fire-and-forget) |
| 0xF9 | BOOTSEL | (none) | — | Enter bootloader (fire-and-forget) |

### GunFX Commands (0x01-0x2F)

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x01 | TRIGGER_ON | rpm:u16le | Start firing at specified RPM (1-3000) |
| 0x02 | TRIGGER_OFF | fan_delay_ms:u16le | Stop firing; delay before fan turns off |
| 0x10 | SRV_SET | servo_id:u8, pulse_us:u16le | Set servo position (500-2500µs) |
| 0x11 | SRV_SETTINGS | servo_id:u8, min:u16le, max:u16le, speed:u16le, accel:u16le, decel:u16le | Configure servo limits and motion profile |
| 0x12 | SRV_RECOIL_JERK | servo_id:u8, jerk_us:u16le, variance_us:u16le | Configure recoil jerk per shot |
| 0x20 | SMOKE_HEAT | on:u8 (0=off, 1=on) | Control smoke heater |
| 0x21 | SMOKE_SETTINGS | pulsing:u8, speed:u8, pulse_high:u8, pulse_low:u8, pulse_ms:u16le, spindown_ms:u16le | Configure smoke fan behavior |

### Telemetry (Pico → Hub)

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0xF3 | INIT_READY | name:str, version:str, platform:str, cpuMHz:u32le, ramBytes:u32le | Device info response |
| 0xF4 | STATUS | flags:u8, fan_off_ms:u16le, srv1-3:u16le each, rpm:u16le | Periodic status (1Hz) |
| 0xF5 | ERROR | code:u8, msg:string | Asynchronous error notification |

**Status flags (bit field):**
| Bit | Name | Description |
|-----|------|-------------|
| 0 | firing | Currently firing |
| 1 | flash_active | Flash LED on |
| 2 | flash_fading | Flash fading out |
| 3 | heater_on | Smoke heater active |
| 4 | fan_on | Smoke fan running |
| 5 | fan_spindown | Fan spinning down |

---

## Text Protocol

**Format:** Line-based commands for testing via serial terminal  
**Command Format:** `COMMAND_NAME key=value key2=value2\n`

### System Commands

```
INIT protocol=text
SHUTDOWN
REBOOT
BOOTSEL
KEEPALIVE
```

### GunFX Commands

```
TRIGGER_ON rpm=600
TRIGGER_OFF fanDelayMs=3000

SERVO_SET id=1 pulseUs=1500
SERVO_CONFIG id=1 minUs=1000 maxUs=2000 speed=4000 accel=8000 decel=8000
SERVO_RECOIL_JERK id=1 jerkUs=50 varianceUs=10

SMOKE_HEAT on=1
SMOKE_SETTINGS pulsing=1 speed=255 pulseHigh=255 pulseLow=80 pulseMs=50 spindownMs=5000
```

### Responses

```
INIT_READY name=GunFX-1A2B version=0.2.0 platform=RP2040 cpuMHz=133 ramBytes=200000
STATUS firing=1 flash=1 heater=1 fan=1 srv1=1500 srv2=1500 srv3=1500 rpm=600
ACK
NACK code=32 reason=Invalid servo ID (use 1-3)
ERROR code=1 msg=Connection timeout
```

See [docs/COMMANDS.md](docs/COMMANDS.md) for complete command reference.

---

## Behavior Details

### Servo Motion

`SRV_SET` commands move smoothly using trapezoidal velocity profile:
- Accelerate until max_speed
- Cruise at max_speed  
- Decelerate to stop at target
- Automatic braking/turnaround when reversing direction

**Defaults:** max_speed=4000 µs/s, accel=8000 µs/s², decel=8000 µs/s²

### Recoil Jerk Effect

`SRV_RECOIL_JERK` configures simulated recoil kick effect:

- On each shot, a random jerk offset is applied to servo position
- Jerk direction is randomly ± (positive or negative)
- Jerk magnitude = base `jerk_us` + random(0 to `variance_us`)
- Jerk clears after flash fade completes

**Example:** `jerk_us=50, variance_us=25` → each shot applies ±50µs to ±75µs offset

### Smoke Fan Modes

**Constant Mode** (`pulsing=0`):
- Fan runs at configured `speed` while firing
- Spins down after `spindownMs` delay when firing stops

**Pulsing Mode** (`pulsing=1`):
- Fan pulses with each shot for more realistic smoke puffs
- `pulseHigh`: Speed during shot (e.g., 255)
- `pulseLow`: Speed between shots (e.g., 80)
- `pulseMs`: High-speed pulse duration (e.g., 50ms)

### Firing Behavior

1. `TRIGGER_ON rpm=N` starts muzzle flash at specified rate (1-3000 RPM)
2. Flash pulse duration: 30ms with 80ms fade-out
3. Smoke fan starts immediately (constant or pulsing mode)
4. `TRIGGER_OFF` stops flash immediately
5. Fan continues for `fanDelayMs` before turning off

---

## Build & Upload

### Using Arduino IDE

1. Install the [Arduino-Pico board package](https://github.com/earlephilhower/arduino-pico)
   - File → Preferences → Additional Board Manager URLs: `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   - Tools → Board → Boards Manager → Search "pico" → Install "Raspberry Pi Pico/RP2040"

2. Open `controllers/gunfx/pico/gunfx_pico.ino`

3. Configure:
   - Tools → Board → Raspberry Pi Pico/RP2040 → Raspberry Pi Pico
   - Tools → Port → (select COM port)
   - Tools → USB Stack → "Pico SDK"

4. Upload

### Using PlatformIO

```bash
cd controllers/gunfx/pico
pio run -t upload
```

## Troubleshooting

**Pico not detected:**
- Hold BOOTSEL button while connecting USB
- Pico should appear as a mass storage device
- Drag and drop UF2 file from Arduino IDE build output

**Serial communication issues:**
- Confirm baud rate (115200)
- Check USB-C cable (must support data, not just power)
- On Linux, add user to `dialout` group: `sudo usermod -a -G dialout $USER`

