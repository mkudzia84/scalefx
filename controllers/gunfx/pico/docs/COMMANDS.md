# GunFX Text Protocol Commands

Commands specific to the GunFX controller for gun effects (muzzle flash, smoke, servos).

For system commands (INIT, SHUTDOWN, REBOOT, etc.), see [Text Commands](../../../lib/serial/docs/TEXT_COMMANDS.md).

---

## Trigger Commands

### TRIGGER_ON

Start firing at specified rate.

```
TRIGGER_ON rpm=600
TRIGGER_ON rpm=1200
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `rpm` | uint16 | 1-3000 | Rounds per minute |

**Effect:** Starts muzzle flash pulsing, smoke fan, and recoil jerk.

**Errors:**
| Code | Name | Cause |
|------|------|-------|
| 0x04 | MISSING_PARAMETER | rpm parameter missing |
| 0x40 | INVALID_RPM | RPM out of range (1-3000) |
| 0x41 | ALREADY_FIRING | Already firing |

---

### TRIGGER_OFF

Stop firing with optional fan spindown delay.

```
TRIGGER_OFF
TRIGGER_OFF fanDelayMs=5000
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `fanDelayMs` | uint16 | 0-65535 | Fan spindown delay in ms (default: 0) |

**Effect:** Stops muzzle flash immediately, schedules fan shutoff after delay.

---

## Servo Commands

### SERVO_SET

Set servo position immediately.

```
SERVO_SET id=1 pulseUs=1500
SERVO_SET id=2 pulseUs=1000
SERVO_SET id=3 pulseUs=2000
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `id` | uint8 | 1-3 | Servo ID |
| `pulseUs` | uint16 | 500-2500 | Pulse width in microseconds |

**Errors:**
| Code | Name | Cause |
|------|------|-------|
| 0x04 | MISSING_PARAMETER | id or pulseUs missing |
| 0x20 | SERVO_INVALID_ID | Servo ID not 1-3 |
| 0x21 | SERVO_PULSE_RANGE | Pulse outside 500-2500µs |

---

### SERVO_CONFIG

Configure servo limits and motion profile.

```
SERVO_CONFIG id=1 minUs=1000 maxUs=2000 speed=4000 accel=8000 decel=8000
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `id` | uint8 | 1-3 | Servo ID |
| `minUs` | uint16 | 500-2500 | Minimum pulse width |
| `maxUs` | uint16 | 500-2500 | Maximum pulse width |
| `speed` | uint16 | 0-65535 | Max speed in µs/sec (0=unlimited) |
| `accel` | uint16 | 0-65535 | Acceleration in µs/sec² (0=unlimited) |
| `decel` | uint16 | 0-65535 | Deceleration in µs/sec² (0=unlimited) |

**Errors:**
| Code | Name | Cause |
|------|------|-------|
| 0x20 | SERVO_INVALID_ID | Servo ID not 1-3 |
| 0x22 | SERVO_MIN_MAX | minUs >= maxUs |

---

### SERVO_RECOIL_JERK

Configure recoil jerk effect for a servo.

```
SERVO_RECOIL_JERK id=1 jerkUs=50 varianceUs=10
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `id` | uint8 | 1-3 | Servo ID |
| `jerkUs` | uint16 | 0-500 | Jerk amount in µs (0=disabled) |
| `varianceUs` | uint16 | 0-200 | Random variance in µs |

**Effect:** On each shot, servo jerks by ±jerkUs (plus random variance).

---

## Smoke Commands

### SMOKE_HEAT

Control smoke heater.

```
SMOKE_HEAT on=1
SMOKE_HEAT on=0
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `on` | uint8 | 0-1 | 0=off, 1=on |

**Warning:** Heater requires proper thermal management!

**Errors:**
| Code | Name | Cause |
|------|------|-------|
| 0x30 | HEATER_SAFETY | Heater safety interlock triggered |

---

### SMOKE_SETTINGS

Configure smoke fan behavior.

```
SMOKE_SETTINGS pulsing=0 speed=255
SMOKE_SETTINGS pulsing=1 speed=255 pulseHigh=255 pulseLow=80 pulseMs=50 spindownMs=5000
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `pulsing` | uint8 | 0-1 | 0=constant speed, 1=pulse with shots |
| `speed` | uint8 | 0-255 | Fan speed in constant mode |
| `pulseHigh` | uint8 | 0-255 | Fan speed during shot (pulsing mode) |
| `pulseLow` | uint8 | 0-255 | Fan speed between shots (pulsing mode) |
| `pulseMs` | uint16 | 0-1000 | High-speed pulse duration in ms |
| `spindownMs` | uint16 | 0-65535 | Default spindown delay in ms |

**Modes:**
- **Constant** (`pulsing=0`): Fan runs at fixed `speed` while firing
- **Pulsing** (`pulsing=1`): Fan pulses with each shot for realistic smoke puffs

---

## Status Telemetry

### STATUS

Periodic status update from slave (sent automatically every 1 second).

```
STATUS firing=1 flash=1 fading=0 heater=1 fan=1 spindown=0 fanOffMs=0 srv1=1500 srv2=1500 srv3=1500 rpm=600
```

| Field | Type | Description |
|-------|------|-------------|
| `firing` | bool | Currently firing |
| `flash` | bool | Flash active |
| `fading` | bool | Flash fading |
| `heater` | bool | Heater on |
| `fan` | bool | Fan on |
| `spindown` | bool | Fan spinning down |
| `fanOffMs` | uint16 | Ms until fan off |
| `srv1-3` | uint16 | Servo positions in µs |
| `rpm` | uint16 | Current rate of fire |

---

## Error Codes

### GunFX-Specific Errors (0x20-0x4F)

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

## Testing Examples

### Basic Connection Test
```
INIT protocol=text
# Wait for INIT_READY response
KEEPALIVE
```

### Fire Test (short burst)
```
INIT protocol=text
SMOKE_HEAT on=1
# Wait 30s for heater
TRIGGER_ON rpm=600
# Wait 2 seconds
TRIGGER_OFF fanDelayMs=3000
SMOKE_HEAT on=0
```

### Servo Test
```
INIT protocol=text
SERVO_SET id=1 pulseUs=1000
SERVO_SET id=1 pulseUs=2000
SERVO_SET id=1 pulseUs=1500
```

### Configure Recoil
```
INIT protocol=text
SERVO_RECOIL_JERK id=1 jerkUs=30 varianceUs=10
SERVO_RECOIL_JERK id=2 jerkUs=20 varianceUs=5
```

### Pulsing Smoke Fan
```
INIT protocol=text
SMOKE_SETTINGS pulsing=1 pulseHigh=255 pulseLow=60 pulseMs=40
TRIGGER_ON rpm=600
```
