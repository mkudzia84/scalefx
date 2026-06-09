# sfx_platform — Platform Abstraction Foundation

Cross-platform OS/SDK abstraction layer. Zero `sfx_*` dependencies — this
is the bottom of the dependency stack.

**Used by:** every other library (`sfx_serial`, `sfx_board`, `sfx_audio`,
`sfx_storage`, `sfx_usb`, `sfx_peripherals`, `sfx_config`) plus
controller firmware directly.

## Files

| File                       | Purpose |
|----------------------------|---------|
| `platform/sfx_platform.h`  | Platform-detection macros, timing, heap, mutex, GPIO, servo, interrupts, dual-core entry points |
| `platform/spsc_ring_buffer.h` | Lock-free single-producer / single-consumer ring buffer (audio + diagnostics use it) |

> **Note:** Wire encoding (`SfxWire`) and DiagLog have moved into
> [`sfx_serial`](../sfx_serial/) — they're protocol-aware and belong
> with the wire-protocol library, not here.

## Platform detection

| Macro                 | When defined | Platform |
|-----------------------|--------------|----------|
| `SFX_PLATFORM_PICO`   | `ARDUINO_ARCH_RP2040` | RP2040 + RP2350 (Arduino-Pico) |
| `SFX_PLATFORM_ESP32`  | `ESP_PLATFORM` (or `ARDUINO_ARCH_ESP32`) | ESP32-S3 (pure ESP-IDF — HubFX is `framework = espidf`, no Arduino) |

## Abstractions

| Concern | Pico (RP2040/RP2350) | ESP32-S3 | Macro / function |
|---------|----------------------|----------|------------------|
| **Delay (ms)** | `busy_wait_ms` | `vTaskDelay(pdMS_TO_TICKS(...))` | `SFX_DELAY_MS(n)` |
| **Delay (µs)** | `busy_wait_us_32` | `esp_rom_delay_us` | `SFX_DELAY_US(n)` |
| **Free heap** | `rp2040.getFreeHeap()` | `esp_get_free_heap_size()` | `SFX_FREE_HEAP()` |
| **Reboot** | `rp2040.reboot()` | `esp_restart()` | `SFX_REBOOT()` |
| **CPU MHz** | `F_CPU / 1000000` | `getCpuFrequencyMhz()` | `SFX_CPU_MHZ()` |
| **Board ID** | `pico_get_unique_board_id` | `esp_efuse_mac_get_default` | `sfxGetBoardId(out, max)` |
| **Bootloader** | `rp2040.rebootToBootloader()` | n/a (NACK NOT_SUPPORTED) | `sfxRebootToBootloader()` |
| **Mutex** | `mutex_t` | `SemaphoreHandle_t` | `SfxMutex` + `sfxMutexInit/Lock/Unlock` |
| **Servo** | `<Servo.h>` (PIO) | native MCPWM `EspServo` (ESP32Servo dropped) | platform-selected driver |
| **Interrupt (arg)** | `attachInterruptParam` | `attachInterruptArg` | `SFX_ATTACH_INTERRUPT_PARAM` |
| **DMA buffer attr** | no-op | `DMA_ATTR` | `SFX_DMA_BUFFER` |
| **IRAM function** | no-op | `IRAM_ATTR` | `SFX_IRAM_FUNC` |
| **Dual-core entry** | `setup1` / `loop1` | `xTaskCreatePinnedToCore` | `SFX_DUAL_CORE_PICO` / `SFX_DUAL_CORE_FREERTOS` |
| **Platform name** | `"RP2040"` / `"RP2350"` | `"ESP32-S3"` | `SFX_PLATFORM_NAME` |

## SPSC ring buffer

`controllers/lib/sfx_platform/platform/spsc_ring_buffer.h` —
single-producer / single-consumer lock-free ring. Producer and consumer
indices are `std::atomic<size_t>` with explicit memory order
(`release`/`acquire` cross-core, `relaxed` for same-core counters). Used
by the audio mixer for I²S DMA feed and by anywhere else a one-way
producer-consumer queue beats locking.

## Cross-platform discipline (Rule 16)

- **Shared `controllers/lib/` code** uses `sfx_platform.h` abstractions
  exclusively — never raw Pico SDK or ESP-IDF calls. `delay()` /
  `sleep_ms()` / `delayMicroseconds()` are banned.
- **Controller firmware** (single-platform) is free to call its native
  SDK directly when it's clearer than the abstraction.
- **New abstractions** go in `sfx_platform.h` — don't scatter `#ifdef`
  blocks across other files.
- **Timestamps** use `SFX_MILLIS()` / `SFX_MICROS()` — native monotonic
  since-boot wrappers (`to_ms_since_boot`/`time_us_32` on Pico,
  `esp_timer_get_time()` on ESP32). Arduino `millis()` / `micros()` are
  Pico-only (HubFX is pure ESP-IDF, no Arduino), so shared `controllers/lib/`
  code MUST use the `SFX_*` macros, never bare `millis()`.

## Dependencies

```
sfx_platform
├── <Arduino.h>  (Pico only — Arduino-Pico framework; HubFX/ESP32 is pure ESP-IDF)
├── <atomic>     (std::atomic for cross-core indices)
├── Pico SDK headers (pico/time.h, pico/mutex.h, …) on Pico
└── ESP-IDF headers (freertos/*, esp_system.h, esp_timer.h, …) on ESP32-S3
```

Zero `sfx_*` dependencies — this is the root of the lib tree.
