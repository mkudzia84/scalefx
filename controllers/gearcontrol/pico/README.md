# GearControl Pico Controller

> Landing gear effects controller for scale model aircraft

## Overview

The GearControl controller manages retractable landing gear with door sequencing, motor stall detection via INA226 current monitoring, and yaw steering.

### LandingGear Module

Each landing gear unit is encapsulated in a `LandingGear` module that binds together:
- **0-2× ServoControl** for door servos (optional, configurable via DOOR_MODE)
- **2× LedControl** for status LEDs (CW/CCW deploy/retract indicators)
- **Motor H-bridge** pair (CW/CCW GPIOs) for gear extend/retract
- **INA226** current monitor reference for motor stall detection

The yaw steering servo remains in the main controller module but uses `ServoControl` for configurability.

**Hardware:** Raspberry Pi Pico (RP2040)  
**Protocol:** Binary COBS with CRC-8 (115200 baud)  
**Firmware:** v0.2.0 (Build 2)

## Architecture

Uses `PicoServer` component for common server boilerplate (serial init, device naming, indicator LEDs, core protocol, connection management). Module-specific logic is handled by `GearControlServer`.

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
│  │  CoreCommandServer  │  │   GearControlServer          │  │
│  │  (Priority 1)       │  │      (Priority 2)           │  │
│  ├─────────────────────┤  ├─────────────────────────────┤  │
│  │ INIT, SHUTDOWN      │  │ GEAR_DEPLOY/RETRACT/STOP   │  │
│  │ REBOOT, BOOTSEL     │  │ SERVO_SET, SRV_SETTINGS    │  │
│  │ KEEPALIVE, STATUS   │  │ YAW_CONFIG, YAW_INPUT      │  │
│  └─────────────────────┘  │ GEAR_CONFIG, DOOR_CONFIG   │  │
│                           │ BATTERY_CONFIG, CALIBRATE  │  │
│                           └─────────────────────────────┘  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

## Hardware Configuration

| Component | Count | Pins | Description |
|-----------|-------|------|-------------|
| Servos | 8 | GP0-3, GP6-9 | 6 door + 1 yaw + 1 spare |
| INA226 | 3 | I2C (GP4/GP5) | Current monitoring per motor |
| Indicators | 2 | GP13-14 | Bi-color RED/GREEN master status |
| Motors | 3 | GP15-20 (H-bridge CW/CCW) | Landing gear extend/retract |
| Status LEDs | 6 | GP21-26 | CW/CCW indicator per motor |
| Voltage Sense | 1 | GP29 (ADC) | Battery voltage (÷6 divider) |

### Servo Mapping

| Servo ID | Function | Default |
|----------|----------|---------|
| 0 | Gear 0 door servo A (nose) | 1500µs |
| 1 | Gear 0 door servo B (nose) | 1500µs |
| 2 | Gear 1 door servo A (left main) | 1500µs |
| 3 | Gear 1 door servo B (left main) | 1500µs |
| 4 | Gear 2 door servo A (right main) | 1500µs |
| 5 | Gear 2 door servo B (right main) | 1500µs |
| 6 | Yaw steering servo | 1500µs |
| 7 | Spare | 1500µs |

### Status LED Mapping

| LED | Pin | Function |
|-----|-----|----------|
| 0 | GP21 | Motor 0 CW (deploying/deployed) |
| 1 | GP22 | Motor 0 CCW (retracting/retracted) |
| 2 | GP23 | Motor 1 CW (deploying/deployed) |
| 3 | GP24 | Motor 1 CCW (retracting/retracted) |
| 4 | GP25 | Motor 2 CW (deploying/deployed) |
| 5 | GP26 | Motor 2 CCW (retracting/retracted) |

### Indicator LED Mapping

| LED | Pin | Function |
|-----|-----|----------|
| 0 | GP13 | Connection (blink=waiting for INIT, solid=connected, off=lost) |
| 1 | GP14 | Error/warning (off=normal, fast blink=gear error, slow blink=low voltage) |

> These indicator LEDs follow a standard pattern across all ScaleFX controllers.
> LED 1 blink rates: 200ms = gear error (highest priority), 500ms = low voltage triggered.

### Voltage Sense

| Parameter | Value |
|-----------|-------|
| Pin | GP29 (ADC3) |
| Divider | ÷6 resistor divider |
| ADC Resolution | 12-bit (0-4095) |
| Reference | 3.3V |
| Range | 0-19.8V |
| Example | 8.2V input → raw ≈ 1700 → 8203 mV |

### INA226 I2C Addresses

| Motor | Address | A0 | A1 |
|-------|---------|----|----|
| 0 (Nose) | 0x40 | GND | GND |
| 1 (Left) | 0x41 | VS | GND |
| 2 (Right) | 0x44 | GND | VS |

## Protocol

### Packet Types (0x60-0x7F)

| Type | Name | Payload | Description |
|------|------|---------|-------------|
| 0x60 | GEAR_DEPLOY | `[gear_id:u8]` | Deploy landing gear |
| 0x61 | GEAR_RETRACT | `[gear_id:u8]` | Retract landing gear |
| 0x62 | GEAR_STOP | `[gear_id:u8]` | Emergency stop motor |
| 0x63 | GEAR_ALL | `[action:u8]` | 0=retract, 1=deploy, 2=stop all |
| 0x64 | SERVO_SET | `[id:u8][pulse_us:u16LE]` | Set servo position |
| 0x65 | SRV_SETTINGS | `[id:u8][min:u16LE][max:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]` | Configure servo |
| 0x66 | GEAR_CONFIG | `[id:u8][flags:u8][stall_mA:u16][timeout_ms:u16]` | Configure gear |
| 0x67 | DOOR_CONFIG | `[id:u8][open0:u16][close0:u16][open1:u16][close1:u16]` | Configure doors |
| 0x68 | YAW_CONFIG | `[gear_id:u8][neutral:u16][min:u16][max:u16]` | Configure yaw |
| 0x69 | YAW_INPUT | `[position_us:u16LE]` | Set yaw position |
| 0x6A | GEAR_CALIBRATE | `[gear_id:u8]` | Start stall current calibration |
| 0x6B | GEAR_CALIB_STATUS | `[gear_id:u8][phase:u8][current:u16LE][peak:u16LE][stall:u16LE]` | Calibration progress (server→client) |
| 0x6C | GEAR_CALIB_CANCEL | `[gear_id:u8]` | Cancel calibration in progress |
| 0x6D | BATTERY_CONFIG | `[auto_deploy:u8]` | Configure battery auto-deploy (0=off, 1=on) |
| 0x6E | DOOR_MODE | `[gear_id:u8][mode:u8][delay_ms:u16LE]` | Configure door activation mode |

### Error Codes (0x60-0x6F)

| Code | Name | Description |
|------|------|-------------|
| 0x60 | INVALID_GEAR_ID | Gear ID out of range (0-2) |
| 0x61 | INVALID_SERVO_ID | Servo ID out of range (0-7) |
| 0x62 | GEAR_BUSY | Gear sequence in progress |
| 0x63 | MOTOR_STALL | Motor stall detected |
| 0x64 | MOTOR_TIMEOUT | Operation timed out |
| 0x65 | SERVO_OUT_OF_RANGE | Servo pulse out of range (500-2500µs) |
| 0x66 | INA226_ERROR | Power monitor communication error |
| 0x67 | YAW_NOT_AVAILABLE | Yaw not configured for this gear |
| 0x68 | INVALID_ACTION | Invalid gear-all action |
| 0x69 | NO_CURRENT_MONITOR | No INA226 attached (required for calibration) |
| 0x6A | NOT_CALIBRATING | Gear is not currently calibrating (cancel rejected) |

### GEAR_CONFIG Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | CLOSE_DOORS_ON_RETRACT | Close doors after gear retracts |
| 1 | CLOSE_DOORS_ON_DEPLOY | Close doors after gear deploys |
| 2 | HAS_YAW | This gear has yaw servo (used for steering) |

### STATUS Response (33 bytes module data)

After the 12-byte core header `[counter:u32][uptime:u32][freeRam:u32]`:

```
Per gear (3 × 9 = 27 bytes):
  [state:u8]                   // GearState enum (see below)
  [motorCurrent_mA:u16LE]     // Current motor draw in milliamps
  [door0Pos_us:u16LE]         // Door servo 0 position in µs
  [door1Pos_us:u16LE]         // Door servo 1 position in µs
  [calibratedStall_mA:u16LE]  // Calibrated stall threshold (0 = not calibrated)

Yaw + LEDs + Voltage + Config (6 bytes):
  [yawPos_us:u16LE]           // Yaw servo position in µs
  [ledFlags:u8]               // Bits 0-5 status LEDs, 6-7 indicator LEDs
  [batteryVoltage_mV:u16LE]   // Battery voltage in millivolts
  [batteryConfigFlags:u8]     // Bit 0: auto-deploy enabled, Bit 1: low voltage triggered
```

**batteryConfigFlags bits:**
| Bit | Name | Description |
|-----|------|-------------|
| 0 | AUTO_DEPLOY | Auto-deploy on low voltage is enabled |
| 1 | LOW_VOLTAGE_TRIGGERED | Low voltage event fired, emergency deploy occurred |

**GearState enum:**
| Value | Name | Description |
|-------|------|-------------|
| 0 | UNKNOWN | Initial state (power-on) |
| 1 | DEPLOYED | Gear fully extended |
| 2 | RETRACTED | Gear fully retracted |
| 3 | DEPLOYING | Deploy sequence in progress |
| 4 | RETRACTING | Retract sequence in progress |
| 5 | ERROR | Sequence failed (timeout) |
| 6 | CALIBRATING | Stall current calibration in progress |

## Landing Gear Sequencing

### Deploy Sequence
1. Open door servos per door mode (skip if NONE)
2. Wait for door travel time (1500ms per door, mode-dependent)
3. Run motor forward (extend gear)
4. Monitor INA226 current — stall = fully extended
5. Stop motor
6. Optionally close doors (if `CLOSE_DOORS_ON_DEPLOY` flag set)
7. Set state to DEPLOYED
8. If yaw configured for this gear, set yaw to neutral

**Motor detection:**

- **INA226 board fault:** The INA226 is soldered on the PCB. If it fails I2C
  init during `setup()`, the gear is put into ERROR state immediately. The
  error indicator LED blinks and STATUS reports the fault. Deploy/retract
  commands are rejected with `GEAR_BUSY` (gear is in error until reset).

- **Motor unplugged at runtime:** If the INA226 is working but the motor is
  disconnected (open circuit), the firmware detects this during the 500ms
  motor startup period. If peak current stays below 20mA
  (`MOTOR_DETECT_THRESHOLD_mA`), the motor is considered absent and the
  sequence completes with door sequencing only. This handles hot-unplug.

- **Motor timeout with feedback:** If the motor drew meaningful current but
  never stalled within the timeout, this is a genuine ERROR (motor fault).

### Stall Detection

Stall detection uses two mechanisms to prevent false positives, especially during in-flight operation where aerodynamic drag increases motor load:

**Stall confirmation (200ms):** Current must exceed the threshold continuously for 200ms (`STALL_CONFIRM_ms`) before the firmware declares a stall. If current drops below the threshold during this window, the timer resets. This prevents transient drag gusts or turbulence from triggering false stall events.

**In-flight drag headroom:** Calibration is performed on the ground (static). In flight, aerodynamic drag on the gear mechanism increases the motor's free-running current, narrowing the margin between normal operation and the calibrated stall threshold. For calibrated gears, the effective threshold is raised by a percentage of the measured baseline (free-running) current:

```
effectiveThreshold = calibratedStall + (baseline × dragHeadroom%)
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `STALL_CONFIRM_ms` | 200ms | Stall current must be sustained this long |
| `DRAG_HEADROOM_PCT` | 20% | Percentage of baseline added to threshold |
| `CALIB_MARGIN_FACTOR` | 80% | Calibrated threshold = peak × 0.80 |

**Example** (200mA free-running, 500mA stall peak):
- Calibrated threshold: 500 × 0.80 = 400mA
- Drag headroom: 200 × 0.20 = 40mA
- Effective threshold: 400 + 40 = **440mA**
- In-flight running current (with drag): ~300mA → still 140mA below threshold ✓

> **Note:** Drag headroom only applies to calibrated gears. Uncalibrated gears use the manually configured `stallCurrent_mA` directly (user sets appropriate margin).

### Retract Sequence
1. If yaw configured, return yaw to neutral position
2. Open door servos per door mode (skip if NONE)
3. Wait for door travel time (1500ms per door, mode-dependent)
4. Run motor reverse (retract gear)
5. Monitor INA226 current — stall = fully retracted
6. Stop motor
7. Close doors (if `CLOSE_DOORS_ON_RETRACT` flag set, default: yes)
8. Set state to RETRACTED

### Yaw Behavior
- Only active when the associated gear is in DEPLOYED state
- YAW_INPUT commands are silently ignored when gear is not deployed
- Yaw returns to neutral before retract sequence begins
- Range is clamped to configured min/max

### Battery Auto-Deploy

A configurable safety feature that automatically deploys all landing gears when the battery voltage drops below the low warning threshold (3.2V/cell for LiPo).

**Enable:** Send `BATTERY_CONFIG` with `[0x01]`  
**Disable:** Send `BATTERY_CONFIG` with `[0x00]`

**Behavior:**
- Only triggers when the controller is initialized (connected)
- Uses `BatteryMonitor::onLowVoltage()` callback with hysteresis (50mV/cell re-arm)
- Deploys all 3 gears simultaneously (same as `GEAR_ALL` with action=deploy)
- Auto-deploy setting is reset to OFF on shutdown/disconnect (requires re-configuration)
- Current state is reported in STATUS response `batteryConfigFlags` (bit 0 = enabled, bit 1 = triggered)

**Visual indicators when emergency deploy fires:**
- **Indicator LED 1** (GP14): slow blink 500ms (low voltage warning)
- **Per-gear status LEDs**: deploy LED solid ON + retract LED slow blink 500ms (emergency-deployed)
- Both visual indicators persist until the controller is reset or re-initialized
- A normal `GEAR_DEPLOY` command clears the emergency flag for that gear

### Door Mode Configuration

Each landing gear can be configured with a door activation mode that determines how door servos behave during deploy/retract sequences. Doors are optional — gears can operate with no doors, one door, or two doors.

**Wire format (4 bytes):**
```
[gear_id:u8][mode:u8][delay_ms:u16LE]
```

| Mode | Value | Doors | Behavior |
|------|-------|-------|----------|
| NONE | 0 | 0 | Motor only — door open/close steps are skipped entirely |
| SINGLE | 1 | 1 | Door servo A only (servo index 0) |
| DUAL_SYNC | 2 | 2 | Both doors move simultaneously (default, backward compatible) |
| DUAL_DELAY | 3 | 2 | Door B starts after configurable `delay_ms` |
| DUAL_SEQ | 4 | 2 | Door B starts after door A reaches target |

**Default:** `DUAL_SYNC` with `delay_ms=500` (preserves original behavior)

**Door ordering for DUAL_DELAY and DUAL_SEQ:**
- **Opening:** Door A (servo 0) first → Door B (servo 1) second
- **Closing:** Door B (servo 1) first → Door A (servo 0) second

This mirrors real aircraft inner/outer door behavior where the inner door opens first and closes last.

**`delay_ms` parameter:**
- Only used by `DUAL_DELAY` mode (ignored for other modes)
- Valid range: 0–5000ms
- Controls the time gap between starting door A and starting door B

**Mode behavior with gear flags:**
- `CLOSE_DOORS_ON_DEPLOY` / `CLOSE_DOORS_ON_RETRACT` flags are respected
- When mode is `NONE`, close-door flags have no effect (no doors to close)
- When mode is `SINGLE`, only door A is closed

### SRV_SETTINGS (Servo Configuration)

Servo configuration follows the same pattern as GunFX and LightFX controllers.

**Wire format (11 bytes):**
```
[servo_id:u8][min_us:u16LE][max_us:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]
```

| Field | Type | Unit | Description |
|-------|------|------|-------------|
| servo_id | u8 | — | Servo ID (0-5: door, 6: yaw, 7: spare) |
| min_us | u16LE | µs | Minimum pulse width limit |
| max_us | u16LE | µs | Maximum pulse width limit |
| speed | u16LE | µs/s | Maximum speed (trapezoidal profiling) |
| accel | u16LE | µs/s² | Acceleration rate |
| decel | u16LE | µs/s² | Deceleration rate |

**Defaults:** speed=4000 µs/s, accel=8000 µs/s², decel=8000 µs/s²

**Servo ID mapping:**
- 0-1: Gear 0 door servos (nose)
- 2-3: Gear 1 door servos (left main)
- 4-5: Gear 2 door servos (right main)
- 6: Yaw steering servo
- 7: Spare (not currently used)

Door servos (0-5) are managed by the `LandingGear` module via `ServoControl`.
Yaw servo (6) uses `ServoControl` directly in the main controller.

### GEAR_CALIBRATE (Stall Current Calibration)

Automates stall current detection by probing motor movement in each direction.
Handles the edge case where calibration starts at a mechanical endpoint by
first running the motor in retract to clear any deploy endpoint position.

**Sequence:**
1. **CLEAR_RUN**: Run motor in retract direction to move away from deploy endpoint
   - Uses configured `stallCurrent_mA` for stall detection (stop early if already at retract endpoint)
   - Timeout: 2 seconds max
2. **CLEAR_SETTLE**: Wait 500ms for motor to stop
3. **DEPLOY_RUN**: Run motor in deploy direction (CW)
   - Skip inrush spike (first 100ms)
   - Baseline sampling window (100-500ms): average current = free-running baseline
   - After startup: detect stall (baseline + 150mA sustained 200ms) or safety cutoff (3000mA)
   - Record peak current for deploy direction
4. **MID_SETTLE**: Stop motor, settle for 1000ms
5. **RETRACT_RUN**: Run motor in retract direction (CCW), repeat measurement
6. Calculate threshold: min(deploy_peak, retract_peak) × 80%
7. Update `gearConfig.stallCurrent_mA` with calibrated value

**Why CLEAR_RUN?** If calibration starts at the deploy endpoint, DEPLOY_RUN would
immediately stall. The baseline (average current during 100-500ms) would equal the
stall current, making relative stall detection (baseline + 150mA) impossible.
By retracting first, DEPLOY_RUN always starts away from the deploy endpoint,
producing an accurate free-running baseline. After DEPLOY_RUN stalls at the deploy
endpoint, RETRACT_RUN naturally starts from the correct position.

**During calibration:**
- Gear state is `CALIBRATING` (6)
- Status LEDs show alternating chase pattern
- STATUS response includes live `motorCurrent_mA` and `calibratedStall_mA`
- Server emits `GEAR_CALIB_STATUS` packets every 250ms and on phase transitions

**GEAR_CALIB_STATUS wire format (8 bytes, server→client unsolicited):**
```
[gear_id:u8][phase:u8][current_mA:u16LE][peak_mA:u16LE][calibratedStall_mA:u16LE]
```

| Field | Type | Description |
|-------|------|-------------|
| gear_id | u8 | Gear index (0-2) |
| phase | u8 | CalibPhase enum value |
| current_mA | u16LE | Live motor current reading (mA) |
| peak_mA | u16LE | Peak current for current phase (0 during CLEAR phases) (mA) |
| calibratedStall_mA | u16LE | Final calibrated value (valid when phase=COMPLETE) (mA) |

**CalibPhase enum:**
| Value | Name | Description |
|-------|------|-------------|
| 0 | IDLE | Not calibrating |
| 1 | CLEAR_RUN | Brief retract to clear deploy endpoint |
| 2 | CLEAR_SETTLE | Settle after clearing endpoint |
| 3 | DEPLOY_RUN | Running motor in deploy direction |
| 4 | MID_SETTLE | Settling between directions |
| 5 | RETRACT_RUN | Running motor in retract direction |
| 6 | COMPLETE | Calibration finished successfully |
| 7 | ERROR | Calibration failed |
| 8 | CANCELLED | Calibration cancelled by client |

**After calibration (phase=COMPLETE):**
- Gear state returns to `UNKNOWN` (position is unknown after motor probing)
- `calibratedStall_mA` in STATUS and GEAR_CALIB_STATUS reports the detected threshold
- The value is used for subsequent deploy/retract stall detection

**Cancellation:**
Send `GEAR_CALIB_CANCEL` with `[gear_id:u8]` to abort calibration.
Server ACKs and emits a final `GEAR_CALIB_STATUS` with phase=CANCELLED.
NACKs with `NOT_CALIBRATING` (0x6A) if gear is not calibrating.

**Errors:**
- `GEAR_BUSY` (0x62): Gear is mid-sequence, cannot calibrate
- `NO_CURRENT_MONITOR` (0x69): No INA226 attached to this gear
- `NOT_CALIBRATING` (0x6A): Cancel requested but gear is not calibrating

**Timeout:** 8 seconds per measurement direction. 2 seconds for endpoint clearing.
If no stall is detected, the peak current observed during the timeout period is
still used for calibration.

**Payload:** `[gear_id:u8]` — gear index 0-2

## Build

```bash
# Build firmware
cd controllers/gearcontrol/pico
python -m platformio run -e pico

# Build and flash (centralized script)
python scripts/build_and_flash.py gearcontrol
```
