# GunFX Controller - Raspberry Pi Pico

Slave microcontroller for gun FX hardware control. Receives commands from the main HubFX board over USB serial and drives:
- Nozzle flash LED (PWM)
- Smoke heater and fan
- Turret servos (pitch, yaw, retract)
- Status LEDs (heater indicator, firing status)

**Version:** 0.4.0  
**Protocol:** Binary COBS with CRC-8  
**Baud Rate:** 1000000 (1Mbps)

---

## Hardware

The GunFX board connects to the main ScaleFX Hub board via a custom USB-C cable. The USB-C cable provides both power and data communication to the Pico.

### Pico Pinout

| GPIO | Function |
|------|----------|
| 1 | Servo 1 |
| 2 | Servo 2 |
| 3 | Servo 3 |
| 4 | I2C SDA |
| 5 | I2C SCL |
| 9 | Nozzle Flash LED (PWM) |
| 13 | Indicator LED (connection) |
| 14 | Indicator LED (error) |
| 15 | Smoke Heater (Motor 0 CW) |
| 16 | Motor 0 CCW — held LOW |
| 17 | Smoke Fan Motor PWM (Motor 1 CW) |
| 18 | Motor 1 CCW — held LOW |
| 21 | Status LED — heater on (Motor 0) |
| 23 | Status LED — fan running (Motor 1) |
| 24 | Status LED — fan spindown (Motor 1) |
| 25 | Status LED — firing active (Motor 2) |
| 26 | Status LED — flash pulse (Motor 2) |
| 29 | Battery voltage ADC (÷6 divider) |

### Status LED Indicators

LEDs are mapped to GearControl board motor channels:

| GPIO | Motor Ch | Function |
|------|----------|----------|
| 21 | Motor 0 CW | Heater active |
| 23 | Motor 1 CW | Fan running |
| 24 | Motor 1 CCW | Fan spinning down |
| 25 | Motor 2 CW | Firing active |
| 26 | Motor 2 CCW | Flash pulse |

**Indicator LEDs (PicoServer, GP13/GP14):**
- GP13: Connection status (blink=waiting, solid=connected, off=lost)
- GP14: Error (off=normal, fast blink=error)

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
| 0x01-0x2F | GunFX | Controller-specific commands |
| 0xF0-0xFF | Core | Universal system commands |

### ACK/NACK Response Protocol

All commands return a response:
- **ACK (0xF6)** - Command executed successfully
- **NACK (0xF7)** - Command failed with error code and reason

Exceptions (fire-and-forget, no response expected):
- `REBOOT (0xF8)` - Device reboots immediately
- `BOOTSEL (0xF9)` - Device enters bootloader immediately

---

## System Commands (0xF0-0xFF)

| Type | Name | Payload | Response | Description |
|------|------|---------|----------|-------------|
| 0xF0 | INIT | (none) | INIT_READY | Initialize connection |
| 0xF1 | SHUTDOWN | (none) | ACK | Safe shutdown, outputs off |
| 0xF2 | KEEPALIVE | (none) | ACK | Reset watchdog timer |
| 0xF3 | INIT_READY | (response) | — | Device info response |
| 0xF4 | STATUS | (response) | — | Status telemetry |
| 0xF5 | STATUS_REQ | (none) | STATUS | Request status |
| 0xF6 | ACK | (none) | — | Command success |
| 0xF7 | NACK | code:u8, reason:str | — | Command failure |
| 0xF8 | REBOOT | (none) | — | Reboot (fire-and-forget) |
| 0xF9 | BOOTSEL | (none) | — | Enter bootloader |

### INIT_READY Payload

| Field | Type | Description |
|-------|------|-------------|
| name_len | u8 | Device name length |
| name | string | Device name (e.g., "GunFX-A4B2") |
| ver_len | u8 | Version length |
| version | string | Firmware version (e.g., "0.3.0") |
| plat_len | u8 | Platform length |
| platform | string | Hardware platform (e.g., "RP2040") |
| cpuMHz | u16le | CPU frequency in MHz |
| ramBytes | u32le | Free RAM in bytes |
| build | u32le | Build number |

---

## GunFX Commands (0x01-0x2F)

### Trigger Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x01 | TRIGGER_ON | rpm:u16le | Start firing at specified RPM (1-3000) |
| 0x02 | TRIGGER_OFF | fan_delay_ms:u16le | Stop firing; delay before fan turns off |

**TRIGGER_ON Behavior:**
1. Starts muzzle flash at specified rate (1-3000 RPM)
2. Flash pulse: 30ms at full brightness + 80ms fade-out
3. Smoke fan starts immediately (constant or pulsing mode)
4. Recoil jerk applied to configured servos

**TRIGGER_OFF Behavior:**
1. Flash stops immediately
2. Fan continues for `fan_delay_ms` before turning off

### Servo Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x10 | SRV_SET | servo_id:u8, pulse_us:u16le | Set servo position (500-2500µs) |
| 0x11 | SRV_SETTINGS | servo_id:u8, min:u16le, max:u16le, speed:u16le, accel:u16le, decel:u16le | Configure servo limits and motion profile |
| 0x12 | SRV_RECOIL_JERK | servo_id:u8, jerk_us:u16le, variance_us:u16le | Configure recoil jerk per shot |

**Motion Profile:**
- Uses trapezoidal velocity profile
- Accelerate → Cruise → Decelerate
- Automatic braking on direction reversal
- Defaults: speed=4000 µs/s, accel=8000 µs/s², decel=8000 µs/s²

**Recoil Jerk Effect:**
- On each shot, random jerk offset applied to servo
- Direction: randomly ± (positive or negative)
- Magnitude: base `jerk_us` + random(0 to `variance_us`)
- Clears after flash fade completes
- Example: `jerk_us=50, variance_us=25` → ±50µs to ±75µs per shot

### Smoke Control

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x20 | SMOKE_HEAT | on:u8 (0=off, 1=on) | Control smoke heater |
| 0x21 | SMOKE_SETTINGS | pulsing:u8, speed:u8, pulse_high:u8, pulse_low:u8, pulse_ms:u16le, spindown_ms:u16le | Configure smoke fan behavior |
| 0x22 | SMOKE_RESET | (none) | Clear smoke error states (disconnect/overcurrent) |
| 0x23 | SMOKE_CURRENT_LIMIT | channel:u8 (0=heater, 1=fan), limit_mA:u16le | Set overcurrent protection limit (0=disable) |

**Fan Modes:**

| Mode | Description |
|------|-------------|
| Constant (pulsing=0) | Fan runs at configured `speed` while firing, spins down after `spindown_ms` |
| Pulsing (pulsing=1) | Fan pulses with each shot for realistic smoke puffs |

**Pulsing Mode Parameters:**
- `pulse_high`: Speed during shot (e.g., 255)
- `pulse_low`: Speed between shots (e.g., 80)
- `pulse_ms`: High-speed pulse duration (e.g., 50ms)

---

## Status Telemetry

### STATUS Payload (0xF4)

| Field | Type | Description |
|-------|------|-------------|
| flags | u8 | Status bit field (see below) |
| fan_off_ms | u16le | Remaining fan spindown time |
| servo1_us | u16le | Servo 1 position |
| servo2_us | u16le | Servo 2 position |
| servo3_us | u16le | Servo 3 position |
| rpm | u16le | Current firing rate |

**Status Flags:**
| Bit | Name | Description |
|-----|------|-------------|
| 0 | firing | Currently firing |
| 1 | flash_active | Flash LED on |
| 2 | flash_fading | Flash fading out |
| 3 | heater_on | Smoke heater active |
| 4 | fan_on | Smoke fan running |
| 5 | fan_spindown | Fan spinning down |

### Smoke Error Reasons (STATUS bytes 40-41)

Reported per-channel in STATUS payload:

| Byte | Channel | Description |
|------|---------|-------------|
| 40 | Heater | SmokeErrorReason code |
| 41 | Fan | SmokeErrorReason code |

| Code | Name | Description |
|------|------|-------------|
| 0x00 | NONE | No error |
| 0x01 | HEATER_DISCONNECTED | Heater drawing 0 current while ON |
| 0x02 | FAN_DISCONNECTED | Fan drawing 0 current while ON |
| 0x03 | HEATER_OVERCURRENT | Heater current exceeded limit |
| 0x04 | FAN_OVERCURRENT | Fan current exceeded limit |

### Overcurrent Throttle State (STATUS bytes 42-43)

| Byte | Channel | Description |
|------|---------|-------------|
| 42 | Heater | PWM duty cap (255=no throttle) |
| 43 | Fan | PWM duty cap (255=no throttle) |

**Disconnect Detection:**
- When heater/fan is ON, INA226 current is monitored
- 500ms startup ignore period after turn-on
- If current stays below 5mA for >1 second → flagged as disconnected
- Error auto-clears when current returns (re-plugged)
- Error auto-clears when the channel is turned off then on again
- Explicit clear via `SMOKE_RESET` (0x22) command

**Overcurrent Protection:**
- Default limits: heater 5000mA, fan 3000mA (configurable via `SMOKE_CURRENT_LIMIT`)
- When current exceeds limit for >100ms (debounce), PWM is stepped down
- Step-down rate: -10 duty every 50ms (≈1.25s from full to zero)
- If PWM reaches 0 and current still over limit → channel shut off, overcurrent error set
- While throttled and current under limit, throttled duty is maintained (no auto-increase)
- Throttle state resets on: off→on transition, `SMOKE_RESET`, or `SMOKE_CURRENT_LIMIT` command
- Set limit to 0 to disable overcurrent protection for a channel
- Error indicator LED (GP14) blinks when any smoke error is active

---

## Error Codes

### Generic Errors (SerialError namespace)

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

### GunFX-Specific Errors (0x20-0x4F)

| Code | Name | Description |
|------|------|-------------|
| 0x20 | SERVO_INVALID_ID | Servo ID out of range (1-3) |
| 0x21 | SERVO_PULSE_RANGE | Pulse width outside 500-2500µs |
| 0x22 | SERVO_MIN_MAX | minUs >= maxUs |
| 0x23 | SERVO_NOT_CONFIGURED | Servo not configured |
| 0x30 | INVALID_FAN_SPEED | Invalid fan speed value |
| 0x31 | HEATER_DISCONNECTED | Heater drawing 0 current while ON |
| 0x32 | FAN_DISCONNECTED | Fan motor drawing 0 current while ON |
| 0x33 | HEATER_OVERCURRENT | Heater current exceeded protection limit |
| 0x34 | FAN_OVERCURRENT | Fan current exceeded protection limit |
| 0x40 | INVALID_RPM | RPM out of range (1-3000) |
| 0x41 | ALREADY_FIRING | Already firing |
| 0x42 | NOT_FIRING | Not currently firing |

---

## Architecture

Uses `PicoServer` component for common server boilerplate (serial init, device naming, indicator LEDs, core protocol, connection management). Module-specific logic is handled by `GunFxServer`.

### Module Architecture

The firmware is organized into self-contained modules following the same pattern as GearControl:

| Module | File | Purpose |
|--------|------|---------|
| `MuzzleFlash` | `muzzle_flash.h/cpp` | Flash LED pulse/fade, per-shot recoil jerk on servos |
| `SmokeGenerator` | `smoke_generator.h/cpp` | Heater relay + PWM fan (constant/pulsing modes) |
| `ServoControl` | `lib/srv_control/` | Motion-profiled servo positioning (shared library) |
| `PicoServer` | `lib/components/` | Serial, indicators, core protocol, connection management |

**Module wiring (in `setup()`):**
```
MuzzleFlash ──onShot()──→ SmokeGenerator.triggerPulse()
     │
     └── attachServo() → ServoControl[0..2] (recoil jerk)
```

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
│  │  CoreCommandServer  │  │       GunFxServer             │  │
│  │  (Priority 1)       │  │       (Priority 2)          │  │
│  ├─────────────────────┤  ├─────────────────────────────┤  │
│  │ INIT, SHUTDOWN      │  │ TRIGGER_ON/OFF              │  │
│  │ REBOOT, BOOTSEL     │  │ SRV_SET, SRV_SETTINGS       │  │
│  │ KEEPALIVE, STATUS   │  │ SRV_RECOIL_JERK             │  │
│  └─────────────────────┘  │ SMOKE_HEAT, SMOKE_SETTINGS  │  │
│                           └─────────────────────────────┘  │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │            Hardware Modules (local to firmware)          ││
│  │                                                         ││
│  │  ┌─────────────┐  ┌─────────────────┐  ┌────────────┐ ││
│  │  │ MuzzleFlash │  │ SmokeGenerator  │  │ServoControl│ ││
│  │  │ (flash+jerk)│──│ (heater+fan)    │  │ [0..2]     │ ││
│  │  └─────────────┘  └─────────────────┘  └────────────┘ ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

**Chain of Responsibility Pattern:**
1. `PicoServer` initializes serial, indicators, and core protocol
2. `CommandRouter` receives COBS packet
3. Routes to `CoreCommandServer` first (system commands)
4. If not handled, routes to `GunFxServer` (module commands)
5. If no handler matches, sends NACK with INVALID_COMMAND

---

## Build & Upload

### Using PlatformIO (Recommended)

```bash
cd controllers/gunfx/pico

# Build
pio run

# Build and upload (device must be in BOOTSEL mode)
pio run -t upload

# Clean build
pio run -t clean
```

### Using build_and_flash.py Script

The `scripts/build_and_flash.py` script provides automated build and flash with verification:

```bash
cd controllers/gunfx/pico

# Full build and flash (auto-detects port, sends BOOTSEL command)
python scripts/build_and_flash.py

# Skip build, flash existing firmware
python scripts/build_and_flash.py --no-build

# Incremental build (no clean)
python scripts/build_and_flash.py --no-clean

# Specify port
python scripts/build_and_flash.py --port COM3
```

**Script Features:**
- Increments build number automatically
- Sends BOOTSEL command over serial (no button press needed)
- Waits for RPI-RP2 drive to appear
- Copies UF2 firmware file
- Verifies post-flash version

### Manual BOOTSEL Method

1. Hold BOOTSEL button on Pico while connecting USB
2. RPI-RP2 drive appears
3. Copy `.pio/build/pico/firmware.uf2` to drive
4. Device auto-reboots with new firmware

---

## Version History

- **v0.4.0** - Modular architecture refactoring
  - Extracted `MuzzleFlash` module (flash pulse/fade, recoil jerk)
  - Extracted `SmokeGenerator` module (heater + fan, constant/pulsing modes)
  - Separated SRV_RECOIL_JERK into dedicated callback (was mixed with SRV_SETTINGS)
  - Removed `GunFxServoConfig.recoilJerkUs/recoilJerkVarianceUs` fields (now in `RecoilJerkConfig`)
  - Reduced `gunfx_pico.ino` from 531 to ~250 lines (glue code only)
  - No wire protocol changes — fully backward compatible

- **v0.3.0** - Binary-only protocol
  - Removed text protocol support
  - Using new serial library (CoreCommandServer + GunFxServer)
  - COBS framing with CRC-8 validation
  - Chain of Responsibility architecture

- **v0.2.0** - Dual protocol support
  - Added binary COBS protocol
  - Protocol negotiation via INIT command

- **v0.1.0** - Initial release
  - Text-based protocol only

