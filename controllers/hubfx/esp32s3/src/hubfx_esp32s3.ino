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
 *   Upstream:  UART0 via USB-UART bridge (1Mbps) — COBS protocol for
 *              debug/config CLI and DiagLog retrieval
 *   Downstream: USB Host (OTG) — binary COBS to each slave controller
 *
 * Serial architecture (ESP32-S3 DevKitC-1):
 *   The DevKitC-1 has TWO USB connectors:
 *     - USB-UART (CP2102N/CH340): Connected to UART0 → `Serial`
 *       Used for: flashing (esptool), COBS protocol, DiagLog, CLI
 *     - USB-OTG (native USB): Reserved for USB Host mode (slave controllers)
 *       cdc_on_boot=0 prevents the OTG port from acting as a CDC serial
 *
 * This is a skeleton for migration from HubFX Pico (RP2350).
 * Code modules will be migrated individually from controllers/hubfx/pico/.
 *
 * Migration status:
 *   [x] DiagLog over UART (SFX_LOG_* macros, DIAG_HISTORY command)
 *   [x] Core protocol (INIT, SHUTDOWN, REBOOT, STATUS, KEEPALIVE, I2C_SCAN)
 *   [x] USB Host (mount/unmount logging, device listing)
 *   [x] Flash storage (LittleFS, file list/info/delete/mkdir/download/upload)
 *   [x] USB device listing (USB_DEVICES_REQ/RESP)
 *   [x] SD card storage (SD_MMC 1-bit SDIO, file ops, upload/download)
 *   [ ] Audio mixer (I2S output via ESP-IDF driver)
 *   [ ] Config reader
 *   [ ] Slave management (INIT handshake, SlaveType identification)
 *   [ ] Audio server (protocol handler)
 *   [ ] Engine server (protocol handler)
 *   [ ] Engine FX
 *   [ ] Gun FX
 *   [ ] System sounds
 */

#define FIRMWARE_VERSION "0.7.3"
#define BUILD_NUMBER 37

#include <Arduino.h>
#include <atomic>

// Platform abstraction (ESP32-S3 specific macros, mutexes, delays)
#include <platform/sfx_platform.h>
#include <platform/diag_log.h>

// Shared serial protocol library
#include <serial/serial.h>
#include <server/sfx_server.h>

// USB Host (CDC-ACM for slave controller communication)
#include <usb/sfx_usb_host.h>

// Flash storage (LittleFS)
#include <storage/flash.h>

// SD card storage (SD_MMC 1-bit SDIO)
#include <storage/sd_card.h>

// Protocol handlers (HubFX-specific commands)
#include "protocol/hubfx_storage_server.h"
#include "protocol/hubfx_usb_server.h"

// ============================================================================
// Pin Definitions (ESP32-S3 DevKitC-1)
// ============================================================================

// Indicator LEDs (directly driven GPIO)
// DevKitC-1 has a user-addressable RGB LED on GPIO48 — not used here.
// Use external LEDs on available GPIOs for connection/error indicators.
#define PIN_LED_CONNECTION  13   // Connection status LED
#define PIN_LED_ERROR       14   // Error/warning status LED

// I2S Audio Output
// TODO: Assign pins based on chosen DAC board
// #define PIN_I2S_DATA    ?
// #define PIN_I2S_BCLK    ?
// #define PIN_I2S_LRCLK   ?
// #define PIN_I2S_MCLK    ?    // ESP32-S3 supports MCLK (Pico does not)

// SD Card (SD_MMC 1-bit SDIO)
#define PIN_SD_MMC_CMD  38   // SD_MMC command
#define PIN_SD_MMC_CLK  39   // SD_MMC clock
#define PIN_SD_MMC_D0   40   // SD_MMC data 0

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
std::atomic<bool> usbHostReady{false};       // Core 0 writes, Core 0 reads (bus task context)

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
// Periodic Diagnostic Logging
// ============================================================================

static uint32_t lastDiagLog_ms = 0;
static constexpr uint32_t DIAG_LOG_INTERVAL_ms = 10000;  // Every 10 seconds

/**
 * @brief Emit periodic diagnostic log entries (uptime, heap, core 1 stats)
 *
 * Called from loop(). Rate-limited to DIAG_LOG_INTERVAL_ms.
 * These messages are buffered in DiagLog's ring buffer and can be
 * retrieved via the CLI `diag` command (DIAG_HISTORY packet).
 */
static void logDiagnostics() {
    uint32_t now = millis();
    if (now - lastDiagLog_ms < DIAG_LOG_INTERVAL_ms) return;
    lastDiagLog_ms = now;

    uint32_t uptime_s = now / 1000;
    uint32_t heap = SFX_FREE_HEAP();
    uint32_t c1Count = loop1Count.load(std::memory_order_relaxed);
    bool c1Ready = core1Ready.load(std::memory_order_acquire);

    UsbHost& usb = UsbHost::instance();
    bool usbOk = usbHostReady.load(std::memory_order_acquire);

    SFX_LOG_DEBUG("uptime=%lus heap=%lu core1=%s loop1=%lu usb=%s cdc=%d",
                  uptime_s, heap,
                  c1Ready ? "ready" : "NOT_READY",
                  c1Count,
                  usbOk ? "ready" : "off",
                  usb.cdcDeviceCount());
}

// ============================================================================
// Global Objects
// ============================================================================

// Server infrastructure (upstream protocol handling over UART0)
SfxServer server;

// Module protocol handlers
HubFxStorageServer storageServer;
HubFxUsbServer usbServer;

// ============================================================================
// Arduino Setup (Core 0)
// ============================================================================

void setup() {
    // SfxServer handles serial init (UART0 @ 1Mbps), device naming,
    // indicator LEDs, CoreCommandServer, and DiagLog initialization
    server.begin("HubFX", FIRMWARE_VERSION, BUILD_NUMBER,
                 PIN_LED_CONNECTION, PIN_LED_ERROR);

    // HubFX is the master — auto-init, no upstream connection timeout
    server.setConnectionTimeoutEnabled(false);

    server.onInit([]() {
        // PC sent INIT — new session starting.
        // Cancel any upload left over from a previous session that disconnected
        // without sending SHUTDOWN (e.g., Ctrl+C, crash, USB unplug).
        storageServer.cancelActiveUpload();
        SFX_LOG_INFO("INIT received — debug session active");
    });

    server.onShutdown([]() {
        // PC session ended — clean up any active upload
        storageServer.cancelActiveUpload();
        SFX_LOG_INFO("SHUTDOWN — debug session ended");
    });

    // ---- STATUS callback: module-specific status bytes ----
    // Appended after the 20-byte core header in STATUS responses.
    // Layout: [flags:u8][slaveMask:u8][loop1Count:u32LE] = 6 bytes
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 6) return 0;

        // flags byte:
        //   bit 0: core1Ready
        //   bit 1: audioInitialized
        //   bit 2: flash initialized
        //   bit 3: USB host ready
        //   bit 4: SD card ready
        uint8_t flags = 0;
        if (core1Ready.load(std::memory_order_acquire))      flags |= 0x01;
        if (audioInitialized.load(std::memory_order_acquire)) flags |= 0x02;
        if (FlashModule::instance().isInitialized())         flags |= 0x04;
        if (usbHostReady.load(std::memory_order_acquire))    flags |= 0x08;
        if (SdCardModule::instance().isInitialized())        flags |= 0x10;
        buf[0] = flags;

        // Slave presence bitmask (not yet populated — placeholder)
        buf[1] = 0x00;

        // Core 1 loop counter (diagnostic)
        CoreProtocol::putU32LE(&buf[2], loop1Count.load(std::memory_order_relaxed));

        return 6;
    });

    // ---- Initialize Flash (LittleFS) ----
    {
        FlashModule& flash = FlashModule::instance();
        if (flash.begin()) {
            FlashStorageInfo info;
            flash.getStorageInfo(info);
            SFX_LOG_INFO("Flash ready: %lu/%lu bytes used",
                         (unsigned long)info.usedBytes, (unsigned long)info.totalBytes);
        } else {
            SFX_LOG_ERROR("Flash init failed");
        }
    }

    // ---- Initialize SD Card (SD_MMC 1-bit SDIO) ----
    {
        SdCardModule& sd = SdCardModule::instance();
        if (sd.beginSDIO(true, PIN_SD_MMC_CLK, PIN_SD_MMC_CMD, PIN_SD_MMC_D0)) {
            StorageInfo info;
            sd.getStorageInfo(info);

            static const char* typeNames[] = {"NONE", "MMC", "SD", "SDHC", "UNKNOWN"};
            uint8_t ct = (uint8_t)info.cardType;
            const char* typeName = ct <= 4 ? typeNames[ct] : "?";

            SFX_LOG_INFO("SD card ready: %s %lu MB (total=%lu free=%lu used=%lu, SDIO 1-bit)",
                         typeName,
                         (unsigned long)info.cardSize_MB,
                         (unsigned long)info.totalSpace_MB,
                         (unsigned long)info.freeSpace_MB,
                         (unsigned long)info.usedSpace_MB);
        } else {
            SFX_LOG_WARN("SD card not available (no card inserted?)");
        }
    }

    // ---- Register module protocol handlers ----
    storageServer.begin(&Serial);
    usbServer.begin(&Serial);
    server.addModuleHandler(&storageServer);
    server.addModuleHandler(&usbServer);

    // ---- USB Host initialization ----
    // ESP32-S3 HW USB-OTG on fixed GPIO19 (D-) / GPIO20 (D+).
    // Event-driven: daemon + CDC-ACM tasks run in FreeRTOS background.
    {
        UsbHost& usb = UsbHost::instance();

        // Register mount/unmount callbacks BEFORE init() so we catch
        // any devices that enumerate during startup.
        usb.onMount([](uint8_t devAddr, uint16_t vid, uint16_t pid) {
            SFX_LOG_INFO("USB device mounted: addr=%d VID=%04X PID=%04X",
                         devAddr, vid, pid);
        });
        usb.onUnmount([](uint8_t devAddr) {
            SFX_LOG_INFO("USB device unmounted: addr=%d", devAddr);
        });

        if (usb.begin()) {
            if (usb.init()) {
                usbHostReady.store(true, std::memory_order_release);
                SFX_LOG_INFO("USB Host ready (%s)", usb.backendName());
            } else {
                SFX_LOG_ERROR("USB Host init() failed");
            }
        } else {
            SFX_LOG_ERROR("USB Host begin() failed");
        }
    }

    // TODO: Config reader
    // TODO: Audio init (codec + mixer)
    // TODO: Domain-specific command handlers (slave, audio, engine)
    //       e.g. server.addModuleHandler(&audioServer);

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

    SFX_LOG_INFO("HubFX ESP32-S3 v%s (build %d) — setup complete", FIRMWARE_VERSION, BUILD_NUMBER);
    SFX_LOG_INFO("Platform: %s @ %lu MHz, heap: %lu bytes",
                 SFX_PLATFORM_NAME, (unsigned long)SFX_CPU_MHZ(), (unsigned long)SFX_FREE_HEAP());
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

void loop() {
    // Serial protocol processing (UART0 — COBS packets from CLI/PC)
    server.loop();

    // Check for stuck uploads (client crash, USB disconnect, etc.)
    storageServer.checkUploadTimeout();

    // Periodic diagnostic logging (buffered in DiagLog ring, retrieved via `diag`)
    logDiagnostics();

    // TODO: Slave polling
    // TODO: Audio mixer producer (SD reads, WAV decode, mix into ring buffer)
    // TODO: Engine FX state machine

    vTaskDelay(pdMS_TO_TICKS(1));  // Yield to FreeRTOS scheduler
}
