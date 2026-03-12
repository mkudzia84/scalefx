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

#include <Arduino.h>
#include <atomic>

// ============================================================================
//  PLATFORM DETECTION
// ============================================================================

#if defined(ARDUINO_ARCH_RP2040)
    // Covers both RP2040 and RP2350 (Arduino-Pico framework by Earle Philhower)
    #define SFX_PLATFORM_PICO       1
    #define SFX_PLATFORM_ESP32      0
#elif defined(ARDUINO_ARCH_ESP32)
    // ESP32-S3 (ESP-IDF Arduino framework)
    #define SFX_PLATFORM_PICO       0
    #define SFX_PLATFORM_ESP32      1
#else
    #error "Unsupported platform — add native API mappings for your target"
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
//  millis() is safe on ALL platforms (Rule 16) and not abstracted here.
//  These macros replace platform-specific blocking delays.

#if SFX_PLATFORM_PICO
    #include <pico/time.h>
    #define SFX_DELAY_MS(ms)        busy_wait_ms(ms)
    #define SFX_DELAY_US(us)        busy_wait_us_32(us)
#elif SFX_PLATFORM_ESP32
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>
    #define SFX_DELAY_MS(ms)        vTaskDelay(pdMS_TO_TICKS(ms))
    #define SFX_DELAY_US(us)        esp_rom_delay_us(us)
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

    #define SFX_FREE_HEAP()         esp_get_free_heap_size()
    #define SFX_REBOOT()            esp_restart()
    #define SFX_CPU_MHZ()           getCpuFrequencyMhz()  // Arduino-ESP32 API (portable across IDF versions)

    // Board unique ID — MAC address on ESP32
    inline void sfxGetBoardId(char* out, size_t maxLen) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(out, maxLen, "%02X%02X%02X%02X",
                 mac[2], mac[3], mac[4], mac[5]);
    }

    // No BOOTSEL equivalent on ESP32 — OTA or USB-DFU instead
    inline void sfxRebootToBootloader() {
        // ESP32-S3 USB-DFU: Not directly available, perform normal restart
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
    inline void sfxMutexLock(SfxMutex& mtx)           { xSemaphoreTake(mtx.handle, portMAX_DELAY); }
    inline bool sfxMutexTryLock(SfxMutex& mtx)        { return xSemaphoreTake(mtx.handle, 0) == pdTRUE; }
    inline void sfxMutexUnlock(SfxMutex& mtx)         { xSemaphoreGive(mtx.handle); }
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
//  SERVO — Platform-Specific Include
// ============================================================================
//
//  Arduino-Pico uses <Servo.h> (PIO-based).
//  ESP32 uses <ESP32Servo.h> (LEDC-based). API is compatible.

#if SFX_PLATFORM_PICO
    #include <Servo.h>
#elif SFX_PLATFORM_ESP32
    #include <ESP32Servo.h>
#endif

// ============================================================================
//  INTERRUPT — Portable attachInterrupt with Parameter
// ============================================================================
//
//  Arduino-Pico: attachInterruptParam(pin, isr, mode, param)
//  ESP32:        attachInterruptArg(pin, isr, arg, mode)  ← different arg order!

#if SFX_PLATFORM_PICO
    #define SFX_ATTACH_INTERRUPT_PARAM(pin, isr, mode, param) \
        attachInterruptParam(digitalPinToInterrupt(pin), isr, mode, param)
#elif SFX_PLATFORM_ESP32
    #define SFX_ATTACH_INTERRUPT_PARAM(pin, isr, mode, param) \
        attachInterruptArg(digitalPinToInterrupt(pin), isr, param, mode)
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
