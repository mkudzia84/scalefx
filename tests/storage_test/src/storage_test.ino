/*
 * Storage Test — ESP32-S3 Upload Throughput Harness
 *
 * Minimal firmware that wires StorageServer (the shipping single-core
 * Esp32StoragePolicy) to LittleFS (flash) and SD_MMC (1-bit SDIO) for
 * throughput measurement of the sfx_storage upload protocol. Paired with
 * the Go client in tests/storage_test_client/.
 *
 * Single-core design:
 *   Core 0 handles protocol AND SD writes synchronously. No ring buffer,
 *   no writer task, no cross-core atomics. Matches what HubFX ships with.
 *
 * SD pinout — matches HubFX so the DevKitC-1 can reuse the HubFX SD
 * harness:
 *   CMD = GPIO 38
 *   CLK = GPIO 39
 *   D0  = GPIO 40
 *
 * Indicator LED: onboard RGB on GPIO 48 (DevKitC-1 N8R8).
 *
 * CLI behavior: identifies as "StorageTest-XXXX" on `init`. Supports the
 * full file.* command set (list/tree/info/delete/mkdir/download/upload)
 * plus sd.status / flash.status. No audio, USB host, config, or engine.
 */

#define FIRMWARE_VERSION "0.2.0"
#define BUILD_NUMBER     1

#include <Arduino.h>
#include <esp_system.h>

#include <platform/sfx_platform.h>
#include <platform/diag_log.h>
#include <serial/serial.h>
#include <server/sfx_server.h>

// Storage (LittleFS flash + SD_MMC 1-bit SDIO)
#include <storage/flash.h>
#include <storage/sd_card.h>
#include <server/storage_server.h>

// ============================================================================
// Pin Definitions (ESP32-S3 DevKitC-1 — same as HubFX)
// ============================================================================

#define PIN_LED_CONNECTION  48   // Onboard RGB LED
#define PIN_LED_ERROR       -1   // No external error LED

#define PIN_SD_MMC_CMD  38
#define PIN_SD_MMC_CLK  39
#define PIN_SD_MMC_D0   40

// ============================================================================
// Server Instances
// ============================================================================

static SfxServer server;
static StorageServer storageServer;

// ============================================================================
// Setup helpers
// ============================================================================

static void logResetReason() {
    esp_reset_reason_t r = esp_reset_reason();
    const char* name = "UNKNOWN";
    switch (r) {
        case ESP_RST_POWERON:   name = "POWER_ON";   break;
        case ESP_RST_EXT:       name = "EXTERNAL";   break;
        case ESP_RST_SW:        name = "SOFTWARE";   break;
        case ESP_RST_PANIC:     name = "PANIC";      break;
        case ESP_RST_INT_WDT:   name = "INT_WDT";    break;
        case ESP_RST_TASK_WDT:  name = "TASK_WDT";   break;
        case ESP_RST_WDT:       name = "OTHER_WDT";  break;
        case ESP_RST_DEEPSLEEP: name = "DEEPSLEEP";  break;
        case ESP_RST_BROWNOUT:  name = "BROWNOUT";   break;
        case ESP_RST_USB:       name = "USB";        break;
        default: break;
    }
    SFX_LOG_INFO("Boot reason: %s (%d)", name, (int)r);
}

static void initStorage() {
    FlashModule& flash = FlashModule::instance();
    if (flash.begin()) {
        FlashStorageInfo info;
        flash.getStorageInfo(info);
        SFX_LOG_INFO("Flash ready: %lu/%lu bytes used",
                     (unsigned long)info.usedBytes,
                     (unsigned long)info.totalBytes);
    } else {
        SFX_LOG_ERROR("Flash init failed");
    }

    SdCardModule& sd = SdCardModule::instance();
    SdCardModule::Config sdCfg { .clk = PIN_SD_MMC_CLK,
                                  .cmd = PIN_SD_MMC_CMD,
                                  .d0  = PIN_SD_MMC_D0 };
    if (sd.begin(sdCfg)) {
        StorageInfo info;
        sd.getStorageInfo(info);
        static const char* typeNames[] = {"NONE", "MMC", "SD", "SDHC", "UNKNOWN"};
        uint8_t ct = (uint8_t)info.cardType;
        const char* typeName = ct <= 4 ? typeNames[ct] : "?";
        SFX_LOG_INFO("SD ready: %s %luMB (free=%luMB used=%luMB, 1-bit SDIO HS)",
                     typeName,
                     (unsigned long)info.cardSize_MB,
                     (unsigned long)info.freeSpace_MB,
                     (unsigned long)info.usedSpace_MB);
    } else {
        SFX_LOG_WARN("SD card not available (no card inserted?)");
    }
}

// ============================================================================
// setup / loop
// ============================================================================

void setup() {
    server.begin("StorageTest", FIRMWARE_VERSION, BUILD_NUMBER,
                 PIN_LED_CONNECTION, PIN_LED_ERROR);

    // Redirect ESP-IDF logs through DiagLog so they don't corrupt COBS.
    DiagLog::instance().captureEspLog();

    logResetReason();

    // This firmware is the master in its USB-serial session — no upstream
    // connection timeout. CLI / Go test client drives everything.
    server.setConnectionTimeoutEnabled(false);

    server.onInit([](uint8_t mode, uint8_t flags) {
        (void)mode; (void)flags;
        // Cancel any upload left over from a previous session.
        storageServer.cancelActiveUpload();
        SFX_LOG_INFO("INIT — session started");
    });

    server.onShutdown([]() {
        storageServer.cancelActiveUpload();
        SFX_LOG_INFO("SHUTDOWN — session ended");
    });

    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 1) return 0;
        uint8_t flags = 0;
        if (FlashModule::instance().isInitialized())  flags |= 0x01;
        if (SdCardModule::instance().isInitialized()) flags |= 0x02;
        if (storageServer.isUploadActive())           flags |= 0x04;
        if (storageServer.isStreamActive())           flags |= 0x08;
        buf[0] = flags;
        return 1;
    });

    initStorage();

    storageServer.begin(&Serial);
    server.addModuleHandler(&storageServer);

    storageServer.onTransferStart([]() { server.core().setTransferActive(true);  });
    storageServer.onTransferEnd  ([]() { server.core().setTransferActive(false); });

    // Auto-connected — no upstream INIT required to start serving.
    server.indicators().setConnected(true);

    SFX_LOG_INFO("StorageTest v%s build %d ready — heap=%lu PSRAM_free=%lu",
                 FIRMWARE_VERSION, BUILD_NUMBER,
                 (unsigned long)SFX_FREE_HEAP(),
                 (unsigned long)(SFX_HAS_PSRAM ? sfxPsramFree_bytes() : 0));
}

void loop() {
    if (storageServer.isStreamActive()) {
        storageServer.processStream();
    } else {
        server.loop();
    }
    storageServer.checkUploadTimeout();

    if (!storageServer.isUploadActive()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
