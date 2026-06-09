# ScaleFX Libraries

Shared C++ libraries for ScaleFX microcontroller firmware.

> System-wide composition / dispatch / role-transport diagrams live in
> [instructions/32-ARCHITECTURE-DIAGRAMS.md](../../instructions/32-ARCHITECTURE-DIAGRAMS.md).

## Libraries

| Library | Purpose | Direct deps |
|---------|---------|-------------|
| [sfx_platform](sfx_platform/) | OS/SDK abstraction (`SfxMutex`, `SFX_DELAY_MS`, `SFX_FREE_HEAP`, …) + SPSC ring | ESP-IDF (ESP32) / Arduino (Pico) |
| [sfx_serial](sfx_serial/) | Wire protocol (CRC-8, COBS), DiagLog, client side (`SerialBus`/`BusClient`/`ResultQueue`), generic-expander wire (`ComponentPacket`) | sfx_platform |
| [sfx_peripherals](sfx_peripherals/) | LED, servo, PWM, I²C, INA226, battery, indicator-LED + battery service policies | sfx_platform, sfx_serial, sfx_config |
| [sfx_board](sfx_board/) | Board-side framework — `BoardServer<...>` / `BoardOf<...>` composer + `BoardServicePolicy` + hub-local `PortServicePolicy` / `RoleServicePolicy` (+ role classes & handlers) + `StreamWriter` | sfx_platform, sfx_serial, sfx_peripherals, sfx_config |
| [sfx_audio](sfx_audio/) | 8-channel WAV mixer, I²S output, codec drivers + `AudioServicePolicy` | sfx_platform, sfx_serial, sfx_storage, sfx_board |
| [sfx_storage](sfx_storage/) | SD card (SdFat on Pico / ESP-IDF VFS-FAT on ESP32) + LittleFS flash singletons + `StorageServicePolicy` | sfx_platform, sfx_serial, sfx_board |
| [sfx_config](sfx_config/) | YAML parser + templatised `ConfigStore` + `ConfigServicePolicy` | sfx_platform, sfx_serial, sfx_board |
| [sfx_usb](sfx_usb/) | USB Host abstraction (Pico TinyUSB / ESP32 USB-OTG CDC-ACM) | sfx_platform |
| [esp_cdc_acm](esp_cdc_acm/) | ESP-IDF CDC-ACM host driver wrapper | sfx_platform |

**Supported platforms:** RP2040, RP2350, ESP32-S3

## Dependency graph

```
sfx_platform                        (foundation — zero sfx_* deps)
  ↓
sfx_serial                          (wire + client + generic-expander wire)
  ↓
sfx_peripherals                     (drivers + indicator/battery policies)
  ↓
sfx_board                           (BoardServer + policies + CoreClient)
  ↓
sfx_audio  sfx_storage  sfx_config  (subsystem drivers + their service policies)

sfx_usb / esp_cdc_acm               (depend only on sfx_platform)
```

## Key include paths

| Include | Library | Description |
|---------|---------|-------------|
| `platform/sfx_platform.h` | sfx_platform | OS/SDK abstraction (mutexes, delays, heap, reboot, board ID) |
| `platform/spsc_ring_buffer.h` | sfx_platform | Lock-free single-producer/single-consumer ring |
| `serial/wire.h` | sfx_serial | Stateless wire encoding (CRC-8, COBS, endian helpers) — `SfxWire::` |
| `serial/diag_log.h` | sfx_serial | DiagLog ring-buffered logging singleton |
| `serial/serial.h` | sfx_serial | Umbrella (core + client + components) |
| `serial/core/core.h` | sfx_serial | `CorePacket`, `SerialError`, `CommandResult`, `CommandHandleResult`, SFX_* macros |
| `serial/components/components.h` | sfx_serial | Generic-expander wire protocol (`ComponentPacket`) |
| `client/bus_client.h` | sfx_serial | `BusClient` — master-side typed command/query API |
| `server/board_server.h` | sfx_board | `BoardServer<...UserPolicies>` + `BoardServerBase` |
| `server/board_of.h` | sfx_board | `BoardOf<TBoard, TStream, Caps, ...>` — composer that auto-adds Port + Role services (HubFX form) |
| `server/board_service.h` | sfx_board | `BoardServicePolicy` (lifecycle / IDENTIFY / STATUS / I2C_SCAN) |
| `server/port_service.h` | sfx_board | `PortServicePolicy` (hub-local port enumeration) |
| `server/role_service.h` | sfx_board | `RoleServicePolicy` (per-port role attach / drive / query dispatch) |
| `server/stream.h` | sfx_board | `StreamWriter` (chunked + CRC-16) |
| `audio/audio_mixer.h` | sfx_audio | 8-channel WAV mixer |
| `server/audio_service.h` | sfx_audio | `AudioServicePolicy<TMixer>` |
| `storage/sd_card.h` | sfx_storage | SD card singleton |
| `storage/flash.h` | sfx_storage | LittleFS flash singleton |
| `server/storage_service.h` | sfx_storage | `StorageServicePolicy<TPolicy>` (+ `storage_upload_engine.h` `UploadEngine<TPolicy>`, `storage_path_util.h`) |
| `config/yaml_parser.h` | sfx_config | Lightweight YAML subset parser |
| `config/config_store.h` | sfx_config | Templatised schema-driven config manager |
| `server/multi_config_server.h` | sfx_config | `ConfigServicePolicy` (path-routed multi-store) |
| `usb/sfx_usb_host.h` | sfx_usb | Abstract USB Host interface |
| `pwm/pwm_output.h` | sfx_peripherals | PwmOutput concept — pin-level PWM |
| `gpio/native_gpio.h` | sfx_peripherals | MCU GPIO wrapper (PwmOutput backend via `analogWrite`) |
| `pwm/pca9685.h` | sfx_peripherals | PCA9685 16-channel 12-bit I²C PWM (PwmOutput backend) |
| `led/led_control.h` | sfx_peripherals | Single-channel LED controller |
| `led/led_event_seq.h` | sfx_peripherals | Looping LED event sequence player |
| `servo/srv_control.h` | sfx_peripherals | Servo motion profiling |
| `power/ina226.h` | sfx_peripherals | INA226 power monitor |
| `power/battery_server.h` | sfx_peripherals | `BatteryServicePolicy<TBattery>` |
| `indicators/indicator_leds.h` | sfx_peripherals | `IndicatorServicePolicy` (connection + error LEDs) |

## PlatformIO configuration

All controllers use auto-discovery:

```ini
lib_extra_dirs = ../../lib
```

PlatformIO's LDF resolves dependencies based on `#include` directives in
firmware source plus the explicit `dependencies` list in each
`library.json`.
