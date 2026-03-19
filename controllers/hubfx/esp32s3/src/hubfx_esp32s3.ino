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

#define FIRMWARE_VERSION "0.21.2"
#define BUILD_NUMBER 110

#include <Arduino.h>
#include <atomic>

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
#include "protocol/slave_registry.h"

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
std::atomic<bool> usbHostReady{false};       // Core 0 writes, Core 0 reads (bus task context)

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
    bool usbOk = usbHostReady.load(std::memory_order_acquire);

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

// Slave registry and clients (one per controller type)
SlaveRegistry& slaveRegistry = SlaveRegistry::instance();
GunFxClient gunfxClient;
LightFxClient lightfxClient;
GearControlClient gearcontrolClient;

// ============================================================================
// USB Device Discovery
// ============================================================================

/**
 * @brief Poll a BusClient until isServerReady() or timeout.
 * @param client  BusClient to poll
 * @param timeout_ms  Maximum wait time (default 3000ms)
 * @return true if server is ready within timeout
 */
static bool awaitInitReady(BusClient& client, uint32_t timeout_ms = 3000) {
    unsigned long start = millis();
    while (!client.isServerReady() && (millis() - start < timeout_ms)) {
        client.process();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return client.isServerReady();
}

/**
 * @brief Attempt to init a slave client on a given USB device index.
 *
 * Sends INIT, waits for INIT_READY, and identifies the controller by
 * matching the device name prefix ("GunFX", "LightFX", "GearControl").
 *
 * @param usbIndex USB CDC device index in UsbHost
 * @return SlaveType detected, or SlaveType::Unknown if failed
 */
static SlaveType tryInitSlave(int usbIndex) {
    SFX_LOG_DEBUG("Probing USB index %d...", usbIndex);

    // Create a temporary generic client to probe the device
    BusClient probe;
    if (!probe.begin(usbIndex)) {
        SFX_LOG_ERROR("Failed to begin probe on USB index %d", usbIndex);
        return SlaveType::Unknown;
    }

    probe.sendInit();

    if (!awaitInitReady(probe)) {
        SFX_LOG_WARN("No INIT_READY from USB index %d (timeout 3s)", usbIndex);
        return SlaveType::Unknown;
    }

    const char* name = probe.serverName();
    SFX_LOG_INFO("USB index %d identified as: %s", usbIndex, name);

    // Match by name prefix
    SlaveType type = SlaveType::Unknown;
    if (strncmp(name, "GunFX", 5) == 0) {
        type = SlaveType::GunFX;
    } else if (strncmp(name, "LightFX", 7) == 0) {
        type = SlaveType::LightFX;
    } else if (strncmp(name, "GearControl", 11) == 0) {
        type = SlaveType::GearControl;
    }

    if (type == SlaveType::Unknown) {
        SFX_LOG_WARN("Unknown slave type: %s", name);
        return SlaveType::Unknown;
    }

    // Now init the proper typed client
    BusClient* client = nullptr;
    switch (type) {
        case SlaveType::GunFX:
            if (gunfxClient.begin(usbIndex)) {
                gunfxClient.sendInit();
                client = &gunfxClient;
            }
            break;
        case SlaveType::LightFX:
            if (lightfxClient.begin(usbIndex)) {
                lightfxClient.sendInit();
                client = &lightfxClient;
            }
            break;
        case SlaveType::GearControl:
            if (gearcontrolClient.begin(usbIndex)) {
                gearcontrolClient.sendInit();
                client = &gearcontrolClient;
            }
            break;
        default:
            break;
    }

    if (!client) {
        SFX_LOG_ERROR("Failed to initialize %s client", slaveTypeName(type));
        return SlaveType::Unknown;
    }

    // Wait for typed client INIT_READY
    if (client->isServerReady() || awaitInitReady(*client)) {
        slaveRegistry.registerSlave(type, client, usbIndex);
        slaveRegistry.setConnected(type, true);
        slaveRegistry.setReady(type, true);
        SFX_LOG_INFO("Slave %s ready: %s", slaveTypeName(type), client->serverName());
        return type;
    }

    SFX_LOG_ERROR("Typed client INIT_READY timeout for %s", slaveTypeName(type));
    return SlaveType::Unknown;
}

/**
 * @brief Scan all USB CDC devices and attempt to identify unregistered ones
 */
static void scanAndInitSlaves() {
    UsbHost& usb = UsbHost::instance();
    int devCount = usb.cdcDeviceCount();
    SFX_LOG_DEBUG("Scanning %d USB CDC devices for slaves...", devCount);

    for (int i = 0; i < devCount; i++) {
        const CdcDeviceInfo* info = usb.getCdcDevice(i);
        if (!info || !info->connected) continue;

        // Check if this device index is already assigned to a slave
        bool alreadyAssigned = false;
        for (uint8_t s = 0; s < slaveRegistry.count(); s++) {
            if (slaveRegistry[s].usbIndex == i && slaveRegistry[s].ready) {
                alreadyAssigned = true;
                break;
            }
        }

        if (!alreadyAssigned) {
            tryInitSlave(i);
        }
    }
}

/**
 * @brief USB mount callback — log device, trigger slave scan if appropriate
 */
static void onUsbMount(uint8_t devAddr, uint16_t vid, uint16_t pid) {
    const char* name = knownDeviceName(vid, pid);
    if (name) {
        SFX_LOG_INFO("USB device mounted: addr=%d VID=%04X PID=%04X — %s",
                     devAddr, vid, pid, name);
    } else {
        SFX_LOG_INFO("USB device mounted: addr=%d VID=%04X PID=%04X (unknown)",
                     devAddr, vid, pid);
    }
}

/**
 * @brief USB unmount callback — mark slave as disconnected
 */
static void onUsbUnmount(uint8_t devAddr) {
    SFX_LOG_WARN("USB device unmounted: addr=%d", devAddr);

    UsbHost& usb = UsbHost::instance();
    for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
        SlaveEntry& slave = slaveRegistry[i];
        if (slave.connected) {
            const CdcDeviceInfo* info = usb.getCdcDevice(slave.usbIndex);
            if (!info || !info->connected || info->dev_addr == devAddr) {
                slaveRegistry.setConnected(slave.type, false);
                SFX_LOG_WARN("Slave %s disconnected (USB addr=%d)", slaveTypeName(slave.type), devAddr);
            }
        }
    }
}

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
        SFX_LOG_INFO("[Config] Applied — engine %s (%s)",
                     cfg.engineFx.enabled ? "enabled" : "disabled",
                     cfg.engineFx.type);
        EngineFX::instance().applyConfig(cfg.engineFx);
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

/** @brief Initialize USB Host and register mount/unmount callbacks. */
static void initUsbHost() {
    UsbHost& usb = UsbHost::instance();

    // Register mount/unmount callbacks BEFORE init() so we catch
    // any devices that enumerate during startup.
    usb.onMount(onUsbMount);
    usb.onUnmount(onUsbUnmount);

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

/** @brief Pre-register slave types and set up slave log relays. */
static void initSlaves() {
    slaveRegistry.registerSlave(SlaveType::GunFX, &gunfxClient, -1);
    slaveRegistry.registerSlave(SlaveType::LightFX, &lightfxClient, -1);
    slaveRegistry.registerSlave(SlaveType::GearControl, &gearcontrolClient, -1);
    SFX_LOG_DEBUG("Slave registry: %d slots pre-registered", slaveRegistry.count());

    // Relay slave log messages into HubFX's own diagnostic log buffer
    auto registerLogRelay = [](BusClient& client, const char* prefix) {
        client.onLogMessage([prefix](uint8_t level, uint32_t, const char* msg) {
            char buf[160];
            snprintf(buf, sizeof(buf), "[%s] %s", prefix, msg);
            DiagLog::instance().ingest(level, buf);
        });
    };
    registerLogRelay(gunfxClient, "GunFX");
    registerLogRelay(lightfxClient, "LightFX");
    registerLogRelay(gearcontrolClient, "GearCtrl");
}

/** @brief Initialize engine FX from loaded config. */
static void initEngineFx() {
    auto& store = configServer.store();
    if (store.isLoaded()) {
        EngineFX::instance().begin(store.data().engineFx);
    } else {
        SFX_LOG_WARN("Config not loaded — engine FX deferred until config reload");
    }
}

/**
 * @brief Initialize audio mixer (Phase 1) and launch Core 1 consumer task.
 *
 * Phase 1 (Core 0): channels, ring buffer, codec init via I2C.
 * Phase 2 (Core 1): I2S hardware init + producer task launch.
 */
static void initAudio() {
    Mixer& mixer = Mixer::instance();

    // Configure TAS5825M codec singleton via I2C
    // Supply voltage: TAS5825M_12V for 3S LiPo (~11.1V)
    //                 TAS5825M_15V for 4S LiPo (~14.8V)
    //                 TAS5825M_20V / TAS5825M_24V for bench PSU
    TAS5825Codec::instance().begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL,
                                   AUDIO_SAMPLE_RATE, TAS5825M_12V);

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

    // HubFX is the master — auto-init, no upstream connection timeout
    server.setConnectionTimeoutEnabled(false);

    server.onInit([]() {
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
            }
        }

        // 3. Reset engine FX state machine (stops sounds, clears state)
        {
            EngineFX& engine = EngineFX::instance();
            if (engine.isInitialized()) {
                engine.end();
            }
        }

        // 4. Reload config from flash (re-applies engine settings via callback)
        configServer.loadConfig();

        // 5. Re-initialize engine FX with (re)loaded config
        initEngineFx();

        // 6. Re-scan slaves so PC gets fresh state
        if (usbHostReady.load(std::memory_order_acquire)) {
            scanAndInitSlaves();
        }

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
        if (usbHostReady.load(std::memory_order_acquire))    flags |= 0x08;
        if (SdCardModule::instance().isInitialized())        flags |= 0x10;
        buf[0] = flags;

        // Slave presence bitmask: bit0=GunFX, bit1=LightFX, bit2=GearControl
        uint8_t slaveMask = 0;
        for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
            if (slaveRegistry[i].ready) {
                slaveMask |= (1 << ((uint8_t)slaveRegistry[i].type - 1));
            }
        }
        buf[1] = slaveMask;

        // Core 1 loop counter (diagnostic)
        CoreProtocol::putU32LE(&buf[2], loop1Count.load(std::memory_order_relaxed));

        return 6;
    });

    initStorage();
    initProtocolHandlers();
    initConfig();
    initUsbHost();
    initSlaves();
    initEngineFx();
    initAudio();

    // Hub is the master — mark as operational immediately
    server.indicators().setConnected(true);

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

    // ---- Slave client polling ----
    // Process incoming data from all connected/ready slave controllers.
    // Each process() call drains the CDC RX buffer and handles COBS frames
    // (ACK/NACK responses, LOG_MESSAGE relay, async notifications).
    {
        static uint32_t lastSlavePoll_ms = 0;
        static constexpr uint32_t SLAVE_POLL_INTERVAL_ms = 100;

        uint32_t now = millis();
        if (now - lastSlavePoll_ms >= SLAVE_POLL_INTERVAL_ms) {
            lastSlavePoll_ms = now;
            for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
                SlaveEntry& slave = slaveRegistry[i];
                if (slave.client && slave.connected && slave.ready) {
                    slave.client->process();
                }
            }
        }
    }

    // ---- Periodic slave discovery ----
    // If any slave slot is not yet ready and USB devices exist, attempt to
    // identify and bind them. Rate-limited to avoid spamming INIT probes.
    {
        static uint32_t lastDiscoveryScan_ms = 0;
        static constexpr uint32_t DISCOVERY_SCAN_INTERVAL_ms = 5000;

        uint32_t now = millis();
        if (usbHostReady.load(std::memory_order_acquire) &&
            now - lastDiscoveryScan_ms >= DISCOVERY_SCAN_INTERVAL_ms) {
            lastDiscoveryScan_ms = now;

            // Check if any slot still needs discovery
            bool anyUnready = false;
            for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
                if (!slaveRegistry[i].ready) {
                    anyUnready = true;
                    break;
                }
            }

            UsbHost& usb = UsbHost::instance();
            if (anyUnready && usb.cdcDeviceCount() > 0) {
                scanAndInitSlaves();
            }
        }
    }

    // Engine FX state machine tick
    EngineFX::instance().process();

    // During active upload or streaming, skip vTaskDelay() to maximize
    // throughput. In stream mode, processStreamData() needs to be called
    // as fast as possible to drain the UART RX buffer into the ring buffer.
    if (!storageServer.isUploadActive() && !storageServer.isStreamReceiving()) {
        vTaskDelay(pdMS_TO_TICKS(1));  // Yield to FreeRTOS scheduler
    }
}
