/*
 * HubFX ESP32-S3 — Autonomous Protocol Router Hub
 *
 * Central hub controller for ScaleFX system. Manages slave controllers
 * (GunFX, LightFX, GearControl) via USB Host, audio mixing, and effects.
 *
 * DUAL-CORE ARCHITECTURE (ESP32-S3, dual Xtensa LX7 @ 240 MHz):
 *   Core 0: Main loop — serial protocol, slave management, storage operations
 *   Core 1: Audio consumer task (I2S DMA output, highest priority)
 *           Audio producer task (WAV decode, SD reads, mixing, lower priority)
 *           USB Host polling
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
 *   [x] Audio mixer (I2S output via ESP-IDF driver, 8-ch WAV, dual-core)
 *   [x] Config reader (YAML from flash, reload/save/get protocol)
 *   [x] Slave management (INIT handshake, SlaveType identification, routing)
 *   [x] Audio server (protocol handler)
 *   [x] Engine server (protocol handler)
 *   [x] Engine FX
 *   [ ] Gun FX
 *   [ ] System sounds
 */

#define FIRMWARE_VERSION "0.26.1"
#define BUILD_NUMBER 130

#include <Arduino.h>
#include <atomic>
#include <esp_system.h>

// Platform abstraction (ESP32-S3 specific macros, mutexes, delays)
#include <platform/sfx_platform.h>
#include <platform/diag_log.h>

// Shared serial protocol library
#include <serial/serial.h>
#include <server/sfx_server.h>

// Config schemas
#include "config/hubfx_config.h"
#include <config/config_store.h>
#include <server/config_server.h>

// USB Host (CDC-ACM for slave controller communication)
#include <usb/sfx_usb_host.h>

// Flash storage (LittleFS)
#include <storage/flash.h>

// SD card storage (SD_MMC 1-bit SDIO)
#include <storage/sd_card.h>

// Protocol handlers (HubFX-specific commands)
#include <server/storage_server.h>
#include "protocol/hubfx_usb_server.h"
#include "protocol/slave_server.h"
#include "protocol/slave_manager.h"

// Slave client classes (one per controller type)
#include <gunfx/client/gunfx_client.h>
#include <lightfx/client/lightfx_client.h>
#include <gearcontrol/client/gearcontrol_client.h>

// Audio mixer and codec (8-channel WAV mixer with I2S output)
#include <audio/audio_log.h>
#include <server/audio_server.h>

// HubFX audio channel assignments and concrete Mixer type
#include "hubfx_audio.h"

// Engine FX (sound effects state machine + protocol handler)
#include "effects/engine_fx.h"
#include "protocol/engine_server.h"

// Audio protocol server type alias
using AudioServer = AudioServerT<Mixer>;

// Config store and server type aliases
using HubFxConfigStore  = ConfigStore<HubFxConfigSchema>;
using HubFxConfigServer = ConfigServerT<HubFxConfigStore>;

// ============================================================================
// Pin Definitions (ESP32-S3 DevKitC-1)
// ============================================================================

// Indicator LEDs (directly driven GPIO)
// DevKitC-1 N8R8 has a user-addressable RGB LED on GPIO48 (active-high).
// Use the onboard LED for connection status. Error LED disabled (no external LED).
#define PIN_LED_CONNECTION  48   // Onboard RGB LED (connection status)
#define PIN_LED_ERROR       -1   // Disabled (no external error LED)

// I2S Audio Output (Freenove Audio Converter & Amplifier module)
// No MCLK needed — DAC auto-configures from BCLK/LRCLK
#define PIN_I2S_DOUT    41   // I2S serial data output (DIN on DAC)
#define PIN_I2S_BCLK    42   // I2S bit clock (BCK on DAC)
#define PIN_I2S_LRCLK   14   // I2S word clock / left-right (LCK on DAC)

// SD Card (SD_MMC 1-bit SDIO)
#define PIN_SD_MMC_CMD  38   // SD_MMC command
#define PIN_SD_MMC_CLK  39   // SD_MMC clock
#define PIN_SD_MMC_D0   40   // SD_MMC data 0

// I2C (for TAS5825M codec control, power monitoring, etc.)
#define PIN_I2C_SDA     8    // I2C data  (to TAS5825M SDA)
#define PIN_I2C_SCL     9    // I2C clock (to TAS5825M SCL)

// ============================================================================
// Core 1 Task — Audio Consumer (highest priority on Core 1)
// ============================================================================

// FreeRTOS task handle for Core 1 consumer
static TaskHandle_t core1TaskHandle = nullptr;

// Cross-core state flags
std::atomic<bool> audioInitialized{false};   // Core 0 writes, Core 1 reads
std::atomic<bool> core1Ready{false};         // Core 1 writes, Core 0 reads
// Diagnostic: Core 1 loop iteration counter
std::atomic<uint32_t> loop1Count{0};         // Core 1 writes, Core 0 reads

/**
 * Core 1 consumer task — audio I2S output loop.
 *
 * DUAL-TASK AUDIO ARCHITECTURE (both pinned to Core 1):
 *   Consumer (this task, priority MAX-1): ring buffer → I2S DMA.
 *     Blocks in i2s_channel_write() when DMA is full, releasing CPU.
 *   Producer (AudioMixer task, priority MAX-2): WAV decode + SD reads
 *     + float mixing → ring buffer. Runs when consumer is blocked.
 *
 * Core 0 is freed entirely for protocol handling, slave management,
 * and storage operations. Commands reach the producer via the async
 * command queue (playAsync, stopAsync, etc.).
 */
static void core1Task(void* param) {
    core1Ready.store(true, std::memory_order_release);
    SFX_LOG_INFO("Core 1 consumer task started, waiting for audio init...");

    // Wait for Core 0 to complete Mixer::begin() (Phase 1)
    while (!audioInitialized.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Phase 2: Initialize I2S hardware on Core 1
    // ESP-IDF I2S driver must be installed and written to from the same core.
    Mixer& mixer = Mixer::instance();
    if (!mixer.beginI2S()) {
        SFX_LOG_ERROR("Core 1: I2S init failed — audio disabled");
        // Fall through to idle loop so task doesn't exit
        while (true) {
            loop1Count.fetch_add(1, std::memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    SFX_LOG_INFO("Core 1: I2S running — starting producer task, then consumer loop");

    // Launch producer task on same core (lower priority).
    // Producer fills the SPSC ring buffer from WAV decode + SD reads.
    // It runs when this consumer task blocks on i2s_channel_write().
    mixer.startProducerTask(
        1,                          // Core 1 (same as consumer)
        configMAX_PRIORITIES - 2,   // Below consumer priority
        8192                        // Stack size
    );

    // Audio consumer loop — reads from SPSC ring buffer, writes to I2S DMA.
    // i2s_channel_write() blocks when DMA buffers are full, providing natural pacing.
    // When blocked, FreeRTOS yields to the producer task on this core.
    while (true) {
        loop1Count.fetch_add(1, std::memory_order_relaxed);
        mixer.consume();
        // No vTaskDelay needed — i2s_write() in consume() blocks when DMA is full,
        // naturally pacing the loop at the audio sample rate.
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
    bool usbOk = SlaveManager::instance().isUsbReady();

    SFX_LOG_DEBUG("uptime=%lus heap=%lu core1=%s loop1=%lu usb=%s cdc=%d",
                  uptime_s, heap,
                  c1Ready ? "ready" : "NOT_READY",
                  c1Count,
                  usbOk ? "ready" : "off",
                  usb.cdcDeviceCount());

    // Audio mixer diagnostics
    Mixer& mixer = Mixer::instance();
    if (mixer.isInitialized()) {
        SFX_LOG_DEBUG("audio: i2s=%s ring=%d%% underruns=%lu playing=%s",
                      mixer.isI2SRunning() ? "on" : "off",
                      mixer.getRingFillPercent(),
                      (unsigned long)mixer.getUnderruns(),
                      mixer.isAnyPlaying() ? "yes" : "no");
    }
}

// ============================================================================
// Global Objects
// ============================================================================

// Server infrastructure (upstream protocol handling over UART0)
SfxServer server;

// Module protocol handlers
StorageServer storageServer;
HubFxUsbServer usbServer;
AudioServer audioServer;
SlaveServer slaveServer;
HubFxConfigServer configServer;
EngineServer engineServer;

// ============================================================================
// Flash File I/O Bridges (for ConfigStore)
// ============================================================================

/**
 * @brief Read a file from onboard flash (LittleFS) into a buffer.
 * @return Bytes read, or -1 on error.
 */
static int flashReadFile(const char* path, char* buffer, size_t maxLen) {
    FlashModule& flash = FlashModule::instance();
    if (!flash.isInitialized()) return -1;

    flash.lock();
    LFSFile file;
    uint8_t err = flash.openRead(path, file);
    if (err != 0) {    // FlashError::OK == 0
        flash.unlock();
        return -1;
    }

    int bytesRead = file.read((uint8_t*)buffer, maxLen);
    file.close();
    flash.unlock();
    return bytesRead;
}

/**
 * @brief Write a buffer to a file on onboard flash (LittleFS).
 * @return Bytes written, or -1 on error.
 */
static int flashWriteFile(const char* path, const char* data, size_t len) {
    FlashModule& flash = FlashModule::instance();
    if (!flash.isInitialized()) return -1;

    flash.lock();
    LFSFile file;
    uint8_t err = flash.openWrite(path, file, true);  // truncate
    if (err != 0) {
        flash.unlock();
        return -1;
    }

    int written = file.write((const uint8_t*)data, len);
    file.close();
    flash.unlock();
    return written;
}

// Typed slave clients (file-scope, registered with SlaveManager via addSlave)
static GunFxClient gunfxClient;
static LightFxClient lightfxClient;
static GearControlClient gearcontrolClient;

// ============================================================================
// Arduino Setup (Core 0)
// ============================================================================

// ---- Setup helpers (called once from setup()) ----

/** @brief Initialize Flash (LittleFS) and SD card (SDIO 1-bit). */
static void initStorage() {
    // Flash (LittleFS)
    FlashModule& flash = FlashModule::instance();
    if (flash.begin()) {
        FlashStorageInfo info;
        flash.getStorageInfo(info);
        SFX_LOG_INFO("Flash ready: %lu/%lu bytes used",
                     (unsigned long)info.usedBytes, (unsigned long)info.totalBytes);
    } else {
        SFX_LOG_ERROR("Flash init failed");
    }

    // SD card (SD_MMC 1-bit SDIO)
    SdCardModule& sd = SdCardModule::instance();
    SdCardModule::Config sdCfg { .clk = PIN_SD_MMC_CLK, .cmd = PIN_SD_MMC_CMD, .d0 = PIN_SD_MMC_D0 };
    if (sd.begin(sdCfg)) {
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

/** @brief Configure config store with flash I/O and load initial config. */
static void initConfig() {
    auto& store = configServer.store();
    store.setFileReader(flashReadFile);
    store.setFileWriter(flashWriteFile);

    // Callback fires after every successful config load/reload
    store.onLoaded([](const HubFxConfig& cfg) {
        // Apply audio config (codec supply voltage)
        TAS5825M_SupplyVoltage voltage;
        if (TAS5825Codec::parseSupplyVoltage(cfg.audio.codecSupplyVoltage, voltage)) {
            auto& codec = TAS5825Codec::instance();
            if (codec.isInitialized() && codec.getSupplyVoltage() != voltage) {
                if (codec.setSupplyVoltage(voltage)) {
                    SFX_LOG_INFO("[Config] Codec supply voltage \u2192 %s",
                                 TAS5825Codec::supplyVoltageStr(voltage));
                } else {
                    SFX_LOG_ERROR("[Config] Failed to set codec supply voltage");
                }
            }
        } else {
            SFX_LOG_WARN("[Config] Unknown codec_supply_voltage: '%s' (use 12v/15v/20v/24v)",
                         cfg.audio.codecSupplyVoltage);
        }

        // Apply engine config
        SFX_LOG_INFO("[Config] Applied — engine %s (%s), output=%s",
                     cfg.engineFx.enabled ? "enabled" : "disabled",
                     cfg.engineFx.type,
                     outputChannelsString(cfg.engineFx.outputChannels));
        EngineFX::instance().applyConfig(cfg.engineFx);

        // Log gun_fx config (audio routing applied when GunFX is implemented)
        SFX_LOG_INFO("[Config] Applied — gun_fx output=%s",
                     outputChannelsString(cfg.gunFx.outputChannels));
    });

    // Initial load from flash
    configServer.loadConfig();  // Reads /config.yaml from LittleFS
}

/** @brief Register module protocol handlers and build the handler chain. */
static void initProtocolHandlers() {
    storageServer.begin(&Serial);
    usbServer.begin(&Serial);
    audioServer.begin(&Serial);
    slaveServer.begin(&Serial);
    configServer.begin(&Serial);
    engineServer.begin(&Serial);

    // Handler chain: CoreCommandServer (0xF0-0xFF)
    //              → ConfigServer         (0x90-0x92, 0xAC config)
    //              → SlaveServer          (0x80-0x83 mgmt + 0x96-0x98 routing)
    //              → HubFxUsbServer       (0xA7-0xA8 USB diag)
    //              → AudioServer          (0x84-0x8B audio)
    //              → EngineServer         (0x8C-0x8F engine FX)
    //              → StorageServer        (0x93-0xA6 SD/flash + files)
    server.addModuleHandler(&configServer);
    server.addModuleHandler(&slaveServer);
    server.addModuleHandler(&usbServer);
    server.addModuleHandler(&audioServer);
    server.addModuleHandler(&engineServer);
    server.addModuleHandler(&storageServer);

    // Start dual-core storage writer task (Core 1 handles SD writes)
    storageServer.policy().startWriterTask();
}

/** @brief Register slave types with SlaveManager and initialize USB Host. */
static void initSlaveManager() {
    SlaveManager& mgr = SlaveManager::instance();
    mgr.addSlave({ SlaveType::GunFX,       "GunFX",       "GunFX",   &gunfxClient });
    mgr.addSlave({ SlaveType::LightFX,     "LightFX",     "LightFX", &lightfxClient });
    mgr.addSlave({ SlaveType::GearControl, "GearControl", "GearCtrl", &gearcontrolClient });
    mgr.begin();
}

// NOTE: Engine FX is initialized by the config onLoaded callback
// (EngineFX::applyConfig) — no separate initEngineFx() needed.

/**
 * @brief Initialize audio mixer (Phase 1) and launch Core 1 consumer task.
 *
 * Phase 1 (Core 0): channels, ring buffer, codec init via I2C.
 * Phase 2 (Core 1): I2S hardware init + producer task launch.
 */
static void initAudio() {
    Mixer& mixer = Mixer::instance();

    // Configure TAS5825M codec singleton via I2C
    // Initial supply voltage from config (default: 12V for 3S LiPo)
    // Can be changed at runtime via config.yaml audio.codec_supply_voltage
    TAS5825M_SupplyVoltage initVoltage = TAS5825M_12V;
    if (configServer.store().isLoaded()) {
        TAS5825Codec::parseSupplyVoltage(
            configServer.store().data().audio.codecSupplyVoltage, initVoltage);
    }
    bool codecOk = TAS5825Codec::instance().begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL,
                                                  AUDIO_SAMPLE_RATE, initVoltage);
    if (codecOk) {
        SFX_LOG_INFO("TAS5825M codec initialized (supply=%s)",
                     TAS5825Codec::supplyVoltageStr(initVoltage));
    } else {
        SFX_LOG_WARN("TAS5825M codec init failed — will retry periodically "
                     "(check battery/PVDD power)");
    }

    // Phase 1 init: channels, ring buffer, pin storage
    if (mixer.begin(PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRCLK)) {
        audioInitialized.store(true, std::memory_order_release);
        SFX_LOG_INFO("Audio mixer Phase 1 ready — Core 1 will start I2S");
    } else {
        SFX_LOG_ERROR("Audio mixer init failed — audio disabled");
    }

    // Launch Core 1 task for audio consumer (highest priority)
    xTaskCreatePinnedToCore(
        core1Task,          // Task function
        "AudioConsumer",    // Task name
        8192,               // Stack size (bytes)
        nullptr,            // Parameters
        configMAX_PRIORITIES - 1,  // Highest priority for I2S output
        &core1TaskHandle,   // Task handle
        1                   // Pin to Core 1
    );
    // Note: Producer task is launched FROM core1Task after I2S init,
    // ensuring both tasks run on Core 1 and I2S is ready.
}

// ---- Main setup ----

void setup() {
    // SfxServer handles serial init (UART0 @ 1Mbps), device naming,
    // indicator LEDs, CoreCommandServer, and DiagLog initialization
    server.begin("HubFX", FIRMWARE_VERSION, BUILD_NUMBER,
                 PIN_LED_CONNECTION, PIN_LED_ERROR);

    // Redirect ESP-IDF ESP_LOGx() output into DiagLog ring buffer.
    // CRITICAL: Without this, ESP_LOGE/W from USB host, WiFi, or any
    // ESP-IDF component writes raw text to UART0, corrupting the binary
    // COBS protocol stream and causing the CLI to hang or misparse packets.
    // After this call, ALL ESP-IDF logs become proper [IDF]-prefixed
    // LOG_MESSAGE packets visible via the CLI `diag` command.
    DiagLog::instance().captureEspLog();

    // Verify redirect is working — this ESP_LOGW goes through the redirect
    // and should appear as "[IDF] W (xxx) SFX: ..." in `diag` output
    ESP_LOGW("SFX", "ESP-IDF log redirect active (build %d)", BUILD_NUMBER);

    // Log reset reason — helps diagnose USB hub hot-plug brownout resets
    {
        esp_reset_reason_t reason = esp_reset_reason();
        const char* reasonStr = "UNKNOWN";
        switch (reason) {
            case ESP_RST_POWERON:   reasonStr = "POWER_ON";   break;
            case ESP_RST_EXT:       reasonStr = "EXTERNAL";   break;
            case ESP_RST_SW:        reasonStr = "SOFTWARE";   break;
            case ESP_RST_PANIC:     reasonStr = "PANIC";      break;
            case ESP_RST_INT_WDT:   reasonStr = "INT_WDT";    break;
            case ESP_RST_TASK_WDT:  reasonStr = "TASK_WDT";   break;
            case ESP_RST_WDT:       reasonStr = "OTHER_WDT";  break;
            case ESP_RST_DEEPSLEEP: reasonStr = "DEEPSLEEP";   break;
            case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT";    break;
            case ESP_RST_SDIO:      reasonStr = "SDIO";        break;
            case ESP_RST_USB:       reasonStr = "USB";         break;
            default:                reasonStr = "UNKNOWN";     break;
        }
        SFX_LOG_INFO("Boot reason: %s (%d)", reasonStr, (int)reason);
        if (reason == ESP_RST_BROWNOUT) {
            SFX_LOG_ERROR("*** BROWNOUT RESET — possible USB hub inrush current issue ***");
        } else if (reason == ESP_RST_PANIC) {
            SFX_LOG_ERROR("*** PANIC RESET — check backtrace in serial monitor ***");
        } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT) {
            SFX_LOG_ERROR("*** WATCHDOG RESET — a task may have hung ***");
        }
    }

    // HubFX is the master — auto-init, no upstream connection timeout
    server.setConnectionTimeoutEnabled(false);

    // Track boot completion time — first INIT right after boot can skip
    // heavy re-initialization since everything is already in a clean state.
    static uint32_t bootComplete_ms = 0;
    static constexpr uint32_t FRESH_BOOT_WINDOW_MS = 5000;  // 5s grace period

    server.onInit([]() {
        uint32_t now = millis();

        // If this is the first INIT within the fresh-boot window, everything
        // was just initialized by setup() — skip the expensive re-init.
        if (bootComplete_ms > 0 && (now - bootComplete_ms) < FRESH_BOOT_WINDOW_MS) {
            bootComplete_ms = 0;  // Consume the grace — subsequent INITs do full re-init
            SFX_LOG_INFO("INIT received — fresh boot, skipping re-init");

            // Still scan for slaves (may have enumerated after setup())
            SlaveManager::instance().scanAndInit();
            return;
        }

        // PC sent INIT — new session starting.
        // Full re-initialization of all subsystems to ensure clean state.
        SFX_LOG_INFO("INIT received — re-initializing all subsystems");

        // 1. Cancel any upload left over from a previous session that
        //    disconnected without SHUTDOWN (e.g., Ctrl+C, crash, USB unplug).
        storageServer.cancelActiveUpload();

        // 2. Stop all audio and reset codec to clean power-on state.
        //    stopAsync is thread-safe (Core 0 → Core 1 command queue).
        {
            Mixer& mixer = Mixer::instance();
            if (mixer.isInitialized()) {
                mixer.stopAsync(-1, AudioStopMode::Immediate);
                mixer.setMasterVolumeAsync(1.0f);
                mixer.resetUnderruns();
            }

            // Reset TAS5825M codec (full I2C re-init sequence)
            TAS5825Codec& codec = TAS5825Codec::instance();
            if (codec.isInitialized()) {
                codec.reset();
                codec.clearFaults();
                SFX_LOG_INFO("Audio codec reset");
            } else {
                // Codec never initialized (e.g., no battery at boot) — try now
                if (codec.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL,
                                AUDIO_SAMPLE_RATE, codec.getSupplyVoltage())) {
                    codec.clearFaults();
                    SFX_LOG_INFO("Audio codec initialized on INIT");
                }
            }
        }

        // 3. Reset engine FX state machine (stops sounds, clears state)
        {
            EngineFX& engine = EngineFX::instance();
            if (engine.isInitialized()) {
                engine.end();
            }
        }

        // 4. Reload config from flash.
        // The onLoaded callback re-applies engine settings via applyConfig().
        configServer.loadConfig();

        // 5. Re-scan slaves so PC gets fresh state
        SlaveManager::instance().scanAndInit();

        SFX_LOG_INFO("INIT complete — all subsystems re-initialized");
    });

    server.onShutdown([]() {
        // PC session ended — stop audio, cancel uploads, clean up
        storageServer.cancelActiveUpload();

        // Stop all audio playback
        Mixer& mixer = Mixer::instance();
        if (mixer.isInitialized() && mixer.isAnyPlaying()) {
            mixer.stopAsync(-1, AudioStopMode::Immediate);
        }

        // Stop engine FX
        EngineFX& engine = EngineFX::instance();
        if (engine.isActive()) {
            engine.forceStop();
        }

        SFX_LOG_INFO("SHUTDOWN — session ended, audio stopped");
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
        if (SlaveManager::instance().isUsbReady())          flags |= 0x08;
        if (SdCardModule::instance().isInitialized())        flags |= 0x10;
        buf[0] = flags;

        // Slave presence bitmask: bit0=GunFX, bit1=LightFX, bit2=GearControl
        buf[1] = SlaveManager::instance().slaveMask();

        // Core 1 loop counter (diagnostic)
        CoreProtocol::putU32LE(&buf[2], loop1Count.load(std::memory_order_relaxed));

        return 6;
    });

    // Initialize I2C bus early — gives maximum stabilization time before
    // TAS5825M codec probe. On ESP32, Wire.begin() installs the I2C master
    // driver and performs bus recovery (SCL toggling) if SDA is stuck low.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);  // 100kHz I2C

    initStorage();
    initProtocolHandlers();
    initConfig();       // onLoaded callback initializes EngineFX
    initSlaveManager();
    initAudio();

    // Hub is the master — mark as operational immediately
    server.indicators().setConnected(true);
    bootComplete_ms = millis();  // Start fresh-boot grace period

    SFX_LOG_INFO("HubFX ESP32-S3 v%s (build %d) — setup complete", FIRMWARE_VERSION, BUILD_NUMBER);
    SFX_LOG_INFO("Platform: %s @ %lu MHz, heap: %lu bytes",
                 SFX_PLATFORM_NAME, (unsigned long)SFX_CPU_MHZ(), (unsigned long)SFX_FREE_HEAP());
    if (SFX_HAS_PSRAM) {
        SFX_LOG_INFO("PSRAM: %lu KB total, %lu KB free",
                     (unsigned long)(sfxPsramTotal() / 1024),
                     (unsigned long)(sfxPsramFree_bytes() / 1024));
    } else {
        SFX_LOG_WARN("PSRAM: not available");
    }
}

// ============================================================================
// Periodic Codec Health Check
// ============================================================================
// If the TAS5825M codec failed to initialize at boot (e.g., battery not
// connected → PVDD not powered → I2C probe fails), periodically retry.
// Once initialized, stop retrying.

static void checkCodecHealth() {
    static uint32_t lastCheck_ms = 0;
    static constexpr uint32_t CHECK_INTERVAL_MS = 5000;  // every 5s

    TAS5825Codec& codec = TAS5825Codec::instance();
    if (codec.isInitialized()) return;  // Already working — nothing to do

    uint32_t now = millis();
    if (now - lastCheck_ms < CHECK_INTERVAL_MS) return;
    lastCheck_ms = now;

    SFX_LOG_INFO("Retrying TAS5825M codec init...");
    if (codec.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL, AUDIO_SAMPLE_RATE,
                    codec.getSupplyVoltage())) {
        codec.clearFaults();
        SFX_LOG_INFO("TAS5825M codec initialized on retry — audio amp online");
    }
    // On failure, begin() already logs I2C probe error — no extra log needed
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

void loop() {
    // UPLOAD_STREAM mode: bypass COBS/CommandRouter, route Serial data
    // directly to StorageServer's ring buffer for maximum throughput.
    // Normal COBS processing resumes after all file_size bytes are received.
    if (storageServer.isStreamReceiving()) {
        storageServer.processStreamData(Serial);
    } else {
        // Normal serial protocol processing (UART0 — COBS packets from CLI/PC)
        server.loop();
    }

    // Check for stuck uploads (client crash, USB disconnect, etc.)
    storageServer.checkUploadTimeout();

    // ---- Audio: stop playback when SD upload starts ----
    // SD card is shared between audio reads (Core 1 producer) and
    // upload writes (Core 0). When an upload begins, stop all audio
    // tracks via the async command queue (cross-core safe).
    {
        static bool wasUploading = false;
        bool uploading = storageServer.isUploadActive();
        if (uploading && !wasUploading) {
            Mixer& mixer = Mixer::instance();
            if (mixer.isAnyPlaying()) {
                mixer.stopAsync(-1, AudioStopMode::Immediate);
                MIXER_LOG("Audio stop queued — SD upload starting");
            }
        }
        wasUploading = uploading;
    }

    // NOTE: produce() is no longer called from loop().
    // The producer runs as a dedicated FreeRTOS task on Core 1,
    // launched by startProducerTask() inside core1Task().

    // Periodic diagnostic logging (buffered in DiagLog ring, retrieved via `diag`)
    logDiagnostics();

    // Retry TAS5825M codec init if it failed at boot (e.g., no battery power)
    checkCodecHealth();

    // ---- Slave client polling ----
    // Process incoming data from all connected/ready slave controllers.
    // Slave client polling + periodic discovery (rate-limited internally)
    SlaveManager::instance().process();

    // Engine FX state machine tick
    EngineFX::instance().process();

    // During active upload or streaming, skip vTaskDelay() to maximize
    // throughput. In stream mode, processStreamData() needs to be called
    // as fast as possible to drain the UART RX buffer into the ring buffer.
    if (!storageServer.isUploadActive() && !storageServer.isStreamReceiving()) {
        vTaskDelay(pdMS_TO_TICKS(1));  // Yield to FreeRTOS scheduler
    }
}
