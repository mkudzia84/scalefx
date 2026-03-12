# ScaleFX Libraries

Shared libraries for ScaleFX microcontroller firmware.

## Library

| Library | Purpose | Dependencies |
|---------|---------|--------------|
| [components](components/) | Hardware drivers, serial protocol, platform abstraction, audio, storage | Arduino, Wire, Servo, SdFat |

All code lives in a single `components` library. The serial protocol stack, audio subsystem, storage drivers, and platform abstraction layer are organized as subdirectories within `components/`.

**Supported platforms:** RP2040, RP2350, ESP32-S3

## Domains

| Domain | Include Path | Description |
|--------|-------------|-------------|
| Platform | `platform/sfx_platform.h` | Cross-platform abstraction (mutexes, delays, GPIO, memory) |
| Audio | `audio/audio_mixer.h` | 8-channel WAV mixer, I2S output, codec drivers |
| LED | `led/led_control.h` | GPIO LED control with event-based animations |
| Power | `power/ina226.h` | I2C device framework, INA226, battery monitor |
| PWM | `pwm/pwm_control.h` | RC PWM input with averaging and hysteresis |
| Serial | `serial/serial.h` | Binary COBS protocol, command routing, clients |
| Server | `server/sfx_server.h` | Common server controller boilerplate |
| Servo | `servo/srv_control.h` | Servo motion profiling with jerk effects |
| Storage | `storage/sd_card.h` | SD card (SdFat), LittleFS flash singletons |
| Indicators | `indicators/indicator_leds.h` | Standardized status LEDs (GP13/GP14) |

See [components/README.md](components/README.md) for full API documentation.

See [components/serial/PROTOCOL.md](components/serial/PROTOCOL.md) for binary protocol specification.

## PlatformIO Configuration

All controllers use auto-discovery:
```ini
lib_extra_dirs = ../../lib
```

This makes the `components` library available to all controllers. PlatformIO resolves dependencies automatically based on `#include` directives in firmware source.

> **Note:** The `serial/` directory at this level contains legacy headers from before the serial library was folded into `components/serial/`. These files are no longer used by any controller and will be removed in a future cleanup.
