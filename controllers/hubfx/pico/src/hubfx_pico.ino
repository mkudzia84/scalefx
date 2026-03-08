/*
 * HubFX Pico - Autonomous Protocol Router Hub
 *
 * Central hub controller for ScaleFX system. Manages slave Pico controllers
 * (GunFX, LightFX, GearControl) via USB Host, audio mixing, and effects.
 * Auto-initializes on boot and operates independently â€” no upstream host
 * required for normal operation.
 *
 * PC SERIAL (OPTIONAL):
 *   USB CDC serial (1Mbps) provides configuration and troubleshooting access.
 *   The hub detects PC connection via tud_cdc_connected() and only processes
 *   serial commands when a terminal is open. All normal operations (slave
 *   management, audio, effects) continue regardless of PC serial state.
 *
 * DUAL-CORE ARCHITECTURE:
 *   Core 0: Main loop â€” serial protocol processing, slave client polling,
 *           audio mixing commands, effects state machines
 *   Core 1: USB Host task (PIO-USB), audio I2S output
 *
 * COMMUNICATION:
 *   Upstream:  USB CDC Serial (1Mbps) â€” optional, config/debug only
 *   Downstream: PIO-USB Host CDC â€” binary COBS to each slave Pico
 *
 * MULTI-HANDLER ARCHITECTURE:
 *   Each domain registers its own ICommandHandler with PicoServer:
 *     CoreCommandServer (0xF0-0xFF) â€” system commands (auto-registered)
 *     SlaveServer       (0x80-0x83, 0x96-0x98) â€” slave mgmt + subcmd routing
 *     AudioServer       (0x84-0x8B) â€” audio playback control
 *     EngineServer      (0x8C-0x8F) â€” engine FX control
 *     StorageServer     (0x90-0xA6) â€” config + SD card + file transfer
 *
 * SLAVE ROUTING (subcmd pattern):
 *   Packet type 0x96 (SLAVE_ROUTE_GUNFX)       [subcmd:u8][payload...] â†’ GunFX
 *   Packet type 0x97 (SLAVE_ROUTE_LIGHTFX)     [subcmd:u8][payload...] â†’ LightFX
 *   Packet type 0x98 (SLAVE_ROUTE_GEARCONTROL) [subcmd:u8][payload...] â†’ GearControl
 *   The subcmd byte is the original slave packet type, forwarded as-is.
 */

#define FIRMWARE_VERSION "2.6.0"
#define BUILD_NUMBER 9

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

// ============================================================================
// Codec Selection (must be before conditional includes)
// ============================================================================

// #define USE_TAS5825_CODEC
// #define USE_SIMPLE_I2S_CODEC
#define USE_PICOAUDIO_CODEC         // Pimoroni Pico Audio Pack (PCM5100A)

// Shared serial protocol library (core, bus, clients)
#include <serial.h>
#include <pico_server.h>

// Local HubFX modules â€” domain-specific servers
#include "board_manager/hubfx_protocol.h"
#include "board_manager/slave_registry.h"
#include "board_manager/slave_server.h"
#include "audio/audio_server.h"
#include "effects/engine_server.h"
#include "storage/storage_server.h"

// Audio system
#include "audio/audio_codec.h"
#if defined(USE_TAS5825_CODEC)
#include "audio/tas5825_codec.h"
#elif defined(USE_PICOAUDIO_CODEC) || defined(USE_SIMPLE_I2S_CODEC)
#include "audio/simple_i2s_codec.h"
#endif
#include "audio/audio_mixer.h"
#include "audio/audio_channels.h"
#include "audio/system_sounds.h"

// Storage
#include "storage/sd_card.h"
#include "storage/config_reader.h"

// Effects
#include "effects/engine_fx.h"

// TinyUSB config
#include "tusb_config.h"

// Logging — all HubFX modules use macros from hubfx_log.h which route
// through PicoServer's DiagLog singleton (initialized by PicoServer::begin()).
#include "hubfx_log.h"

// ============================================================================
// Pin Definitions
// ============================================================================

// I2S Audio Output — pin assignments depend on codec board
#if defined(USE_PICOAUDIO_CODEC)
// Pimoroni Pico Audio Pack (PCM5100A DAC)
#define PIN_I2S_DATA    9   // GP9  — I2S DIN
#define PIN_I2S_BCLK    10  // GP10 — I2S BCLK
#define PIN_I2S_LRCLK   11  // GP11 — I2S LRCLK (always BCLK+1)
#define PIN_I2S_MUTE    22  // GP22 — PCM5100A XSMT (HIGH=unmute)
#else
// TAS5825M / generic I2S
#define PIN_I2S_DATA    6   // GP6
#define PIN_I2S_BCLK    7   // GP7
#define PIN_I2S_LRCLK   8   // GP8
#endif

// I2C for codec control (TAS5825M)
#if defined(USE_TAS5825_CODEC)
#define PIN_I2C_SDA     4   // GP4
#define PIN_I2C_SCL     5   // GP5
#endif

// SD Card (SPI0)
#define PIN_SD_CS       17  // GP17
#define PIN_SD_SCK      18  // GP18
#define PIN_SD_MOSI     19  // GP19
#define PIN_SD_MISO     16  // GP16

// ============================================================================
// Global Objects
// ============================================================================

// Server infrastructure (upstream protocol handling)
PicoServer server;
SlaveServer slaveServer;
AudioServer audioServer;
EngineServer engineServer;
StorageServer storageServer;
SlaveRegistry& slaveRegistry = SlaveRegistry::instance();

// USB Host (downstream to slave Picos, runs on Core 1)
UsbHost usbHost;

// Slave clients (one per controller type)
GunFxClient gunfxClient;
LightFxClient lightfxClient;
GearControlClient gearcontrolClient;

// Audio system
SdCardModule& sdCard = SdCardModule::instance();
#if defined(USE_TAS5825_CODEC)
TAS5825Codec audioCodec;
AudioCodec* codec = &audioCodec;
#elif defined(USE_PICOAUDIO_CODEC)
SimpleI2SCodec audioCodec("PCM5100A", PIN_I2S_MUTE);  // mute on GP22
AudioCodec* codec = &audioCodec;
#elif defined(USE_SIMPLE_I2S_CODEC)
SimpleI2SCodec audioCodec("I2S-DAC");
AudioCodec* codec = &audioCodec;
#else
AudioCodec* codec = nullptr;
#endif
AudioMixer& mixer = AudioMixer::instance();

// Effects
EngineFX engineFx;

// Configuration
ConfigReader configReader;

// State
bool audioInitialized = false;
bool sdInitialized = false;
bool pcSerialConnected = false;  // PC connected over USB CDC serial

// ============================================================================
// Core 1 â€” Audio + USB Host (runs on dedicated core)
// ============================================================================

volatile bool core1Ready = false;

void setup1() {
    // Wait for Core 0 to finish audio init
    while (!audioInitialized) {
        delay(10);
    }

    // Initialize USB Host on Core 1 (PIO-USB timing requirement)
    if (usbHost.init()) {
        SFX_LOG_INFO("USB Host initialized on Core 1 (D+:GP%d, D-:GP%d)",
                     PIO_USB_DP_PIN_DEFAULT, PIO_USB_DP_PIN_DEFAULT + 1);
    } else {
        SFX_LOG_ERROR("USB Host init failed on Core 1");
    }

    core1Ready = true;
    SFX_LOG_INFO("Core 1 ready (audio + USB Host)");
}

void loop1() {
    // Audio mixing (I2S output)
    if (audioInitialized) {
        mixer.process();
    }

    // USB Host task processing (enumeration, hot-plug)
    usbHost.process();
}

// ============================================================================
// USB Device Discovery
// ============================================================================

/**
 * When a USB CDC device mounts, we attempt to identify it by sending INIT
 * and matching the device name prefix in INIT_READY response.
 *
 * For now, we use a simple assignment model:
 *   - First CDC device that mounts â†’ try each client until one responds
 *   - Identification is done asynchronously via BusClient::onReady callback
 *
 * TODO: Future enhancement â€” auto-discovery via VID/PID or device descriptor
 */

void onUsbMount(uint8_t devAddr, uint16_t vid, uint16_t pid) {
    SFX_LOG_INFO("USB device mounted: addr=%d VID=%04X PID=%04X", devAddr, vid, pid);
    // Device will be identified when we poll clients in the main loop
}

void onUsbUnmount(uint8_t devAddr) {
    SFX_LOG_WARN("USB device unmounted: addr=%d", devAddr);

    // Mark any slave using this device as disconnected
    for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
        SlaveEntry& slave = slaveRegistry[i];
        // Check if the client's underlying USB device was at this address
        if (slave.connected) {
            const CdcDeviceInfo* info = usbHost.getCdcDevice(slave.usbIndex);
            if (!info || !info->connected || info->dev_addr == devAddr) {
                slaveRegistry.setConnected(slave.type, false);
                SFX_LOG_WARN("Slave %s disconnected (USB addr=%d)", slaveTypeName(slave.type), devAddr);
            }
        }
    }
}

// ============================================================================
// Slave Initialization
// ============================================================================

/**
 * @brief Attempt to init a slave client on a given USB device index.
 *
 * Sends INIT, waits for INIT_READY, and identifies the controller by
 * matching the device name prefix ("GunFX", "LightFX", "GearControl").
 *
 * @param usbIndex USB CDC device index in UsbHost
 * @return SlaveType detected, or SlaveType::Unknown if failed
 */
SlaveType tryInitSlave(int usbIndex) {
    SFX_LOG_DEBUG("Probing USB index %d...", usbIndex);

    // Create a temporary generic client to probe the device
    // We'll send INIT and see what name comes back
    BusClient probe;
    if (!probe.begin(&usbHost, usbIndex)) {
        SFX_LOG_ERROR("Failed to begin probe on USB index %d", usbIndex);
        return SlaveType::Unknown;
    }

    probe.sendInit(5000);

    // Poll for INIT_READY response (up to 3s)
    unsigned long start = millis();
    while (!probe.isServerReady() && (millis() - start < 3000)) {
        probe.process();
        delay(1);
    }

    if (!probe.isServerReady()) {
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
        SFX_LOG_WARN("Unknown device type: %s", name);
        return SlaveType::Unknown;
    }

    // Now init the proper typed client
    BusClient* client = nullptr;
    switch (type) {
        case SlaveType::GunFX:
            if (gunfxClient.begin(&usbHost, usbIndex)) {
                gunfxClient.sendInit(5000);
                client = &gunfxClient;
            }
            break;
        case SlaveType::LightFX:
            if (lightfxClient.begin(&usbHost, usbIndex)) {
                lightfxClient.sendInit(5000);
                client = &lightfxClient;
            }
            break;
        case SlaveType::GearControl:
            if (gearcontrolClient.begin(&usbHost, usbIndex)) {
                gearcontrolClient.sendInit(5000);
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
    start = millis();
    while (!client->isServerReady() && (millis() - start < 3000)) {
        client->process();
        delay(1);
    }

    if (client->isServerReady()) {
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
void scanAndInitSlaves() {
    int devCount = usbHost.cdcDeviceCount();
    SFX_LOG_DEBUG("Scanning %d USB CDC devices for slaves...", devCount);
    for (int i = 0; i < devCount; i++) {
        const CdcDeviceInfo* info = usbHost.getCdcDevice(i);
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

// ============================================================================
// Audio Initialization
// ============================================================================

bool initAudio() {
    SFX_LOG_INFO("Initializing audio system...");

    if (codec) {
        #if defined(USE_TAS5825_CODEC)
        if (!audioCodec.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL, AUDIO_SAMPLE_RATE)) {
            SFX_LOG_ERROR("TAS5825M codec init failed (I2C SDA=%d SCL=%d)", PIN_I2C_SDA, PIN_I2C_SCL);
            return false;
        }
        audioCodec.setVolume(0.7f);

        #elif defined(USE_PICOAUDIO_CODEC) || defined(USE_SIMPLE_I2S_CODEC)
        if (!audioCodec.begin(AUDIO_SAMPLE_RATE)) {
            SFX_LOG_ERROR("%s codec init failed", audioCodec.getModelName());
            return false;
        }
        #endif
        SFX_LOG_INFO("%s codec initialized", codec->getModelName());
    }

    if (!mixer.begin(PIN_I2S_DATA, PIN_I2S_BCLK, PIN_I2S_LRCLK, codec)) {
        SFX_LOG_ERROR("Audio mixer init failed (I2S DATA=%d BCLK=%d LRCLK=%d)",
                      PIN_I2S_DATA, PIN_I2S_BCLK, PIN_I2S_LRCLK);
        return false;
    }

    mixer.setVolume(-1, 0.8f);
    SFX_LOG_INFO("Audio system ready (master vol 80%%)");
    return true;
}

// ============================================================================
// SD Card Initialization
// ============================================================================

bool initSdCard() {
    const uint8_t speeds[] = {20, 15, 10, 5};
    for (uint8_t i = 0; i < sizeof(speeds); i++) {
        SFX_LOG_DEBUG("SD init attempt at %d MHz...", speeds[i]);
        sdCard.begin(PIN_SD_CS, PIN_SD_SCK, PIN_SD_MOSI, PIN_SD_MISO, speeds[i]);
        if (sdCard.isInitialized()) {
            SFX_LOG_INFO("SD card ready at %d MHz", speeds[i]);
            return true;
        }
        delay(100);
    }
    SFX_LOG_WARN("SD card not found (tried 20/15/10/5 MHz)");
    return false;
}

// ============================================================================
// Arduino Setup (Core 0)
// ============================================================================

void setup() {
    // PicoServer handles serial init, device naming, indicators, core protocol
    server.begin("HubFX", FIRMWARE_VERSION, BUILD_NUMBER);

    SFX_LOG_INFO("HubFX v%s build %d booting...", FIRMWARE_VERSION, BUILD_NUMBER);

    // HubFX is the master â€” it auto-initializes on boot and operates
    // independently of any upstream serial connection. Serial (USB CDC to PC)
    // is optional and used only for configuration / troubleshooting.
    server.setConnectionTimeoutEnabled(false);

    server.onInit([]() {
        // PC sent INIT â€” starting a configuration/debug session.
        // Re-scan slaves so PC gets fresh state.
        SFX_LOG_INFO("INIT received from PC â re-scanning slaves");
        if (core1Ready) {
            scanAndInitSlaves();
        }
    });

    server.onShutdown([]() {
        // PC session ended â no effect on hub operation.
        SFX_LOG_INFO("SHUTDOWN received â hub continues operating");
    });

    // SD card
    sdInitialized = initSdCard();

    // Load configuration
    if (sdInitialized) {
        if (configReader.begin() && configReader.load("/config.yaml")) {
            SFX_LOG_INFO("Config loaded from /config.yaml");
        } else {
            configReader.loadDefaults();
            SFX_LOG_WARN("Config file not found, using defaults");
        }
    } else {
        configReader.loadDefaults();
        SFX_LOG_INFO("No SD card, using default config");
    }

    // Audio
    audioInitialized = initAudio();

    // USB Host setup (Core 1 will call init())
    if (usbHost.begin()) {
        usbHost.onMount(onUsbMount);
        usbHost.onUnmount(onUsbUnmount);
        SFX_LOG_INFO("USB Host configured (slave bus ready)");
    } else {
        SFX_LOG_ERROR("USB Host setup failed");
    }

    // Pre-register slave slots (clients will be bound when devices are discovered)
    // The registry just tracks the relationship; actual connection happens on USB mount
    slaveRegistry.registerSlave(SlaveType::GunFX, &gunfxClient, -1);
    slaveRegistry.registerSlave(SlaveType::LightFX, &lightfxClient, -1);
    slaveRegistry.registerSlave(SlaveType::GearControl, &gearcontrolClient, -1);

    // Relay slave log messages into HubFX's own diagnostic log buffer
    gunfxClient.onLogMessage([](uint8_t level, uint32_t ts, const char* msg) {
        char buf[160];
        snprintf(buf, sizeof(buf), "[GunFX] %s", msg);
        DiagLog::instance().ingest(level, buf);
    });
    lightfxClient.onLogMessage([](uint8_t level, uint32_t ts, const char* msg) {
        char buf[160];
        snprintf(buf, sizeof(buf), "[LightFX] %s", msg);
        DiagLog::instance().ingest(level, buf);
    });
    gearcontrolClient.onLogMessage([](uint8_t level, uint32_t ts, const char* msg) {
        char buf[160];
        snprintf(buf, sizeof(buf), "[GearCtrl] %s", msg);
        DiagLog::instance().ingest(level, buf);
    });

    // Domain-specific command handlers â€” inject dependencies per module
    slaveServer.begin(&Serial);

    audioServer.begin(&Serial);
    audioServer.setAudioMixer(&mixer);

    engineServer.begin(&Serial);
    engineServer.setEngineFX(&engineFx);

    storageServer.begin(&Serial);
    storageServer.setConfigReader(&configReader);

    SFX_LOG_DEBUG("Domain handlers initialized (slave, audio, engine, storage)");

    // STATUS callback â€” report hub-level info
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 8) return 0;

        // Hub status: [slaveCount:u8][slaveMask:u8][usbDevices:u8][sdOk:u8][audioOk:u8]
        // slaveMask: bit0=GunFX, bit1=LightFX, bit2=GearControl (set if ready)
        uint8_t readyCount = 0;
        uint8_t readyMask = 0;
        for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
            if (slaveRegistry[i].ready) {
                readyCount++;
                readyMask |= (1 << ((uint8_t)slaveRegistry[i].type - 1));
            }
        }

        buf[0] = readyCount;
        buf[1] = readyMask;
        buf[2] = (uint8_t)usbHost.cdcDeviceCount();
        buf[3] = sdInitialized ? 1 : 0;
        buf[4] = audioInitialized ? 1 : 0;
        buf[5] = pcSerialConnected ? 1 : 0;  // PC connected over USB CDC
        // Reserved
        buf[6] = 0;
        buf[7] = 0;
        return 8;
    });

    // Register domain-specific handlers with PicoServer's CommandRouter
    // Handler chain: CoreCommandServer (0xF0-0xFF)
    //              â†’ SlaveServer    (0x80-0x83 mgmt + 0x96-0x98 subcmd routing)
    //              â†’ AudioServer    (0x84-0x8B)
    //              â†’ EngineServer   (0x8C-0x8F)
    //              â†’ StorageServer  (0x90-0xA6)
    server.addModuleHandler(&slaveServer);
    server.addModuleHandler(&audioServer);
    server.addModuleHandler(&engineServer);
    server.addModuleHandler(&storageServer);

    // Hub is the master â€” mark as operational immediately (not waiting for INIT)
    server.indicators().setConnected(true);

    SFX_LOG_INFO("HubFX ready \xe2\x80\x94 autonomous master mode (SD:%s audio:%s)",
                 sdInitialized ? "OK" : "NO", audioInitialized ? "OK" : "NO");

    // Play init sound
    if (audioInitialized && sdInitialized) {
        AudioPlaybackOptions opts;
        opts.volume = 0.8f;
        opts.loop = false;
        mixer.playAsync(SystemSounds::CHANNEL, SystemSounds::HUBFX_INITIALIZED, opts);
        SFX_LOG_DEBUG("Init sound queued on channel %d", SystemSounds::CHANNEL);
    }
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

// Slave polling interval
static unsigned long lastSlavePoll_ms = 0;
static constexpr unsigned long SLAVE_POLL_INTERVAL_ms = 100;

// Discovery scan interval
static unsigned long lastDiscoveryScan_ms = 0;
static constexpr unsigned long DISCOVERY_SCAN_INTERVAL_ms = 5000;

void loop() {
    // ================================================================
    // PC Serial Detection
    // ================================================================
    // On RP2040, (bool)Serial checks tud_cdc_connected() â€” returns true
    // when a PC terminal has opened the USB CDC port (DTR/RTS asserted).
    // Serial is optional for HubFX; all normal operations continue without it.

    bool serialNow = (bool)Serial;
    if (serialNow != pcSerialConnected) {
        pcSerialConnected = serialNow;
        if (pcSerialConnected) {
            // PC just connected â€” reset activity timer so timeout doesn't
            // fire immediately from stale timestamps
            server.core().updateActivity();
            SFX_LOG_INFO("PC serial connected");
        } else {
            SFX_LOG_INFO("PC serial disconnected");
        }
    }

    // ================================================================
    // Serial Protocol Processing (only when PC is connected)
    // ================================================================
    if (pcSerialConnected) {
        server.loop();
    }

    // Hub is always operational â€” keep indicators and RAM stats current
    // even when no PC is connected. Re-assert connected=true to override
    // any PicoServer shutdown side-effects (e.g. explicit SHUTDOWN cmd).
    server.indicators().setConnected(true);
    server.core().updateFreeRam(rp2040.getFreeHeap());
    server.indicators().update();

    // ================================================================
    // Slave Client Polling (always active)
    // ================================================================
    unsigned long now = millis();
    if (now - lastSlavePoll_ms >= SLAVE_POLL_INTERVAL_ms) {
        lastSlavePoll_ms = now;

        for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
            SlaveEntry& slave = slaveRegistry[i];
            if (slave.ready && slave.client) {
                slave.client->process();
            }
        }
    }

    // Periodic slave discovery scan (find newly connected devices)
    if (core1Ready && now - lastDiscoveryScan_ms >= DISCOVERY_SCAN_INTERVAL_ms) {
        lastDiscoveryScan_ms = now;

        // Check if any USB devices appeared that we haven't identified yet
        if (usbHost.cdcDeviceCount() > 0) {
            for (uint8_t i = 0; i < slaveRegistry.count(); i++) {
                if (!slaveRegistry[i].ready) {
                    // There's an uninitialized slot â€” try scanning
                    scanAndInitSlaves();
                    break;
                }
            }
        }
    }

    // Engine FX processing (local audio effects)
    engineFx.process();

    delay(1);
}
