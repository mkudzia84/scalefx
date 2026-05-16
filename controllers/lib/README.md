# ScaleFX Libraries

Shared libraries for ScaleFX microcontroller firmware.

## Libraries

| Library | Version | Purpose | Dependencies |
|---------|---------|---------|--------------|
| [sfx_platform](sfx_platform/) | 2.0.0 | Platform abstraction, wire encoding, DiagLog | Arduino |
| [sfx_serial](sfx_serial/) | 3.0.0 | COBS protocol, command routing, module handlers | sfx_platform |
| [sfx_peripherals](sfx_peripherals/) | 1.0.0 | LED, servo, PWM, INA226, I2C drivers | sfx_platform |
| [sfx_server](sfx_server/) | 1.0.0 | Server controller boilerplate (SfxServer) | sfx_platform, sfx_serial, sfx_peripherals |
| [sfx_audio](sfx_audio/) | 2.0.0 | 8-channel WAV mixer, I2S, codec drivers | sfx_platform |
| [sfx_storage](sfx_storage/) | 1.0.0 | SD card (SdFat), LittleFS flash | sfx_platform |
| [sfx_config](sfx_config/) | 1.0.0 | YAML config parser, schema-driven config store | sfx_platform, sfx_serial |
| [sfx_usb](sfx_usb/) | 2.0.0 | USB Host abstraction (Pico/ESP32) | sfx_platform |

**Supported platforms:** RP2040, RP2350, ESP32-S3

## Module Documentation

| Library | README |
|---------|--------|
| sfx_peripherals | [Hardware Peripheral Drivers](sfx_peripherals/README.md) |
| sfx_serial | [Serial Protocol Library](sfx_serial/serial/README.md) |
| sfx_server | [Server Controller Boilerplate](sfx_server/README.md) |
| sfx_storage | [SD Card & Flash Storage](sfx_storage/README.md) |
| sfx_config | [Configuration Management](sfx_config/README.md) |
| sfx_usb | [USB Host Abstraction](sfx_usb/README.md) |

## Dependency Graph

```
sfx_platform          ← Foundation (zero external deps)
  ├── sfx_serial      ← Protocol layer
  ├── sfx_peripherals ← Hardware drivers
  ├── sfx_audio       ← Audio engine (SFX_HAS_AUDIO=1)
  ├── sfx_storage     ← Storage drivers (SFX_HAS_STORAGE=1)
  ├── sfx_config      ← Config management (sfx_platform + sfx_serial)
  └── sfx_usb         ← USB Host (client-only)

sfx_server            ← Server boilerplate
  ├── sfx_platform
  ├── sfx_serial
  └── sfx_peripherals
```

No circular dependencies. Each library depends only on sfx_platform (foundation) and optionally on sfx_serial/sfx_peripherals for the server layer.

## Key Include Paths

| Include | Library | Description |
|---------|---------|-------------|
| `platform/sfx_platform.h` | sfx_platform | Cross-platform abstraction (mutexes, delays, GPIO) |
| `platform/sfx_wire.h` | sfx_platform | Stateless wire encoding (CRC-8, COBS, endian) |
| `platform/diag_log.h` | sfx_platform | DiagLog diagnostic logging singleton |
| `serial/serial.h` | sfx_serial | Umbrella header for protocol stack |
| `serial/hubfx/hubfx.h` | sfx_serial | HubFX protocol (explicit include, not in umbrella) |
| `server/sfx_server.h` | sfx_server | SfxServer lifecycle + indicator LEDs |
| `audio/audio_mixer.h` | sfx_audio | 8-channel WAV mixer |
| `storage/sd_card.h` | sfx_storage | SD card singleton |
| `config/yaml_parser.h` | sfx_config | Lightweight YAML subset parser |
| `config/config_store.h` | sfx_config | Templatized schema-driven config manager |
| `server/config_server.h` | sfx_config | BusServer handler for CONFIG commands |
| `client/config_client.h` | sfx_config | BusClient for config commands |
| `usb/usb_host.h` | sfx_usb | Abstract USB Host interface |
| `pwm/pwm_output.h` | sfx_peripherals | PwmOutput concept — single duck-typed surface for pin-level PWM backends |
| `gpio/native_gpio.h` | sfx_peripherals | MCU GPIO wrapper (singleton, HW PWM via analogWrite). PwmOutput backend. |
| `pwm/pca9685.h` | sfx_peripherals | NXP PCA9685 — 16-channel 12-bit I²C PWM driver. PwmOutput backend. |
| `led/led_control.h` | sfx_peripherals | LedControlT\<TPwm\> single-channel LED controller |
| `led/led_manager.h` | sfx_peripherals | LedManager\<N, TPwm\> multi-channel LED manager |
| `led/led_events.h` | sfx_peripherals | LED animation events (On, Off, Flash, Fade, Beacon) |
| `led/led_event_seq.h` | sfx_peripherals | Looping LED event sequence player |
| `servo/srv_control.h` | sfx_peripherals | Servo motion profiling |
| `power/ina226.h` | sfx_peripherals | INA226 power monitor |

## PlatformIO Configuration

All controllers use auto-discovery:
```ini
lib_extra_dirs = ../../lib
```

PlatformIO's Library Dependency Finder (LDF) resolves dependencies automatically based on `#include` directives in firmware source.
