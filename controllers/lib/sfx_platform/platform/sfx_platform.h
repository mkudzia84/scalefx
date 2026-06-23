/**
 * ScaleFX Platform Abstraction Layer
 *
 * Provides unified macros and types for cross-platform compatibility
 * between RP2040 (Pico), RP2350 (Pico 2), and ESP32-S3.
 *
 * Usage:
 *   #include "platform/sfx_platform.h"
 *   SFX_DELAY_MS(10);
 *   uint32_t heap = SFX_FREE_HEAP();
 *   SFX_REBOOT();
 *
 * Platform detection:
 *   ARDUINO_ARCH_RP2040   — RP2040 and RP2350 (Arduino-Pico framework)
 *   PICO_RP2350           — RP2350 only (Pico 2)
 *   ARDUINO_ARCH_ESP32    — ESP32-S3 (ESP-IDF Arduino)
 *
 * See copilot-instructions.md Rule 16 for full platform-native API table.
 */

#ifndef SFX_PLATFORM_H
#define SFX_PLATFORM_H

#include <atomic>

// ============================================================================
//  PLATFORM DETECTION
// ============================================================================

#if defined(ARDUINO_ARCH_RP2040)
    // Covers both RP2040 and RP2350 (Arduino-Pico framework by Earle Philhower)
    #define SFX_PLATFORM_PICO       1
    #define SFX_PLATFORM_ESP32      0
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
    // ESP32-S3 — `ESP_PLATFORM` is defined by ESP-IDF on BOTH the pure-espidf
    // build AND the Arduino-ESP32 (IDF-component) build; `ARDUINO_ARCH_ESP32`
    // only on the latter.  Checking ESP_PLATFORM is what lets the firmware run
    // native espidf with no Arduino.
    #define SFX_PLATFORM_PICO       0
    #define SFX_PLATFORM_ESP32      1
#else
    #error "Unsupported platform — add native API mappings for your target"
#endif

// Arduino is only pulled on the Pico path (it still rides the Arduino-Pico
// framework, and the GPIO-ISR interrupt macro below maps to attachInterrupt).
// On ESP32 the firmware is native (ESP-IDF) — I2C/GPIO/servo/UART/Stream/timing
// all have native abstractions — so sfx_platform.h no longer leaks <Arduino.h>
// transitively to every includer; each file declares its own real deps.
#if SFX_PLATFORM_PICO
#include <Arduino.h>
#endif

// ============================================================================
//  PLATFORM STRING
// ============================================================================

#if SFX_PLATFORM_PICO
    #if defined(PICO_RP2350)
        #define SFX_PLATFORM_NAME   "RP2350"
    #else
        #define SFX_PLATFORM_NAME   "RP2040"
    #endif
#elif SFX_PLATFORM_ESP32
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        #define SFX_PLATFORM_NAME   "ESP32-S3"
    #else
        #define SFX_PLATFORM_NAME   "ESP32"
    #endif
#endif

// ============================================================================
//  TIMING — Delays and Timestamps
// ============================================================================
//
//  SFX_MILLIS()/SFX_MICROS() are the NATIVE monotonic-since-boot timestamps
//  (no Arduino millis()/micros()) — numerically identical to the Arduino calls
//  they replace, so they are a drop-in.  Use these instead of millis()/micros()
//  in shared lib/ code (Rule 16 + arduino-removal).  These macros also replace
//  platform-specific blocking delays.

#if SFX_PLATFORM_PICO
    #include <pico/time.h>
    // NOTE: SFX_DELAY_MS on Pico is a HARD SPIN (busy_wait_ms) — it pins the
    // core for the whole duration, it does NOT yield to interrupts or let the
    // SDK lower-power-wait.  This is intentional for short, precise delays.  For
    // a poll-loop / cooperative delay prefer sleep_ms() (interrupt-friendly,
    // lower power) or tight_loop_contents() instead of SFX_DELAY_MS.
    #define SFX_DELAY_MS(ms)        busy_wait_ms(ms)
    #define SFX_DELAY_US(us)        busy_wait_us_32(us)
    #define SFX_MILLIS()            to_ms_since_boot(get_absolute_time())
    #define SFX_MICROS()            time_us_32()
#elif SFX_PLATFORM_ESP32
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>
    #include <esp_timer.h>
    #define SFX_DELAY_MS(ms)        vTaskDelay(pdMS_TO_TICKS(ms))
    #define SFX_DELAY_US(us)        esp_rom_delay_us(us)
    #define SFX_MILLIS()            ((uint32_t)(esp_timer_get_time() / 1000))
    #define SFX_MICROS()            ((uint32_t)esp_timer_get_time())
#endif

// ============================================================================
//  SYSTEM — Heap, Reboot, Board ID
// ============================================================================

#if SFX_PLATFORM_PICO
    #include <pico/unique_id.h>

    #define SFX_FREE_HEAP()         rp2040.getFreeHeap()
    #define SFX_REBOOT()            rp2040.reboot()
    #define SFX_CPU_MHZ()           (F_CPU / 1000000)

    // Board unique ID — 8-byte flash ID on Pico
    inline void sfxGetBoardId(char* out, size_t maxLen) {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        snprintf(out, maxLen, "%02X%02X%02X%02X",
                 id.id[4], id.id[5], id.id[6], id.id[7]);
    }

    // Bootloader mode (BOOTSEL)
    inline void sfxRebootToBootloader() {
        rp2040.rebootToBootloader();
    }

#elif SFX_PLATFORM_ESP32
    #include <esp_system.h>
    #include <esp_mac.h>
    #include <esp_timer.h>
    #include <esp_clk_tree.h>

    #define SFX_FREE_HEAP()         esp_get_free_heap_size()
    #define SFX_REBOOT()            esp_restart()
    // Native ESP-IDF CPU frequency (was Arduino getCpuFrequencyMhz()).
    static inline uint32_t sfxCpuMhz() {
        uint32_t hz = 0;
        // On query failure fall back to the configured default rather than
        // emitting a bogus 0 MHz into STATUS / diagnostics.
        if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz) != ESP_OK || hz == 0) {
            return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        }
        return hz / 1000000u;
    }
    #define SFX_CPU_MHZ()           sfxCpuMhz()

    // Board unique ID — MAC address on ESP32
    inline void sfxGetBoardId(char* out, size_t maxLen) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(out, maxLen, "%02X%02X%02X%02X",
                 mac[2], mac[3], mac[4], mac[5]);
    }

    // No BOOTSEL equivalent on ESP32 — OTA or esptool instead.
    // SfxServer does NOT register a BOOTSEL callback on ESP32;
    // BoardServicePolicy sends NACK NOT_SUPPORTED automatically.
    inline void sfxRebootToBootloader() {
        // Fallback: plain restart (no UF2 bootloader on ESP32)
        esp_restart();
    }
#endif

// ============================================================================
//  MUTEX — Cross-Platform Mutex Abstraction
// ============================================================================

#if SFX_PLATFORM_PICO
    #include <pico/mutex.h>

    using SfxMutex = mutex_t;

    inline void sfxMutexInit(SfxMutex& mtx)         { mutex_init(&mtx); }
    inline void sfxMutexLock(SfxMutex& mtx)          { mutex_enter_blocking(&mtx); }
    inline bool sfxMutexTryLock(SfxMutex& mtx)       { return mutex_try_enter(&mtx, nullptr); }
    inline void sfxMutexUnlock(SfxMutex& mtx)        { mutex_exit(&mtx); }

#elif SFX_PLATFORM_ESP32
    #include <freertos/semphr.h>

    // ESP32 uses FreeRTOS semaphore as mutex
    struct SfxMutex {
        SemaphoreHandle_t handle = nullptr;
    };

    inline void sfxMutexInit(SfxMutex& mtx)          { mtx.handle = xSemaphoreCreateMutex(); }
    // Each op guards against a NULL handle (xSemaphoreCreateMutex() returns NULL
    // only on boot-time heap exhaustion): without the guard the FreeRTOS macros
    // dereference a NULL handle → UB.  A NULL mutex degrades to a no-op lock
    // (always "acquired") rather than faulting — loud-but-safe under OOM.
    inline void sfxMutexLock(SfxMutex& mtx)           { if (mtx.handle) xSemaphoreTake(mtx.handle, portMAX_DELAY); }
    inline bool sfxMutexTryLock(SfxMutex& mtx)        { return mtx.handle ? (xSemaphoreTake(mtx.handle, 0) == pdTRUE) : true; }
    inline void sfxMutexUnlock(SfxMutex& mtx)         { if (mtx.handle) xSemaphoreGive(mtx.handle); }
#endif

// ============================================================================
//  GPIO — Platform-Specific Pin Constants
// ============================================================================

#if SFX_PLATFORM_PICO
    // Pico: GPIO24 is VBUS detect (USB power present)
    constexpr uint8_t SFX_VBUS_PIN = 24;
#elif SFX_PLATFORM_ESP32
    // ESP32-S3: No dedicated VBUS pin — use an ADC pin or USB peripheral status
    // Set to 0xFF to indicate "not available"
    constexpr uint8_t SFX_VBUS_PIN = 0xFF;
#endif

// ============================================================================
//  SERVO — see ports/sfx_servo.h
// ============================================================================
//
//  The servo backend (ESP32 MCPWM / Pico PIO) is selected in
//  sfx_peripherals/ports/sfx_servo.h, NOT here — the platform header no longer
//  pulls a servo library (ESP32Servo dropped in the Arduino-removal work).

// ============================================================================
//  INTERRUPT — Portable attachInterrupt with Parameter
// ============================================================================
//
//  Arduino-Pico: attachInterruptParam(pin, isr, mode, param)
//  ESP32:        attachInterruptArg(pin, isr, arg, mode)  ← different arg order!

//  Pico-only: the single user is pico_isr_ppm_policy (ESP32 captures PPM via
//  the native RMT peripheral, no GPIO ISR). Gated to Pico so the macro body
//  never names Arduino's attachInterrupt on the (native) ESP32 build.
#if SFX_PLATFORM_PICO
    #define SFX_ATTACH_INTERRUPT_PARAM(pin, isr, mode, param) \
        attachInterruptParam(digitalPinToInterrupt(pin), isr, mode, param)
#endif

// ============================================================================
//  I2S — Platform-Specific I2S Include and Types
// ============================================================================
//
//  Arduino-Pico: <I2S.h> from earlephilhower core (PIO-based)
//  ESP32-S3:     <driver/i2s_std.h> from ESP-IDF (hardware I2S peripheral)

// I2S abstraction is handled in audio/audio_i2s.h due to complexity.
// This section provides only the detection macro.

#if SFX_PLATFORM_PICO
    #define SFX_I2S_PICO    1
    #define SFX_I2S_ESP32   0
#elif SFX_PLATFORM_ESP32
    #define SFX_I2S_PICO    0
    #define SFX_I2S_ESP32   1
#endif

// ============================================================================
//  MEMORY — ESP32-S3 SRAM DMA Buffer Attributes
// ============================================================================
//
//  ESP32-S3 has internal SRAM that is faster for DMA operations.
//  IRAM_ATTR places code in instruction RAM for ISR performance.
//  DMA_ATTR aligns buffers for DMA controller requirements.

#if SFX_PLATFORM_ESP32
    // Already defined in ESP-IDF framework
    // DMA_ATTR — 4-byte aligned for DMA
    // IRAM_ATTR — place in instruction RAM for ISR
    // DRAM_ATTR — place in data RAM (internal SRAM)
    #ifndef DMA_ATTR
        #define DMA_ATTR __attribute__((aligned(4)))
    #endif
    // SFX_DMA_BUFFER — allocate DMA-capable buffer in internal SRAM
    #define SFX_DMA_BUFFER      DMA_ATTR DRAM_ATTR
    // SFX_IRAM_FUNC — place function in IRAM for faster ISR execution
    #define SFX_IRAM_FUNC       IRAM_ATTR
#else
    // Pico: No special DMA alignment needed, all SRAM is DMA-capable
    #define SFX_DMA_BUFFER
    #define SFX_IRAM_FUNC
    #ifndef IRAM_ATTR
        #define IRAM_ATTR
    #endif
#endif

// ============================================================================
//  PSRAM — External SPI RAM (ESP32-S3 OPI PSRAM)
// ============================================================================
//
//  ESP32-S3 N8R8 has 8 MB OPI PSRAM @ 80 MHz (~100 MB/s bandwidth).
//  Use for large audio buffers that don't need DMA alignment (ring buffers,
//  WAV decode buffers, SD read buffers). I2S DMA buffers stay in internal SRAM.
//
//  On Pico (no PSRAM), all functions fall back to standard heap allocation.
//  This keeps application code platform-agnostic.

#if SFX_PLATFORM_ESP32
    #include <esp_heap_caps.h>

    // Runtime PSRAM detection (true if PSRAM is initialized and usable).
    // Uses ESP-IDF heap API directly — Arduino's psramFound() can return false
    // even when PSRAM is working, because ESP-IDF initializes PSRAM via
    // sdkconfig (qio_opi memory_type) without setting Arduino's tracking flag.
    #define SFX_HAS_PSRAM       (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)

    /**
     * Allocate memory from PSRAM (external SPI RAM).
     * Returns nullptr on failure. Caller must free with sfxPsramFree().
     */
    inline void* sfxPsramMalloc(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    /**
     * Allocate zeroed memory from PSRAM.
     * Returns nullptr on failure. Caller must free with sfxPsramFree().
     */
    inline void* sfxPsramCalloc(size_t n, size_t size) {
        return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    }

    /**
     * Free memory allocated by sfxPsramMalloc/sfxPsramCalloc.
     * Safe to call with nullptr.
     */
    inline void sfxPsramFree(void* ptr) {
        if (ptr) heap_caps_free(ptr);
    }

    /** Total installed PSRAM in bytes */
    inline size_t sfxPsramTotal() {
        return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    }

    /** Free (unallocated) PSRAM in bytes */
    inline size_t sfxPsramFree_bytes() {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    /** Free (unallocated) internal DRAM in bytes.
     *  Excludes PSRAM (use sfxPsramFree_bytes for that).  Internal
     *  DRAM is the scarce resource on ESP32-S3 (~210 KB available
     *  after boot vs 8 MB PSRAM) so it's worth surfacing separately
     *  in STATUS / diagnostics. */
    inline size_t sfxDramFree_bytes() {
        return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    /** Total installed internal DRAM in bytes. */
    inline size_t sfxDramTotal() {
        return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

#else
    // No PSRAM on Pico — fall back to standard heap
    #define SFX_HAS_PSRAM       0

    inline void* sfxPsramMalloc(size_t size)          { return malloc(size); }
    inline void* sfxPsramCalloc(size_t n, size_t size) { return calloc(n, size); }
    inline void sfxPsramFree(void* ptr)                { free(ptr); }
    inline size_t sfxPsramTotal()                      { return 0; }
    inline size_t sfxPsramFree_bytes()                 { return 0; }

    /** Pico has no PSRAM split — DRAM helpers return the standard
     *  heap so callers don't need #ifdef'd dispatch. */
    inline size_t sfxDramFree_bytes()                  { return SFX_FREE_HEAP(); }
    inline size_t sfxDramTotal()                       { return 0; /* runtime total unknown on Pico */ }
#endif

// ============================================================================
//  FILESYSTEM — Platform-Specific Flash Filesystem
// ============================================================================
//
//  Arduino-Pico: LittleFS (via <LittleFS.h>)
//  ESP32-S3:     LittleFS (via <LittleFS.h>) or SPIFFS — both use FS.h base

// Both platforms support LittleFS with similar API through Arduino framework.
// The main difference is partition setup (ESP32 needs partition table config).

// ============================================================================
//  DUAL-CORE — Task/Core Management
// ============================================================================

#if SFX_PLATFORM_PICO
    // Arduino-Pico uses setup1()/loop1() for Core 1
    #define SFX_DUAL_CORE_PICO      1
    #define SFX_DUAL_CORE_FREERTOS  0
#elif SFX_PLATFORM_ESP32
    // ESP32 uses FreeRTOS xTaskCreatePinnedToCore()
    #define SFX_DUAL_CORE_PICO      0
    #define SFX_DUAL_CORE_FREERTOS  1
#endif

#endif // SFX_PLATFORM_H
