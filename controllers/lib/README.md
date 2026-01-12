# ScaleFX Libraries

Shared libraries for ScaleFX microcontroller firmware (HubFX, GunFX).

## Library Overview

| Library | Purpose | Dependencies |
|---------|---------|--------------|
| [led_control](led_control/) | GPIO LED control | Arduino |
| [pwm_control](pwm_control/) | RC PWM input monitoring | Arduino |
| [srv_control](srv_control/) | Servo output with motion profiling | Arduino, Servo |
| [serial](serial/) | Communication protocol stack | Arduino |

## Libraries

### led_control

Simple GPIO LED control with on/off, toggle, and active-low support.

**Features:**
- On/off control
- Active-low mode (LED on when pin LOW)
- State tracking and toggle

**Usage:**
```cpp
#include <led_control.h>

LedControl statusLed;
statusLed.begin(13);        // GPIO 13
statusLed.on();             // Turn on
statusLed.toggle();         // Toggle
```

See [led_control/README.md](led_control/README.md) for full documentation.

---

### pwm_control

RC PWM input monitoring with hardware interrupts, moving average filtering, and hysteresis.

**Features:**
- Hardware interrupt-based PWM measurement
- Moving average filter (8 samples)
- Hysteresis threshold detection
- Configurable channel-to-GPIO mapping
- Async callbacks for value changes

**Usage:**
```cpp
#include <pwm_control.h>

PwmInput throttle;
throttle.begin(PwmInputType::Pwm, 10);  // GP10
throttle.update();
int avg = throttle.average();           // Filtered value
bool above = throttle.aboveThreshold(1500, 50);  // With hysteresis
```

See [pwm_control/README.md](pwm_control/README.md) for full documentation.

---

### srv_control

Servo output control with trapezoidal motion profiling, acceleration/deceleration, and jerk effects.

**Features:**
- Trapezoidal velocity profiles (smooth motion)
- Configurable max speed, acceleration, deceleration
- Position limits with clamping
- Jerk offset for recoil simulation
- Target reached callbacks

**Usage:**
```cpp
#include <srv_control.h>

ServoControl servo;
servo.begin(1, 500, 2500);              // Pin 1, limits 500-2500µs
servo.setMotionProfile(4000, 8000, 8000);  // Speed, accel, decel
servo.setTarget(1800);                  // Moves smoothly
servo.update();                         // Call frequently
```

See [srv_control/README.md](srv_control/README.md) for full documentation.

---

### serial

Complete serial communication stack for ScaleFX controllers.

**Features:**
- Binary protocol (COBS framing, CRC-8)
- Text protocol (human-readable for testing)
- Protocol negotiation via INIT handshake
- Protocol-agnostic interfaces (IGunFxMaster/IGunFxSlave)
- ACK/NACK error handling
- USB Host support for RP2040

**Components:**
| Component | Description |
|-----------|-------------|
| SerialInitHandler | Protocol negotiation, system commands |
| SerialBus / SerialBusText | Low-level packet transport |
| GunFxSerialMaster/Slave | GunFX binary implementation |
| GunFxSerialMasterText/SlaveText | GunFX text implementation |

**Usage:**
```cpp
#include <serial.h>

// Protocol-agnostic slave
SerialInitHandler initHandler;
IGunFxSlave* activeSlave = nullptr;

initHandler.begin(&Serial1, "GunFX-1234");
initHandler.onInitComplete([](ProtocolMode mode) {
    if (mode == ProtocolMode::Binary) {
        activeSlave = &binarySlave;
    } else {
        activeSlave = &textSlave;
    }
    
    activeSlave->onTriggerOn([](uint16_t rpm) {
        startFiring(rpm);
    });
});
```

See [serial/README.md](serial/README.md) for full documentation.  
See [serial/PROTOCOL.md](serial/PROTOCOL.md) for binary protocol specification.  
See [serial/docs/TEXT_COMMANDS.md](serial/docs/TEXT_COMMANDS.md) for text command reference.

---

## PlatformIO Configuration

Add to `platformio.ini`:

```ini
[env:myproject]
lib_deps = 
    ${PROJECT_DIR}/../lib/led_control
    ${PROJECT_DIR}/../lib/pwm_control
    ${PROJECT_DIR}/../lib/srv_control
    ${PROJECT_DIR}/../lib/serial
```

Or use symbolic includes:
```ini
lib_extra_dirs = 
    ${PROJECT_DIR}/../lib
```

## Arduino IDE

Copy the library folders to your Arduino libraries directory, or use symlinks.
