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

#define FIRMWARE_VERSION "0.32.0"
#define BUILD_NUMBER 177

// ============================================================================
// FEATURE FLAGS — Board bring-up: uncomment to enable features one by one
// ============================================================================
// #define FEATURE_CONFIG       // Config store (YAML from flash) — needs flash
#define FEATURE_AUDIO        // Audio mixer + TAS5825M codec + I2S + Core 1 — needs FEATURE_I2C
#define FEATURE_USB_HOST     // USB Host + slave management
// #define FEATURE_ENGINE       // Engine FX — needs FEATURE_AUDIO + FEATURE_CONFIG


#include <Arduino.h>
#include <atomic>
#include <esp_system.h>

// Platform abstraction (ESP32-S3 specific macros, mutexes, delays)
#include <platform/sfx_platform.h>
#include <platform/diag_log.h>

// Shared serial protocol library
#include <serial/serial.h>
#include <server/sfx_server.h>

#include <Wire.h>
#include <power/ina226.h>
#include <gpio/pcal6416a.h>

// Config schemas
#ifdef FEATURE_CONFIG
#include "config/hubfx_config.h"
#include <config/config_store.h>
#include <server/config_server.h>
#endif

// USB Host (CDC-ACM for slave controller communication)
#ifdef FEATURE_USB_HOST
#include <usb/sfx_usb_host.h>
#endif

// Storage (LittleFS flash + SD_MMC SDIO)
#include <storage/flash.h>
#include <storage/sd_card.h>
#include <server/storage_server.h>
#ifdef FEATURE_USB_HOST
#include "protocol/hubfx_usb_server.h"
#include "protocol/slave_server.h"
#include "protocol/slave_manager.h"
#endif

// Slave client classes (one per controller type)
#ifdef FEATURE_USB_HOST
#include <gunfx/client/gunfx_client.h>
#include <lightfx/client/lightfx_client.h>
#include <gearcontrol/client/gearcontrol_client.h>
#endif

// Audio mixer and codec (8-channel WAV mixer with I2S output)
#ifdef FEATURE_AUDIO
#include <audio/audio_log.h>
#include <server/audio_server.h>
#include "hubfx_audio.h"
using AudioServer = AudioServerT<Mixer>;
#endif

// Engine FX (sound effects state machine + protocol handler)
#ifdef FEATURE_ENGINE
#include "effects/engine_fx.h"
#include "protocol/engine_server.h"
#endif

// Config store and server type aliases
#ifdef FEATURE_CONFIG
using HubFxConfigStore  = ConfigStore<HubFxConfigSchema>;
using HubFxConfigServer = ConfigServerT<HubFxConfigStore>;
#endif

// ============================================================================
// Pin Definitions (ESP32-S3 DevKitC-1)
// ============================================================================

// Indicator LEDs (directly driven GPIO)
// DevKitC-1 N8R8 has a user-addressable RGB LED on GPIO48 (active-high).
// Use the onboard LED for connection status. Error LED disabled (no external LED).
#define PIN_LED_CONNECTION  48   // Onboard RGB LED (connection status)
#define PIN_LED_ERROR       -1   // Disabled (no external error LED)

// I2S Audio Output — TAS5825M codec
#define PIN_I2S_DOUT    1    // I2S serial data output (TAS_DI — data to codec)
#define PIN_I2S_BCLK    4    // I2S bit clock (TAS_BCK)
#define PIN_I2S_LRCLK   3    // I2S word clock / frame sync (TAS_FS)
// Note: GPIO2 = TAS_DO (data from codec to ESP32) — not used (output only)

// SD Card (SD_MMC SDIO — currently 1-bit, D1/D2 wired for future 4-bit)
#define PIN_SD_MMC_CMD  38   // SD_MMC command
#define PIN_SD_MMC_CLK  39   // SD_MMC clock
#define PIN_SD_MMC_D0   40   // SD_MMC data 0
#define PIN_SD_MMC_D1   41   // SD_MMC data 1 (4-bit only)
#define PIN_SD_MMC_D2   42   // SD_MMC data 2 (4-bit only)

// I2C (for TAS5825M codec control, power monitoring, etc.)
#define PIN_I2C_SDA     8    // I2C data  (to TAS5825M SDA)
#define PIN_I2C_SCL     9    // I2C clock (to TAS5825M SCL)

// TAS5825M control pins on PCAL6416A expander (Port 1)
// Pin numbers are 0-15: 0-7 = Port 0, 8-15 = Port 1
#define EXP_TAS_FAULT   8    // P1_0 — TAS5825M nFAULT (active-low, input)
#define EXP_TAS_MUTE    9    // P1_1 — TAS5825M MUTE (active-low, output: HIGH=unmuted)
#define EXP_TAS_PDN    10    // P1_2 — TAS5825M PDN/!RST (active-low, output: HIGH=run)

// ============================================================================
// Core 1 Task — Audio Consumer (highest priority on Core 1)
// ============================================================================

#ifdef FEATURE_AUDIO

// FreeRTOS task handle for Core 1 consumer
static TaskHandle_t core1TaskHandle = nullptr;

// Cross-core state flags
std::atomic<bool> audioInitialized{false};   // Core 0 writes, Core 1 reads
std::atomic<bool> core1Ready{false};         // Core 1 writes, Core 0 reads
std::atomic<bool> i2sReady{false};           // Core 1 writes, Core 0 reads — I2S clocks running
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
    i2sReady.store(true, std::memory_order_release);  // Tell Core 0 clocks are live

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

#endif // FEATURE_AUDIO

// ============================================================================
// Boot & Codec Helpers
// ============================================================================

/**
 * @brief Log the ESP32 reset reason (called once from setup).
 */
static void logResetReason() {
    struct ReasonEntry { esp_reset_reason_t code; const char* name; };
    static constexpr ReasonEntry reasons[] = {
        { ESP_RST_POWERON,   "POWER_ON"   },
        { ESP_RST_EXT,       "EXTERNAL"   },
        { ESP_RST_SW,        "SOFTWARE"   },
        { ESP_RST_PANIC,     "PANIC"      },
        { ESP_RST_INT_WDT,   "INT_WDT"    },
        { ESP_RST_TASK_WDT,  "TASK_WDT"   },
        { ESP_RST_WDT,       "OTHER_WDT"  },
        { ESP_RST_DEEPSLEEP, "DEEPSLEEP"  },
        { ESP_RST_BROWNOUT,  "BROWNOUT"   },
        { ESP_RST_SDIO,      "SDIO"       },
        { ESP_RST_USB,       "USB"        },
    };

    esp_reset_reason_t reason = esp_reset_reason();
    const char* name = "UNKNOWN";
    for (const auto& r : reasons) {
        if (r.code == reason) { name = r.name; break; }
    }

    SFX_LOG_INFO("Boot reason: %s (%d)", name, (int)reason);
    if (reason == ESP_RST_BROWNOUT)
        SFX_LOG_ERROR("*** BROWNOUT RESET — possible USB hub inrush current issue ***");
    else if (reason == ESP_RST_PANIC)
        SFX_LOG_ERROR("*** PANIC RESET — check backtrace in serial monitor ***");
    else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT)
        SFX_LOG_ERROR("*** WATCHDOG RESET — a task may have hung ***");
}

// GPIO expander — declared early so both FEATURE_AUDIO diagnostics
// and initI2CDevices() can reference it.
static PCAL6416A gpioExpander;

#ifdef FEATURE_AUDIO
/**
 * @brief Phase 1: Probe I2C, reset codec, enter Deep Sleep.
 *
 * Safe to call before I2S clocks are running.  The codec's PLL is off in
 * Deep Sleep so it won't fault from missing clocks.
 *
 * @return true if codec I2C probe and reset succeeded.
 */
static bool tryInitCodec(TAS5825M_SupplyVoltage voltage) {
    TAS5825Codec& codec = TAS5825Codec::instance();
    return codec.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL, AUDIO_SAMPLE_RATE, voltage);
}

/**
 * @brief Phase 2: Configure registers and transition to PLAY.
 *
 * MUST be called AFTER I2S BCLK/LRCLK are running.
 * Configures gain, clock regs, DSP, then Deep Sleep → HIZ → PLAY.
 *
 * @return true if codec entered PLAY state.
 */
static bool activateCodec() {
    TAS5825Codec& codec = TAS5825Codec::instance();
    if (!codec.isInitialized()) return false;
    return codec.activate();
}

// TAS5825M additional diagnostic registers (not all defined in the driver header)
// See TAS5825M datasheet §7.6
static constexpr uint8_t TAS_REG_POWER_STATE    = 0x68;  // Actual power state [1:0]
static constexpr uint8_t TAS_REG_AUTOMUTE_STATE = 0x69;  // Automute status
static constexpr uint8_t TAS_REG_CHAN_FAULT     = 0x70;  // Channel fault (OC, DC)
static constexpr uint8_t TAS_REG_GLOBAL_FAULT1  = 0x71;  // PVDD OV/UV, clock fault
static constexpr uint8_t TAS_REG_GLOBAL_FAULT2  = 0x72;  // Over-temp, CBUCK
static constexpr uint8_t TAS_REG_OT_WARNING     = 0x73;  // Over-temp warning
static constexpr uint8_t TAS_REG_PIN_CONTROL    = 0x61;  // Pin config / FS detect
static constexpr uint8_t TAS_REG_FS_MON         = 0x37;  // Sample rate monitor

/**
 * @brief Comprehensive audio hardware diagnostics.
 *
 * Reads GPIO expander pins (PDN, MUTE, FAULT) and TAS5825M internal
 * fault/state registers. Logs everything via DiagLog for CLI retrieval.
 * Call from initAudio() and periodic diagnostics.
 */
static void diagnoseAudioHardware() {
    SFX_LOG_INFO("=== Audio Hardware Diagnostics ===");

    // ---- GPIO Expander: TAS control pins ----
    if (gpioExpander.isAvailable()) {
        bool pdnState   = gpioExpander.readPin(EXP_TAS_PDN);
        bool muteState  = gpioExpander.readPin(EXP_TAS_MUTE);
        bool faultState = gpioExpander.readPin(EXP_TAS_FAULT);
        uint8_t port1Dir = gpioExpander.getPortDirection(1);
        uint8_t port1Out = gpioExpander.readPort(1);

        SFX_LOG_INFO("Expander P1: dir=0x%02X out=0x%02X", port1Dir, port1Out);
        SFX_LOG_INFO("  TAS_PDN  (P1_2): %s  [%s]",
                     pdnState ? "HIGH (run)" : "LOW (SHUTDOWN!)",
                     (port1Dir & 0x04) ? "input" : "output");
        SFX_LOG_INFO("  TAS_MUTE (P1_1): %s  [%s]",
                     muteState ? "HIGH (unmuted)" : "LOW (MUTED!)",
                     (port1Dir & 0x02) ? "input" : "output");
        SFX_LOG_INFO("  TAS_FAULT(P1_0): %s  [%s]",
                     faultState ? "HIGH (no fault)" : "LOW (FAULT!)",
                     (port1Dir & 0x01) ? "input" : "output");

        if (!pdnState)
            SFX_LOG_ERROR("*** TAS5825M in POWER-DOWN — PDN pin is LOW ***");
        if (!muteState)
            SFX_LOG_WARN("*** TAS5825M hardware MUTE active ***");
        if (!faultState)
            SFX_LOG_ERROR("*** TAS5825M FAULT asserted ***");
    } else {
        SFX_LOG_WARN("GPIO expander not available — cannot read TAS pins");
    }

    // ---- TAS5825M I2C registers ----
    TAS5825Codec& codec = TAS5825Codec::instance();
    if (codec.isInitialized()) {
        uint8_t devCtrl     = codec.getDeviceControlRegister();
        uint8_t faultReg    = codec.getFaultRegister();

        // Read additional diagnostic registers directly via I2C
        // (using the codec's test method to verify bus)
        uint8_t powerState = 0, automuteState = 0;
        uint8_t chanFault = 0, globalFault1 = 0, globalFault2 = 0, otWarning = 0;
        uint8_t fsMon = 0;

        // These reads go through Wire directly (codec doesn't expose all regs)
        Wire.beginTransmission(TAS5825M_I2C_ADDR);
        Wire.write(0x00); Wire.write(0x00);  // Select Book 0, Page 0
        Wire.endTransmission();
        Wire.beginTransmission(TAS5825M_I2C_ADDR);
        Wire.write(0x7F); Wire.write(0x00);
        Wire.endTransmission();

        auto readReg = [](uint8_t reg, uint8_t& val) -> bool {
            Wire.beginTransmission(TAS5825M_I2C_ADDR);
            Wire.write(reg);
            if (Wire.endTransmission(false) != 0) return false;
            if (Wire.requestFrom(TAS5825M_I2C_ADDR, (uint8_t)1) != 1) return false;
            val = Wire.read();
            return true;
        };

        readReg(TAS_REG_POWER_STATE, powerState);
        readReg(TAS_REG_AUTOMUTE_STATE, automuteState);
        readReg(TAS_REG_CHAN_FAULT, chanFault);
        readReg(TAS_REG_GLOBAL_FAULT1, globalFault1);
        readReg(TAS_REG_GLOBAL_FAULT2, globalFault2);
        readReg(TAS_REG_OT_WARNING, otWarning);
        readReg(TAS_REG_FS_MON, fsMon);

        // Device control state
        const char* ctrlStr = "?";
        switch (devCtrl & 0x03) {
            case 0x00: ctrlStr = "DEEP_SLEEP"; break;
            case 0x01: ctrlStr = "SLEEP"; break;
            case 0x02: ctrlStr = "HIZ"; break;
            case 0x03: ctrlStr = "PLAY"; break;
        }
        SFX_LOG_INFO("  DEVICE_CTRL (0x03): 0x%02X → %s", devCtrl, ctrlStr);

        // Power state (actual, may differ from requested)
        const char* pwrStr = "?";
        switch (powerState & 0x03) {
            case 0x00: pwrStr = "DEEP_SLEEP"; break;
            case 0x01: pwrStr = "SLEEP"; break;
            case 0x02: pwrStr = "HIZ"; break;
            case 0x03: pwrStr = "PLAY"; break;
        }
        SFX_LOG_INFO("  POWER_STATE(0x68): 0x%02X → %s", powerState, pwrStr);
        if ((powerState & 0x03) != 0x03)
            SFX_LOG_WARN("*** Not in PLAY state — amplifier output disabled ***");

        // Automute
        SFX_LOG_INFO("  AUTOMUTE  (0x69): 0x%02X %s", automuteState,
                     (automuteState & 0x03) ? "(channels automuted!)" : "(not automuted)");

        // Volume
        SFX_LOG_INFO("  DIGITAL_VOL(0x4C): 0x%02X (%.1f dB)",
                     codec.getVolumeRegister(),
                     ((float)codec.getVolumeRegister() - 48.0f) * 0.5f);
        SFX_LOG_INFO("  Muted (SW): %s", codec.getMuted() ? "YES" : "NO");

        // Faults
        SFX_LOG_INFO("  FAULT_CLR (0x78): 0x%02X", faultReg);
        SFX_LOG_INFO("  CHAN_FAULT(0x70): 0x%02X%s", chanFault,
                     chanFault ? " — CHANNEL FAULT!" : "");
        if (chanFault) {
            if (chanFault & 0x01) SFX_LOG_ERROR("    bit0: Left OC");
            if (chanFault & 0x02) SFX_LOG_ERROR("    bit1: Right OC");
            if (chanFault & 0x04) SFX_LOG_ERROR("    bit2: Left DC");
            if (chanFault & 0x08) SFX_LOG_ERROR("    bit3: Right DC");
        }
        SFX_LOG_INFO("  GLOBAL1   (0x71): 0x%02X%s", globalFault1,
                     globalFault1 ? " — GLOBAL FAULT!" : "");
        if (globalFault1) {
            if (globalFault1 & 0x01) SFX_LOG_ERROR("    bit0: PVDD OV");
            if (globalFault1 & 0x02) SFX_LOG_ERROR("    bit1: PVDD UV");
            if (globalFault1 & 0x04) SFX_LOG_ERROR("    bit2: Clock fault");
            if (globalFault1 & 0x08) SFX_LOG_ERROR("    bit3: BQ fault");
        }
        SFX_LOG_INFO("  GLOBAL2   (0x72): 0x%02X%s", globalFault2,
                     globalFault2 ? " — GLOBAL FAULT 2!" : "");
        if (globalFault2) {
            if (globalFault2 & 0x01) SFX_LOG_ERROR("    bit0: OT shutdown");
            if (globalFault2 & 0x08) SFX_LOG_ERROR("    bit3: OTW");
        }
        SFX_LOG_INFO("  OT_WARN   (0x73): 0x%02X", otWarning);
        SFX_LOG_INFO("  FS_MON    (0x37): 0x%02X", fsMon);
    } else {
        SFX_LOG_WARN("TAS5825M not initialized — I2C registers not readable");
        SFX_LOG_INFO("  I2C bus test: %s", codec.testI2CConnection() ? "ACK" : "NO ACK");
    }

    // ---- Audio Mixer state ----
    Mixer& mixer = Mixer::instance();
    SFX_LOG_INFO("  Mixer init: %s  I2S: %s  Playing: %s",
                 mixer.isInitialized() ? "yes" : "NO",
                 mixer.isI2SRunning() ? "yes" : "NO",
                 mixer.isAnyPlaying() ? "yes" : "no");
    SFX_LOG_INFO("  Ring: %d%%  Underruns: %lu  Frames: %lu",
                 mixer.getRingFillPercent(),
                 (unsigned long)mixer.getUnderruns(),
                 (unsigned long)mixer.getConsumeFrames());
    SFX_LOG_INFO("=== End Audio Diagnostics ===");
}
#endif // FEATURE_AUDIO

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

    SFX_LOG_DEBUG("uptime=%lus heap=%lu", uptime_s, heap);

#ifdef FEATURE_AUDIO
    SFX_LOG_DEBUG("core1=%s loop1=%lu",
                  core1Ready.load(std::memory_order_acquire) ? "ready" : "NOT_READY",
                  loop1Count.load(std::memory_order_relaxed));

    Mixer& mixer = Mixer::instance();
    if (mixer.isInitialized()) {
        SFX_LOG_DEBUG("audio: i2s=%s ring=%d%% underruns=%lu playing=%s",
                      mixer.isI2SRunning() ? "on" : "off",
                      mixer.getRingFillPercent(),
                      (unsigned long)mixer.getUnderruns(),
                      mixer.isAnyPlaying() ? "yes" : "no");
    }
#endif

#ifdef FEATURE_USB_HOST
    UsbHost& usb = UsbHost::instance();
    SFX_LOG_DEBUG("usb=%s cdc=%d",
                  SlaveManager::instance().isUsbReady() ? "ready" : "off",
                  usb.cdcDeviceCount());
#endif
}

// ============================================================================
// Global Objects
// ============================================================================

// Server infrastructure (upstream protocol handling over UART0)
SfxServer server;

// I2C peripherals
static constexpr uint8_t INA226_COUNT = 6;
static INA226 ina226[INA226_COUNT];
static const uint8_t ina226Addrs[INA226_COUNT] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45 };
// gpioExpander declared earlier (before FEATURE_AUDIO section)

// Module protocol handlers
StorageServer storageServer;
#ifdef FEATURE_USB_HOST
HubFxUsbServer usbServer;
SlaveServer slaveServer;
#endif
#ifdef FEATURE_AUDIO
AudioServer audioServer;
#endif
#ifdef FEATURE_CONFIG
HubFxConfigServer configServer;
#endif
#ifdef FEATURE_ENGINE
EngineServer engineServer;
#endif

// ============================================================================
// Flash File I/O Bridges (for ConfigStore)
// ============================================================================

#ifdef FEATURE_CONFIG
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
#endif // FEATURE_CONFIG

// Typed slave clients (file-scope, registered with SlaveManager via addSlave)
#ifdef FEATURE_USB_HOST
static GunFxClient gunfxClient;
static LightFxClient lightfxClient;
static GearControlClient gearcontrolClient;
#endif

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

        SFX_LOG_INFO("SD card ready: %s %lu MB (total=%lu free=%lu used=%lu, SDIO 1-bit HS)",
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
#ifdef FEATURE_CONFIG
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
#endif // FEATURE_CONFIG

/** @brief Register module protocol handlers and build the handler chain. */
static void initProtocolHandlers() {
    storageServer.begin(&Serial);
#ifdef FEATURE_USB_HOST
    usbServer.begin(&Serial);
    slaveServer.begin(&Serial);
#endif
#ifdef FEATURE_AUDIO
    audioServer.begin(&Serial);
#endif
#ifdef FEATURE_CONFIG
    configServer.begin(&Serial);
#endif
#ifdef FEATURE_ENGINE
    engineServer.begin(&Serial);
#endif

    // Handler chain: CoreCommandServer (0xF0-0xFF)
    //              → ConfigServer         (0x90-0x92, 0xAC config)
    //              → SlaveServer          (0x80-0x83 mgmt + 0x96-0x98 routing)
    //              → HubFxUsbServer       (0xA7-0xA8 USB diag)
    //              → AudioServer          (0x84-0x8B audio)
    //              → EngineServer         (0x8C-0x8F engine FX)
    //              → StorageServer        (0x93-0xA6 SD/flash + files)
#ifdef FEATURE_CONFIG
    server.addModuleHandler(&configServer);
#endif
#ifdef FEATURE_USB_HOST
    server.addModuleHandler(&slaveServer);
    server.addModuleHandler(&usbServer);
#endif
#ifdef FEATURE_AUDIO
    server.addModuleHandler(&audioServer);
#endif
#ifdef FEATURE_ENGINE
    server.addModuleHandler(&engineServer);
#endif
    server.addModuleHandler(&storageServer);

#ifdef FEATURE_AUDIO
    // Suspend audio during stream uploads to free Core 1 for SD writes.
    storageServer.onStreamStart([]() {
        Mixer::instance().suspendAudio();
    });

    storageServer.onStreamEnd([]() {
        Mixer::instance().resumeAudio();
    });
#endif

    // Suppress STATUS_UPDATE during any file transfer (list, tree,
    // download, upload) to keep the serial channel exclusive.
    storageServer.onTransferStart([]() {
        server.core().setTransferActive(true);
    });

    storageServer.onTransferEnd([]() {
        server.core().setTransferActive(false);
    });
}

/** @brief Register slave types with SlaveManager and initialize USB Host. */
#ifdef FEATURE_USB_HOST
static void initSlaveManager() {
    SlaveManager& mgr = SlaveManager::instance();
    mgr.addSlave({ SlaveType::GunFX,       "GunFX",       "GunFX",   &gunfxClient });
    mgr.addSlave({ SlaveType::LightFX,     "LightFX",     "LightFX", &lightfxClient });
    mgr.addSlave({ SlaveType::GearControl, "GearControl", "GearCtrl", &gearcontrolClient });
    mgr.begin();
}
#endif // FEATURE_USB_HOST

// NOTE: Engine FX is initialized by the config onLoaded callback
// (EngineFX::applyConfig) — no separate initEngineFx() needed.

/** @brief Initialize I2C peripherals: INA226 power monitors and PCAL6416A GPIO expander. */
static void initI2CDevices() {
    // INA226 power monitors at 0x40-0x45
    // Default calibration: 100mΩ shunt, 3.2A max (adjust per board design)
    uint8_t inaCount = 0;
    for (uint8_t i = 0; i < INA226_COUNT; i++) {
        if (ina226[i].begin(Wire, ina226Addrs[i], 0.1f, 3.2f)) {
            inaCount++;
            SFX_LOG_INFO("INA226[%d] @ 0x%02X: OK (MFG=0x%04X DIE=0x%04X)",
                         i, ina226Addrs[i],
                         ina226[i].manufacturerId(), ina226[i].dieId());
        } else {
            SFX_LOG_WARN("INA226[%d] @ 0x%02X: not found", i, ina226Addrs[i]);
        }
    }
    SFX_LOG_INFO("INA226: %d/%d monitors initialized", inaCount, INA226_COUNT);

    // PCAL6416AHF GPIO expander at 0x20
    if (gpioExpander.begin(Wire, PCAL6416AAddress::DEFAULT_ADDR)) {
        SFX_LOG_INFO("PCAL6416A @ 0x%02X: OK (16-bit GPIO expander)",
                     gpioExpander.address());

        // Configure TAS5825M control pins on Port 1:
        //   P1_0 (FAULT) = input with pull-up (active-low fault output from TAS)
        //   P1_1 (MUTE)  = output, drive HIGH (unmuted)
        //   P1_2 (PDN)   = output, drive HIGH (power-on / run)
        gpioExpander.setPinDirection(EXP_TAS_FAULT, true);   // input
        gpioExpander.setPullEnable(1, 0x01);                 // enable pull on P1_0
        gpioExpander.setPullSelect(1, 0x01);                 // pull-up on P1_0

        gpioExpander.setPinDirection(EXP_TAS_PDN, false);    // output
        gpioExpander.writePin(EXP_TAS_PDN, true);            // PDN = HIGH → run
        SFX_LOG_INFO("  TAS_PDN  (P1_2) → HIGH (power-on)");

        gpioExpander.setPinDirection(EXP_TAS_MUTE, false);   // output
        gpioExpander.writePin(EXP_TAS_MUTE, true);           // MUTE = HIGH → unmuted
        SFX_LOG_INFO("  TAS_MUTE (P1_1) → HIGH (unmuted)");

        SFX_LOG_INFO("  TAS_FAULT(P1_0) = %s",
                     gpioExpander.readPin(EXP_TAS_FAULT) ? "HIGH (ok)" : "LOW (FAULT!)");
    } else {
        SFX_LOG_WARN("PCAL6416A @ 0x%02X: not found",
                     PCAL6416AAddress::DEFAULT_ADDR);
    }
}

#ifdef FEATURE_AUDIO
/**
 * @brief Initialize audio mixer (Phase 1) and launch Core 1 consumer task.
 *
 * Phase 1 (Core 0): channels, ring buffer, codec init via I2C.
 * Phase 2 (Core 1): I2S hardware init + producer task launch.
 */
static void initAudio() {
    Mixer& mixer = Mixer::instance();

    // ---- Phase 1: Codec I2C probe + reset → Deep Sleep ----
    // The codec goes into Deep Sleep (PLL off) — safe before I2S clocks.
    TAS5825M_SupplyVoltage initVoltage = TAS5825M_12V;
#ifdef FEATURE_CONFIG
    if (configServer.store().isLoaded()) {
        TAS5825Codec::parseSupplyVoltage(
            configServer.store().data().audio.codecSupplyVoltage, initVoltage);
    }
#endif
    if (tryInitCodec(initVoltage)) {
        SFX_LOG_INFO("TAS5825M codec probed OK, in Deep Sleep (supply=%s)",
                     TAS5825Codec::supplyVoltageStr(initVoltage));
    } else {
        SFX_LOG_WARN("TAS5825M codec probe failed — will retry periodically "
                     "(check battery/PVDD power)");
    }

    // Dump full audio hardware state after codec probe
    diagnoseAudioHardware();

    // ---- Mixer Phase 1: channels, ring buffer, pin storage ----
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

    // Store consumer task handle in mixer for suspend/resume coordination
    // (used by suspendAudio/resumeAudio during stream uploads)
    mixer.setConsumerTaskHandle(core1TaskHandle);

    // ---- Phase 2: Wait for I2S clocks, then activate codec ----
    // The codec is in Deep Sleep with PLL off. Once Core 1 starts I2S
    // (BCLK/LRCLK running on GPIO pins), we configure registers and
    // transition Deep Sleep → HIZ (PLL lock) → PLAY.
    if (TAS5825Codec::instance().isInitialized()) {
        // Wait up to 2s for Core 1 to finish beginI2S()
        uint32_t waitStart = millis();
        while (!i2sReady.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (millis() - waitStart > 2000) {
                SFX_LOG_ERROR("Timeout waiting for Core 1 I2S init");
                break;
            }
        }
        if (i2sReady.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(50));  // Let I2S clocks stabilize
            if (activateCodec()) {
                SFX_LOG_INFO("TAS5825M activated — PLAY state with I2S clocks");
            } else {
                SFX_LOG_WARN("TAS5825M activate failed — codec not in PLAY");
            }
            diagnoseAudioHardware();  // Verify final state
        }
    }
}
#endif // FEATURE_AUDIO

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

    logResetReason();

    // HubFX is the master — auto-init, no upstream connection timeout
    server.setConnectionTimeoutEnabled(false);

    // Track boot completion time — first INIT right after boot can skip
    // heavy re-initialization since everything is already in a clean state.
    static uint32_t bootComplete_ms = 0;
    static constexpr uint32_t FRESH_BOOT_WINDOW_MS = 5000;  // 5s grace period

    server.onInit([](uint8_t mode, uint8_t flags) {
        (void)flags;  // HubFX accepts both SLAVE and DIRECT
        SFX_LOG_INFO("INIT mode=%s flags=0x%02X", InitMode::getName(mode), flags);
        uint32_t now = millis();

        // If this is the first INIT within the fresh-boot window, everything
        // was just initialized by setup() — skip the expensive re-init.
        if (bootComplete_ms > 0 && (now - bootComplete_ms) < FRESH_BOOT_WINDOW_MS) {
            bootComplete_ms = 0;  // Consume the grace — subsequent INITs do full re-init
            SFX_LOG_INFO("INIT received — fresh boot, skipping re-init");
#ifdef FEATURE_USB_HOST
            SlaveManager::instance().scanAndIdentify();
#endif
            return;
        }

        // PC sent INIT — new session starting.
        // Full re-initialization of all subsystems to ensure clean state.
        SFX_LOG_INFO("INIT received — re-initializing all subsystems");

        // 1. Cancel any upload left over from a previous session.
        storageServer.cancelActiveUpload();

#ifdef FEATURE_AUDIO
        // 2. Stop all audio and reset codec to clean power-on state.
        {
            Mixer& mixer = Mixer::instance();
            if (mixer.isInitialized()) {
                mixer.stopAsync(-1, AudioStopMode::Immediate);
                mixer.setMasterVolumeAsync(1.0f);
                mixer.resetUnderruns();
            }

            TAS5825Codec& codec = TAS5825Codec::instance();
            if (codec.isInitialized()) {
                codec.reset();
                codec.clearFaults();
                SFX_LOG_INFO("Audio codec reset");
            } else if (tryInitCodec(codec.getSupplyVoltage())) {
                activateCodec();
                SFX_LOG_INFO("Audio codec initialized on INIT");
            }
        }
#endif

#ifdef FEATURE_ENGINE
        // 3. Reset engine FX state machine (stops sounds, clears state)
        {
            EngineFX& engine = EngineFX::instance();
            if (engine.isInitialized()) {
                engine.end();
            }
        }
#endif

#ifdef FEATURE_CONFIG
        // 4. Reload config from flash.
        configServer.loadConfig();
#endif

#ifdef FEATURE_USB_HOST
        // 5. Re-scan slaves so PC gets fresh identification
        SlaveManager::instance().scanAndIdentify();
#endif

        SFX_LOG_INFO("INIT complete — all subsystems re-initialized");
    });

    server.onShutdown([]() {
        storageServer.cancelActiveUpload();

#ifdef FEATURE_AUDIO
        {
            Mixer& mixer = Mixer::instance();
            if (mixer.isInitialized() && mixer.isAnyPlaying()) {
                mixer.stopAsync(-1, AudioStopMode::Immediate);
            }
        }
#endif

#ifdef FEATURE_ENGINE
        {
            EngineFX& engine = EngineFX::instance();
            if (engine.isActive()) {
                engine.forceStop();
            }
        }
#endif

        SFX_LOG_INFO("SHUTDOWN — session ended");
    });

    // ---- STATUS callback: module-specific status bytes ----
    // Appended after the 22-byte core header in STATUS responses.
    // Layout: [flags:u8][slaveMask:u8][loop1Count:u32LE]
    //         [i2cDeviceMask:u8][ina226_mV[0..5]:u16LE x 6] = 6 + 13 = 19 bytes
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 19) return 0;

        // flags byte:
        //   bit 0: core1Ready
        //   bit 1: audioInitialized
        //   bit 2: flash initialized
        //   bit 3: USB host ready
        //   bit 4: SD card ready
        uint8_t flags = 0;
#ifdef FEATURE_AUDIO
        if (core1Ready.load(std::memory_order_acquire))      flags |= 0x01;
        if (audioInitialized.load(std::memory_order_acquire)) flags |= 0x02;
#endif
        if (FlashModule::instance().isInitialized())         flags |= 0x04;
#ifdef FEATURE_USB_HOST
        if (SlaveManager::instance().isUsbReady())          flags |= 0x08;
#endif
        if (SdCardModule::instance().isInitialized())        flags |= 0x10;
        buf[0] = flags;

        // Slave presence bitmask: bit0=GunFX, bit1=LightFX, bit2=GearControl
#ifdef FEATURE_USB_HOST
        buf[1] = SlaveManager::instance().slaveMask();
#else
        buf[1] = 0;
#endif

        // Core 1 loop counter (diagnostic)
#ifdef FEATURE_AUDIO
        CoreProtocol::putU32LE(&buf[2], loop1Count.load(std::memory_order_relaxed));
#else
        CoreProtocol::putU32LE(&buf[2], 0);
#endif

        // I2C device presence bitmask (byte 6):
        //   bit 0: PCAL6416A @ 0x20
        //   bit 1: INA226 @ 0x40
        //   bit 2: INA226 @ 0x41
        //   bit 3: INA226 @ 0x42
        //   bit 4: INA226 @ 0x43
        //   bit 5: INA226 @ 0x44
        //   bit 6: INA226 @ 0x45
        //   bit 7: TAS5825M @ 0x4C (reserved)
        uint8_t i2cMask = 0;
        if (gpioExpander.isAvailable()) i2cMask |= 0x01;
        for (uint8_t i = 0; i < INA226_COUNT; i++) {
            if (ina226[i].isAvailable()) i2cMask |= (1 << (i + 1));
        }
        buf[6] = i2cMask;

        // INA226 bus voltage readings (bytes 7-18): 6 x u16LE in mV
        for (uint8_t i = 0; i < INA226_COUNT; i++) {
            uint16_t voltage_mV = 0;
            if (ina226[i].isAvailable()) {
                ina226[i].update();
                voltage_mV = (uint16_t)ina226[i].busVoltage_mV();  // 0-36000 mV
            }
            CoreProtocol::putU16LE(&buf[7 + i * 2], voltage_mV);
        }

        return 19;
    });

    // Initialize I2C bus early — gives maximum stabilization time before
    // TAS5825M codec probe. On ESP32, Wire.begin() installs the I2C master
    // driver and performs bus recovery (SCL toggling) if SDA is stuck low.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);  // 100kHz I2C
    server.enableI2CScan(Wire);
    initI2CDevices();

    initStorage();
    initProtocolHandlers();
#ifdef FEATURE_CONFIG
    initConfig();       // onLoaded callback initializes EngineFX
    server.markConfigLoaded();  // IDLE → STANDALONE
#endif
#ifdef FEATURE_USB_HOST
    initSlaveManager();
#endif
#ifdef FEATURE_AUDIO
    initAudio();
#endif

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

#ifdef FEATURE_AUDIO
static void checkCodecHealth() {
    static uint32_t lastCheck_ms = 0;
    static constexpr uint32_t CHECK_INTERVAL_MS = 5000;  // every 5s
    static uint32_t lastDiag_ms = 0;
    static constexpr uint32_t DIAG_INTERVAL_MS = 30000;  // full diag every 30s

    uint32_t now = millis();

    // If codec not initialized, retry periodically
    TAS5825Codec& codec = TAS5825Codec::instance();
    if (!codec.isInitialized()) {
        if (now - lastCheck_ms < CHECK_INTERVAL_MS) return;
        lastCheck_ms = now;

        SFX_LOG_INFO("Retrying TAS5825M codec init...");
        if (tryInitCodec(codec.getSupplyVoltage())) {
            activateCodec();
            SFX_LOG_INFO("TAS5825M codec initialized on retry — audio amp online");
            diagnoseAudioHardware();
        }
        return;
    }

    // Periodic full audio diagnostic (even when healthy)
    if (now - lastDiag_ms >= DIAG_INTERVAL_MS) {
        lastDiag_ms = now;
        diagnoseAudioHardware();
    }
}
#endif // FEATURE_AUDIO

// ============================================================================
// Arduino Main Loop (Core 1 — default ARDUINO_RUNNING_CORE)
// ============================================================================

void loop() {
    // Stream upload mode: bypass normal COBS processing entirely.
    if (storageServer.isStreamActive()) {
        storageServer.processStream();
    } else {
        server.loop();
    }
    storageServer.checkUploadTimeout();

#ifdef FEATURE_AUDIO
    // Stop audio playback when SD upload starts (shared SD bus)
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
#endif

    // Periodic diagnostic logging
    logDiagnostics();

#ifdef FEATURE_AUDIO
    checkCodecHealth();
#endif

#ifdef FEATURE_USB_HOST
    SlaveManager::instance().process();
#endif

#ifdef FEATURE_ENGINE
    EngineFX::instance().process();
#endif

    if (!storageServer.isUploadActive()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
