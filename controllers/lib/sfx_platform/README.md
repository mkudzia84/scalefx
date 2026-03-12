# sfx_platform — Platform Foundation Layer

Cross-platform abstraction, stateless wire encoding, and diagnostic logging. This is the root dependency for all other sfx_* libraries — it has zero sfx_* dependencies itself.

**Used by:** sfx_serial, sfx_server, sfx_audio, sfx_storage, sfx_usb, sfx_peripherals (all 6 libraries), plus controller firmware directly.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `sfx_platform.h` | ~268 | Platform abstraction (delays, heap, mutex, GPIO, servo, I2S detect, interrupts, dual-core) |
| `sfx_wire.h` | ~166 | Stateless wire encoding API (CRC-8, COBS, packet build/parse, endian helpers) |
| `sfx_wire.cpp` | ~170 | Wire encoding implementation |
| `diag_log.h` | ~335 | DiagLog singleton — ring-buffered diagnostic logging over COBS serial |
| `diag_log.cpp` | ~106 | DiagLog implementation (logv, ingest, sendHistory) |

## Architecture

```
  sfx_platform (foundation — zero sfx_* dependencies)
 ┌────────────────────────────────────────────────────────┐
 │                                                        │
 │  sfx_platform.h               SfxWire (sfx_wire.h)    │
 │  ┌──────────────────────┐     ┌──────────────────────┐ │
 │  │ SFX_DELAY_MS/US()    │     │ crc8()               │ │
 │  │ SFX_FREE_HEAP()      │     │ cobsEncode/Decode()  │ │
 │  │ SFX_REBOOT()         │     │ buildPacket()        │ │
 │  │ SFX_CPU_MHZ()        │     │ encodePacket()       │ │
 │  │ SfxMutex + lock/etc  │     │ parsePacket()        │ │
 │  │ sfxGetBoardId()      │     │ putU16LE/getU16LE()  │ │
 │  │ SFX_ATTACH_INTERRUPT │     │ putU32LE/getU32LE()  │ │
 │  │ SFX_DMA_BUFFER       │     │ putI16LE/getI16LE()  │ │
 │  │ SFX_PLATFORM_NAME    │     │                      │ │
 │  └──────────────────────┘     └──────────────────────┘ │
 │                                                        │
 │  DiagLog (diag_log.h)                                  │
 │  ┌──────────────────────────────────────────────┐      │
 │  │ SFX_LOG_INFO/WARN/ERROR/DEBUG macros         │      │
 │  │ Ring buffer (128 entries × 128 bytes)         │      │
 │  │ Mutex-protected writes, lock-free reads       │      │
 │  │ COBS LOG_MESSAGE (0xFD) packets               │      │
 │  │ Compile-time strippable: SFX_ENABLE_DIAG_LOG  │      │
 │  └──────────────────────────────────────────────┘      │
 └────────────────────────────────────────────────────────┘
              │
    ┌─────────┼─────────────────────────────┐
    │         │                             │
 sfx_serial  sfx_server  sfx_audio  sfx_storage
 sfx_peripherals         sfx_usb
```

## sfx_platform.h — Platform Abstraction

Unified macros and types for RP2040/RP2350 (Pico SDK) and ESP32-S3 (ESP-IDF).

### Platform Detection

| Macro | When Defined | Platform |
|-------|-------------|----------|
| `SFX_PLATFORM_PICO` | `ARDUINO_ARCH_RP2040` | RP2040 + RP2350 (Arduino-Pico) |
| `SFX_PLATFORM_ESP32` | `ARDUINO_ARCH_ESP32` | ESP32-S3 (ESP-IDF Arduino) |

### Abstraction Table

| Section | Pico API | ESP32 API | Macro/Function |
|---------|----------|-----------|----------------|
| **Timing** | `busy_wait_ms()` | `vTaskDelay()` | `SFX_DELAY_MS()` |
| | `busy_wait_us_32()` | `esp_rom_delay_us()` | `SFX_DELAY_US()` |
| **System** | `rp2040.getFreeHeap()` | `esp_get_free_heap_size()` | `SFX_FREE_HEAP()` |
| | `rp2040.reboot()` | `esp_restart()` | `SFX_REBOOT()` |
| | `F_CPU / 1000000` | `getCpuFrequencyMhz()` | `SFX_CPU_MHZ()` |
| **Board ID** | `pico_get_unique_board_id()` | `esp_efuse_mac_get_default()` | `sfxGetBoardId()` |
| **Bootloader** | `rp2040.rebootToBootloader()` | `esp_restart()` (no BOOTSEL) | `sfxRebootToBootloader()` |
| **Mutex** | `mutex_t` | `SemaphoreHandle_t` wrapper | `SfxMutex` + `sfxMutexInit/Lock/TryLock/Unlock` |
| **Servo** | `<Servo.h>` (PIO) | `<ESP32Servo.h>` (LEDC) | Auto-selected include |
| **Interrupt** | `attachInterruptParam()` | `attachInterruptArg()` | `SFX_ATTACH_INTERRUPT_PARAM()` |
| **I2S** | PIO-based | Hardware I2S | `SFX_I2S_PICO` / `SFX_I2S_ESP32` (detect only) |
| **Memory** | No-op | `DMA_ATTR` / `IRAM_ATTR` | `SFX_DMA_BUFFER` / `SFX_IRAM_FUNC` |
| **Dual-core** | `setup1()`/`loop1()` | `xTaskCreatePinnedToCore()` | `SFX_DUAL_CORE_PICO` / `SFX_DUAL_CORE_FREERTOS` |

### Usage

```cpp
#include "platform/sfx_platform.h"

SFX_DELAY_MS(10);
uint32_t heap = SFX_FREE_HEAP();
SFX_REBOOT();

SfxMutex mtx;
sfxMutexInit(mtx);
sfxMutexLock(mtx);
// ... critical section ...
sfxMutexUnlock(mtx);
```

## SfxWire — Wire Encoding

Stateless namespace with pure functions for the binary COBS protocol. Has **no** platform dependencies (`<stdint.h>`, `<stddef.h>`, `<cstring>` only).

**Design rationale:** Wire encoding lives here instead of sfx_serial to break a circular dependency — DiagLog needs COBS encoding for log packets, but sfx_serial depends on sfx_platform. `CoreProtocol` in `core/core.h` re-exports everything via `using SfxWire::*` for backward compatibility.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `HEADER_SIZE` | 4 | type(1) + tag(1) + len(2) |
| `MAX_PAYLOAD_SIZE` | 512 | Maximum payload bytes |
| `MAX_PACKET_SIZE` | 517 | HEADER + payload + CRC |
| `COBS_BUFFER_SIZE` | ~634 | Worst-case COBS output + delimiter |
| `FRAME_DELIMITER` | 0x00 | COBS frame boundary |
| `TAG_ASYNC` | 0x00 | Unsolicited packet tag |

### Functions

| Function | Purpose |
|----------|---------|
| `crc8(data, len)` | CRC-8 checksum (polynomial 0x07) |
| `cobsEncode(in, len, out)` | COBS encode data |
| `cobsDecode(in, len, out, max)` | COBS decode data |
| `buildPacket(out, type, tag, payload, len)` | Build raw packet with CRC |
| `encodePacket(out, type, tag, payload, len)` | Build + COBS encode + delimiter |
| `parsePacket(in, len, &type, &tag, &payload, &len)` | Parse + verify decoded packet |
| `putU16LE/getU16LE` | 16-bit little-endian helpers |
| `putI16LE/getI16LE` | Signed 16-bit little-endian helpers |
| `putU32LE/getU32LE` | 32-bit little-endian helpers |

## DiagLog — Diagnostic Logging

Singleton with ring buffer, mutex-protected, dual-core safe. Sends log messages as COBS-encoded `LOG_MESSAGE` (0xFD) packets.

### Features

- **Ring buffer:** 128 entries × 128 bytes per message (rolling — oldest overwritten, never blocks)
- **Thread-safe:** `SfxMutex` guards format+enqueue; `std::atomic` for cross-core indices
- **Lock-free reads:** `sendHistory()` snapshots indices without mutex
- **Compile-time strippable:** `SFX_ENABLE_DIAG_LOG=0` → zero-overhead stub
- **Log relay:** `ingest()` method for HubFX slave log forwarding (re-timestamped)
- **Initialized by SfxServer:** `DiagLog::instance().begin(&Serial)` — logging before init is silently discarded

### Log Levels

| Level | Value | Macro | Use |
|-------|-------|-------|-----|
| DEBUG | 0 | `SFX_LOG_DEBUG()` | Verbose tracing (state transitions, polling) |
| INFO | 1 | `SFX_LOG_INFO()` | Operational events (init, config loaded) |
| WARN | 2 | `SFX_LOG_WARN()` | Recoverable issues (SD not found, retry) |
| ERROR | 3 | `SFX_LOG_ERROR()` | Failures (hardware init failed) |

### Wire Format

```
LOG_MESSAGE (0xFD):  [level:u8][millis:u32LE][message:str]
```

### Usage

```cpp
// Via macros (preferred — compile to nothing when stripped)
SFX_LOG_INFO("Initialized at %lu MHz", F_CPU / 1000000UL);
SFX_LOG_WARN("Retry %d of %d", attempt, max);
SFX_LOG_ERROR("Hardware init failed");

// Via singleton directly
DiagLog::instance().info("Custom message");

// Set minimum level (default: INFO)
DiagLog::instance().setMinLevel(DiagLevel::DEBUG);
```

### Compile-Time Stripping

In `platformio.ini`:
```ini
build_flags = -DSFX_ENABLE_DIAG_LOG=0
```

When stripped, the stub class provides the same API surface with zero-overhead empty methods. `SFX_LOG_*` macros compile to `((void)0)`.

## Cross-Platform Notes

- **Shared library code** (`controllers/lib/`) MUST use `sfx_platform.h` abstractions — never raw Pico SDK or ESP-IDF calls
- **Controller firmware** (single-platform) may use platform-native APIs directly
- **`millis()` is safe everywhere** — not abstracted (Pico: timer register, ESP32: `gettimeofday` wrapper)
- **New abstractions** go in `sfx_platform.h` — do not scatter `#ifdef` blocks across component files

## Dependencies

```
sfx_platform
├── Arduino.h (framework)
├── <atomic> (std::atomic for cross-core safety)
├── Platform-specific SDK headers (pico/time.h, freertos/*, esp_system.h, etc.)
└── No sfx_* dependencies (this is the root)
```
