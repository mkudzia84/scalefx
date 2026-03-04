# ScaleFX Libraries

Shared libraries for ScaleFX microcontroller firmware.

## Library Overview

| Library | Purpose | Dependencies |
|---------|---------|--------------|
| [components](components/) | Hardware component drivers (I2C, LED, PWM, servo) | Arduino, Wire, Servo |
| [serial](serial/) | Communication protocol stack | Arduino |

## Libraries

### components

Reusable hardware component drivers for all ScaleFX controllers. Contains I2C device framework, LED control, PWM input monitoring, and servo output with motion profiling.

**Includes:**

| Module | Headers | Description |
|--------|---------|-------------|
| I2C Framework | `i2c_device.h` | Base class for all I2C peripherals |
| INA226 | `ina226.h` | TI INA226 power/current/voltage monitor |
| LED Control | `led_control.h`, `led_events.h`, `led_event_seq.h` | GPIO LED control with event-based animations |
| PWM Input | `pwm_control.h` | RC PWM input with averaging and hysteresis |
| Servo Output | `srv_control.h` | Servo motion profiling with jerk effects |

**Usage:**
```cpp
#include <led_control.h>
#include <srv_control.h>
#include <pwm_control.h>
#include <ina226.h>

LedControl statusLed;
statusLed.begin(13);

ServoControl servo;
servo.begin(1, 500, 2500);
servo.setTarget(1800);

PwmInput throttle;
throttle.begin(PwmInputType::Pwm, 10);

INA226 monitor;
monitor.begin(Wire, 0x40, 0.1f, 3.2f);
```

See [components/README.md](components/README.md) for full documentation.

---

### serial

Complete serial communication stack for ScaleFX controllers.

**Features:**
- Binary protocol (COBS framing, CRC-8)
- Protocol-agnostic interfaces
- ACK/NACK error handling
- USB Host support for RP2040

See [serial/README.md](serial/README.md) for full documentation.  
See [serial/PROTOCOL.md](serial/PROTOCOL.md) for binary protocol specification.

---

## PlatformIO Configuration

All controllers use auto-discovery:
```ini
lib_extra_dirs = ../../lib
```

This makes all libraries under `controllers/lib/` available. PlatformIO resolves dependencies automatically based on `#include` directives in the firmware source.
