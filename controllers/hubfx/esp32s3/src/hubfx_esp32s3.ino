/*
 * HubFX ESP32-S3 — Autonomous Protocol Router Hub
 *
 * Central hub controller for ScaleFX system. Manages slave controllers
 * (GunFX, LightFX, GearControl) via USB Host, audio mixing, and effects.
 *
 * DUAL-CORE ARCHITECTURE (ESP32-S3, dual Xtensa LX7 @ 240 MHz):
 *   Core 0: Main loop — serial protocol, slave management, SD card reads,
 *           WAV decoding, audio mixing (producer)
 *   Core 1: Audio I2S output (consumer), USB Host polling
 *
 * COMMUNICATION:
 *   Upstream:  USB CDC Serial (1Mbps) — optional, config/debug only
 *   Downstream: USB Host — binary COBS to each slave controller
 *
 * This is a skeleton for migration from HubFX Pico (RP2350).
 * Code modules will be migrated individually from controllers/hubfx/pico/.
 *
 * Migration status:
 *   [ ] Audio mixer (I2S output via ESP-IDF driver)
 *   [ ] SD card storage
 *   [ ] Config reader
 *   [ ] Slave management (USB Host)
 *   [ ] Audio server (protocol handler)
 *   [ ] Engine server (protocol handler)
 *   [ ] Storage server (protocol handler)
 *   [ ] Engine FX
 *   [ ] Gun FX
 *   [ ] System sounds
 */

#define FIRMWARE_VERSION "0.1.0"
#define BUILD_NUMBER 1

#include <Arduino.h>
#include <atomic>

// Platform abstraction (ESP32-S3 specific macros, mutexes, delays)
#include <platform/sfx_platform.h>

// Shared serial protocol library
#include <serial/serial.h>
#include <server/sfx_server.h>

// ============================================================================
// Pin Definitions (ESP32-S3 DevKitC-1)
// ============================================================================

// I2S Audio Output
// TODO: Assign pins based on chosen DAC board
// #define PIN_I2S_DATA    ?
// #define PIN_I2S_BCLK    ?
// #define PIN_I2S_LRCLK   ?
// #define PIN_I2S_MCLK    ?    // ESP32-S3 supports MCLK (Pico does not)

// SD Card (SPI)
// TODO: Assign pins based on board wiring
// #define PIN_SD_CS       ?
// #define PIN_SD_SCK      ?
// #define PIN_SD_MOSI     ?
// #define PIN_SD_MISO     ?

// I2C (for codec control, power monitoring, etc.)
// #define PIN_I2C_SDA     ?
// #define PIN_I2C_SCL     ?

// ============================================================================
// Core 1 Task — Audio Consumer + USB Host
// ============================================================================

// FreeRTOS task handle for Core 1
static TaskHandle_t core1TaskHandle = nullptr;

// Cross-core state flags
std::atomic<bool> audioInitialized{false};   // Core 0 writes, Core 1 reads
std::atomic<bool> core1Ready{false};         // Core 1 writes, Core 0 reads

// Diagnostic: Core 1 loop iteration counter
std::atomic<uint32_t> loop1Count{0};         // Core 1 writes, Core 0 reads

/**
 * Core 1 task function — audio I2S consumer loop.
 *
 * On ESP32-S3, Core 1 is dedicated to low-latency audio output.
 * Uses FreeRTOS xTaskCreatePinnedToCore() instead of Pico's setup1()/loop1().
 *
 * TODO: Migrate audio consumer from HubFX Pico loop1()
 */
static void core1Task(void* param) {
    core1Ready.store(true, std::memory_order_release);

    while (true) {
        loop1Count.fetch_add(1, std::memory_order_relaxed);

        // TODO: Audio I2S consumer loop
        //   - Read from AudioRingBuffer
        //   - Write to I2S DMA via ESP-IDF i2s_channel_write()

        vTaskDelay(pdMS_TO_TICKS(10));  // Placeholder — will be replaced by I2S blocking write
    }
}

// ============================================================================
// Global Objects
// ============================================================================

// Server infrastructure (upstream protocol handling)
SfxServer server;

// ============================================================================
// Arduino Setup (Core 0)
// ============================================================================

void setup() {
    // SfxServer handles serial init, device naming, indicators, core protocol
    server.begin("HubFX", FIRMWARE_VERSION, BUILD_NUMBER);

    // HubFX is the master — auto-init, no upstream connection timeout
    server.setConnectionTimeoutEnabled(false);

    server.onInit([]() {
        // PC sent INIT — configuration/debug session starting
    });

    server.onShutdown([]() {
        // PC session ended — no effect on hub operation
    });

    // TODO: SD card init
    // TODO: Config reader
    // TODO: Audio init (codec + mixer)
    // TODO: USB Host init (slave bus)
    // TODO: Domain-specific command handlers (slave, audio, engine, storage)
    // TODO: STATUS callback

    // Launch Core 1 task for audio consumer
    xTaskCreatePinnedToCore(
        core1Task,          // Task function
        "AudioCore1",       // Task name
        8192,               // Stack size (bytes)
        nullptr,            // Parameters
        configMAX_PRIORITIES - 1,  // High priority for audio
        &core1TaskHandle,   // Task handle
        1                   // Pin to Core 1
    );

    // Hub is the master — mark as operational immediately
    server.indicators().setConnected(true);
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

void loop() {
    // Serial protocol processing
    server.loop();

    // TODO: Slave polling
    // TODO: Audio mixer producer (SD reads, WAV decode, mix into ring buffer)
    // TODO: Engine FX state machine
    // TODO: PC serial detection (USB CDC connected check)

    vTaskDelay(pdMS_TO_TICKS(1));  // Yield to FreeRTOS scheduler
}
