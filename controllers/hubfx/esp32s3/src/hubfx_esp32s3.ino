/*
 * HubFX ESP32-S3 — Autonomous Protocol Router Hub
 *
 * Central hub controller for ScaleFX system. Owns audio mixing, engine
 * effects, hub-local LED runtime, RC inputs, storage, and config.
 *
 * DUAL-CORE ARCHITECTURE (ESP32-S3, dual Xtensa LX7 @ 240 MHz):
 *   Core 0: Main loop — serial protocol, storage operations, RC inputs
 *   Core 1: Audio consumer task (I2S DMA output, highest priority)
 *           Audio producer task (WAV decode, SD reads, mixing, lower priority)
 *           USB Host polling
 *
 * COMMUNICATION:
 *   Upstream:  UART0 via USB-UART bridge (1 Mbps) — COBS protocol for
 *              debug/config CLI and DiagLog retrieval
 *   Downstream: USB Host (OTG) — CDC enumeration is live; the layer that
 *               *binds* CDC devices to typed clients (the old slave
 *               manager) was removed 2026-05-16 and will be rebuilt
 *               against the generic-expander model — see
 *               instructions/15-GENERIC-EXPANDER-REFACTOR.md and
 *               instructions/17-SYSTEM-SERVICES.md.
 *
 * Serial architecture (ESP32-S3 DevKitC-1):
 *   The DevKitC-1 has TWO USB connectors:
 *     - USB-UART (CP2102N/CH340): Connected to UART0 → `Serial`
 *       Used for: flashing (esptool), COBS protocol, DiagLog, CLI
 *     - USB-OTG (native USB): Reserved for USB Host mode (expanders)
 *       cdc_on_boot=0 prevents the OTG port from acting as a CDC serial
 *
 * Subsystem status:
 *   [x] DiagLog over UART (SFX_LOG_* macros, DIAG_HISTORY command)
 *   [x] Core protocol (INIT, SHUTDOWN, REBOOT, STATUS, KEEPALIVE, I2C_SCAN)
 *   [x] USB Host (mount/unmount logging, device listing — type-agnostic)
 *   [x] Flash storage (LittleFS, file list/info/delete/mkdir/download/upload)
 *   [x] SD card storage (SD_MMC 4-bit SDIO, file ops, upload/download)
 *   [x] Audio mixer (I2S output via ESP-IDF driver, 8-ch WAV, dual-core)
 *   [x] Config reader (YAML from flash, reload/save/get protocol)
 *   [x] Audio server (protocol handler)
 *   [x] Engine server (protocol handler)
 *   [x] Engine FX
 *   [x] RC input dispatcher (PWM/PPM → engine on/off + light program select)
 *   [x] Local LED runtime — 8-ch PCA9685 (0x70) via the PCA9685 driver
 *       in controllers/lib/sfx_peripherals/pwm/. PwmOutput-concept
 *       adapter scales 8-bit input to the chip's 12-bit duty.
 *   [ ] Generic-expander routing + per-port GUID identity (replaces the
 *       removed slave-management layer)
 *   [ ] Gun FX
 *   [ ] System sounds
 *   [ ] SBUS / Jeti EX Bus input readers (Phase 0 stubs in place)
 */

#define FIRMWARE_VERSION "1.3.0"
#define BUILD_NUMBER 246

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
#include <power/ina226_battery.h>
#include <power/battery_server.h>
#include <pwm/pca9685.h>

// Config schemas — per-domain YAML files (Rule 26)
#include "config/hubfx_config.h"
#include <server/multi_config_server.h>

// USB Host (CDC-ACM for slave controller communication)
#include <usb/sfx_usb_host.h>

// Storage (LittleFS flash + SD_MMC SDIO)
#include <storage/flash.h>
#include <storage/sd_card.h>
#include <storage/storage_config_bridge.h>
#include <server/storage_server.h>
#include "protocol/hubfx_usb_server.h"

// Slave-board management (SlaveManager / SlaveServer / per-slave clients /
// hub_lightfx_apply) was removed 2026-05-16 — it will be rebuilt against
// the generic expander module (instructions/15-GENERIC-EXPANDER-REFACTOR.md,
// instructions/17-SYSTEM-SERVICES.md).  Until that lands, HubFX has no
// understanding of "which board is on which USB port" — USB host still
// runs (CDC enumeration is needed by the new layer) but nothing on the
// hub binds devices to typed clients.

// Audio mixer and codec (8-channel WAV mixer with I2S output)
#include <audio/audio_log.h>
#include <server/audio_server.h>
#include "hubfx_audio.h"
using AudioServer = AudioServerT<Mixer>;

// Engine FX (sound effects state machine + protocol handler)
#include "effects/engine_fx.h"
#include "protocol/engine_server.h"

// RC input dispatcher (PWM/PPM → engine on/off + light program select)
#include "inputs/input_dispatcher.h"

// ============================================================================
// Pin Definitions (ESP32-S3 DevKitC-1)
// ============================================================================

// Indicator LEDs (directly driven GPIO)
// DevKitC-1 N8R8 has a user-addressable RGB LED on GPIO48 (active-high).
// Use the onboard LED for connection status. Error LED disabled (no external LED).
#define PIN_LED_CONNECTION  48   // Onboard RGB LED (connection status)
#define PIN_LED_ERROR       -1   // Disabled (no external error LED)

// I2S Audio Output — TAS5825P codec.
// Pin assignments verified against the EasyEDA netlist for the 8-channel
// HubFX rev — see PINOUT.md. ESP32-S3 QFN-56 pins 22/23/24 = GPIO16/17/18
// land on the codec's DIN/BCK/FS pins via the TAS_DI/TAS_BCK/TAS_FS nets.
// Earlier revs routed I²S to GPIO1/4/3; on this PCB those GPIOs go to the
// PPM input headers (IN_10, IN_11) and an unconnected pin.
#define PIN_I2S_DOUT    16   // I²S serial data output → codec DIN  (net TAS_DI,  U26.22)
#define PIN_I2S_BCLK    17   // I²S bit clock           → codec BCK  (net TAS_BCK, U26.23)
#define PIN_I2S_LRCLK   18   // I²S word clock          → codec FS   (net TAS_FS,  U26.24)
// Note: codec → ESP32 data feedback (TAS_DO) is unconnected on this PCB.

// SD Card (SD_MMC SDIO — 4-bit bus). On this PCB rev the SD socket connects
// to GPIO40/41 for CMD/CLK and GPIO42–44/47 for D0–D3 (was GPIO38/39 + D2 on
// GPIO42 + D3 on GPIO45 in earlier revs — see PINOUT.md "Deltas vs
// production firmware"). The SD_MMC slot 1 on ESP32-S3 routes through the
// GPIO matrix so any GPIO is legal.
#define PIN_SD_MMC_CMD  40   // SD_MMC command (net SD_DCMD,  U26.43)
#define PIN_SD_MMC_CLK  41   // SD_MMC clock   (net SD_CLK,   U26.44)
#define PIN_SD_MMC_D0   42   // SD_MMC data 0  (net SD_DATA0, U26.45)
#define PIN_SD_MMC_D1   43   // SD_MMC data 1  (net SD_DATA1, U26.47)
#define PIN_SD_MMC_D2   44   // SD_MMC data 2  (net SD_DATA2, U26.48)
#define PIN_SD_MMC_D3   47   // SD_MMC data 3  (net SD_DATA3, U26.51)

// I2C (TAS5825P codec, INA226 rail monitors, PCA9685 LED PWM)
#define PIN_I2C_SDA     8    // I²C data  (net SDA, U26.13)
#define PIN_I2C_SCL     9    // I²C clock (net SCL, U26.14)

// PCA9685 LED-channel map (HubFX 8-channel rev). The chip exposes 16
// PWM channels at I²C 0x70; the board's MOSFET gates connect to LED0
// through LED7. See PINOUT.md "LED channel signal chain" for the full
// chain: PCA9685 → MOSFET gate → CH+ → LED → CH- → INA226 shunt.
//
// TAS5825P PDN/MUTE are NOT routed through the expander on this rev —
// both are statically pulled HIGH via 10 kΩ resistors. Codec health is
// observable only through the TAS5825P's own I²C fault registers.
#define EXP_LED_CH1     0    // PCA9685 LED0
#define EXP_LED_CH2     1    // PCA9685 LED1
#define EXP_LED_CH3     2    // PCA9685 LED2
#define EXP_LED_CH4     3    // PCA9685 LED3
#define EXP_LED_CH5     4    // PCA9685 LED4
#define EXP_LED_CH6     5    // PCA9685 LED5
#define EXP_LED_CH7     6    // PCA9685 LED6
#define EXP_LED_CH8     7    // PCA9685 LED7
#define LOCAL_LED_COUNT 8

// ============================================================================
// Core 1 Task — Audio Consumer (highest priority on Core 1)
// ============================================================================


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

// LED PWM driver (PCA9685 @ 0x70) — declared early so audio diagnostics
// and initI2CDevices() can both reference it. Satisfies the PwmOutput
// concept (pwm/pwm_output.h) so LedManager<LOCAL_LED_COUNT, PCA9685>
// can drive it.
static PCA9685 ledPwm;

/**
 * @brief Phase 1: Probe I2C, reset codec, enter Deep Sleep.
 *
 * Safe to call before I2S clocks are running.  The codec's PLL is off in
 * Deep Sleep so it won't fault from missing clocks.
 *
 * @return true if codec I2C probe and reset succeeded.
 */
static bool tryInitCodec(sfx_audio::tas5825::Supply voltage) {
    auto& codec = TAS5825MCodec::instance();
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
    auto& codec = TAS5825MCodec::instance();
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
 * TAS5825M PDN/MUTE/nFAULT are wired entirely in hardware on the current PCB
 * — no GPIO or expander pin drives them — so this routine only reads the
 * codec's I²C fault/state registers. The PCA9685 (U54 @ 0x70) is
 * dedicated to driving the 8 local LED MOSFET gates (see initI2CDevices).
 */
static void diagnoseAudioHardware() {
    SFX_LOG_INFO("=== Audio Hardware Diagnostics ===");

    // ---- TAS5825M I2C registers ----
    auto& codec = TAS5825MCodec::instance();
    if (codec.isInitialized()) {
        uint8_t devCtrl     = codec.getDeviceControlRegister();
        uint8_t faultReg    = codec.getFaultRegister();

        // Read additional diagnostic registers directly via I2C
        // (using the codec's test method to verify bus)
        uint8_t powerState = 0, automuteState = 0;
        uint8_t chanFault = 0, globalFault1 = 0, globalFault2 = 0, otWarning = 0;
        uint8_t fsMon = 0;

        // These reads go through Wire directly (codec doesn't expose all regs)
        Wire.beginTransmission(sfx_audio::tas5825::I2C_ADDR);
        Wire.write(0x00); Wire.write(0x00);  // Select Book 0, Page 0
        Wire.endTransmission();
        Wire.beginTransmission(sfx_audio::tas5825::I2C_ADDR);
        Wire.write(0x7F); Wire.write(0x00);
        Wire.endTransmission();

        auto readReg = [](uint8_t reg, uint8_t& val) -> bool {
            Wire.beginTransmission(sfx_audio::tas5825::I2C_ADDR);
            Wire.write(reg);
            if (Wire.endTransmission(false) != 0) return false;
            if (Wire.requestFrom(sfx_audio::tas5825::I2C_ADDR, (uint8_t)1) != 1) return false;
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

    UsbHost& usb = UsbHost::instance();
    SFX_LOG_DEBUG("usb=%s cdc=%d",
                  usb.isInitialized() ? "ready" : "off",
                  usb.cdcDeviceCount());
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
// ledPwm declared earlier (alongside the audio diagnostics)

// Battery monitoring — hardcoded to INA226 channel 0 (0x40), the rail input
// on the HubFX v1 board. Sensor wiring uses the generic BatteryServerT<TBattery>
// handler that claims core packet 0xEE BATTERY_CONFIG. Chemistry / cell count
// default to LiPo / auto; override at runtime via the BATTERY_CONFIG packet.
static constexpr uint8_t BATTERY_INA226_CHANNEL = 0;
static Ina226Battery batteryMonitor;
static BatteryServerT<Ina226Battery> batteryServer(batteryMonitor);

// Module protocol handlers
StorageServer storageServer;
HubFxUsbServer usbServer;
AudioServer audioServer;
EngineServer engineServer;

// ---- Per-domain config stores + multi-store wire dispatch ----
//
// Each YAML file on hub flash gets its own ConfigStore. MultiConfigServer
// routes CONFIG_RELOAD/SAVE/STATUS by path, so the host (Studio / CLI) can
// push or pull one file at a time without round-tripping the others. Each
// store registers its own typed onLoaded callback in initConfig().
//
// Path layout (Rule 26):
//   hubConfig      → /hubfx.yaml      (hub-only: codec supply voltage, input mappings, …)
//   engineConfig   → /enginefx.yaml   (engine FX state machine)
//   gunConfig      → /gunfx.yaml      (hub-side gun audio routing)
//   lightConfig    → /lightfx.yaml    (master copy fanned to LightFX slaves)
static HubFxSettingsStore hubConfig;
static EngineConfigStore  engineConfig;
static GunFxConfigStore   gunConfig;
static LightFxConfigStore lightConfig;

static ConfigStoreFacadeT<HubFxSettingsStore> hubFacade    (hubConfig);
static ConfigStoreFacadeT<EngineConfigStore>  engineFacade (engineConfig);
static ConfigStoreFacadeT<GunFxConfigStore>   gunFacade    (gunConfig);
static ConfigStoreFacadeT<LightFxConfigStore> lightFacade  (lightConfig);

MultiConfigServer configServer;

// RC input router — engine on/off + light program selector. Picks reader
// (PWM/PPM/SBUS/Jeti) at boot from /hubfx.yaml inputs.mode. Re-init runs on
// config reload via the hubConfig.onLoaded callback below.
static InputDispatcher inputDispatcher;

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
        server.core().addCapability(CoreCapability::FLASH);
    } else {
        SFX_LOG_ERROR("Flash init failed");
    }

    // SD card (SD_MMC 4-bit SDIO) — capability advertises *slot present*,
    // not "card mounted right now": clients should still poll SD_STATUS_REQ
    // to learn whether a card is inserted and its remaining free space.
    server.core().addCapability(CoreCapability::SD);
    SdCardModule& sd = SdCardModule::instance();
    SdCardModule::Config sdCfg {
        .clk = PIN_SD_MMC_CLK,
        .cmd = PIN_SD_MMC_CMD,
        .d0  = PIN_SD_MMC_D0,
        .d1  = PIN_SD_MMC_D1,
        .d2  = PIN_SD_MMC_D2,
        .d3  = PIN_SD_MMC_D3,
    };
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

/** @brief Wire flash I/O + per-domain onLoaded handlers, then load every YAML. */
static void initConfig() {
    // ---- Wire every store to LittleFS ----
    wireConfigStore<FlashModule>(hubConfig);
    wireConfigStore<FlashModule>(engineConfig);
    wireConfigStore<FlashModule>(gunConfig);
    wireConfigStore<FlashModule>(lightConfig);

    // ---- Per-store onLoaded — typed Data&, no composite branching ----
    //
    // Each store fires its own callback after a successful load/reload, so a
    // `config.reload /lightfx.yaml` from the host re-applies just the light
    // bindings — engine/audio/gun stay untouched.

    hubConfig.onLoaded([](const HubFxSettings& cfg) {
        sfx_audio::tas5825::Supply voltage;
        if (sfx_audio::tas5825::parseSupply(cfg.codecSupplyVoltage, voltage)) {
            auto& codec = TAS5825MCodec::instance();
            if (codec.isInitialized() && codec.getSupplyVoltage() != voltage) {
                if (codec.setSupplyVoltage(voltage)) {
                    SFX_LOG_INFO("[Config] Codec supply voltage \u2192 %s",
                                 sfx_audio::tas5825::supplyStr(voltage));
                } else {
                    SFX_LOG_ERROR("[Config] Failed to set codec supply voltage");
                }
            }
        } else {
            SFX_LOG_WARN("[Config] Unknown codec_supply_voltage: '%s' (use 12v/15v/20v/24v)",
                         cfg.codecSupplyVoltage);
        }

        // Re-apply RC input dispatcher (mode, channel mappings, ppm pin).
        // Feature callbacks were wired once at boot in initInputDispatcher().
        inputDispatcher.begin(cfg.inputs);
    });

    engineConfig.onLoaded([](const EngineConfig& cfg) {
        SFX_LOG_INFO("[Config] Applied engine_fx %s (%s), output=%s",
                     cfg.enabled ? "enabled" : "disabled",
                     cfg.type,
                     outputChannelsString(cfg.outputChannels));
        EngineFX::instance().applyConfig(cfg);
    });

    gunConfig.onLoaded([](const GunFxHubConfig& cfg) {
        // Audio routing wires up when GunFX-on-hub lands; for now log only.
        SFX_LOG_INFO("[Config] Applied gun_fx output=%s",
                     outputChannelsString(cfg.outputChannels));
    });

    lightConfig.onLoaded([](const LightProgramConfig& cfg) {
        // Slave-side push was removed with the slave-management deletion
        // (2026-05-16). When the generic-expander layer lands, the new
        // role-keyed push (per [17-SYSTEM-SERVICES.md]) replaces this hook.
        (void)cfg;
        SFX_LOG_INFO("[Config] /lightfx.yaml loaded (no expander layer yet — config parked)");
    });

    // ---- Register stores with the wire dispatcher and do the first load ----
    configServer.addStore(hubFacade);
    configServer.addStore(engineFacade);
    configServer.addStore(gunFacade);
    configServer.addStore(lightFacade);
    configServer.loadAll();

    // Engine + Config commands are always advertised by HubFX (engine FX
    // falls back to built-in defaults when /enginefx.yaml is missing).
    // Storage caps are set in initStorage() because they depend on
    // flash.begin() succeeding.
    server.core().addCapability(CoreCapability::ENGINE | CoreCapability::CONFIG);
}

/** @brief Register module protocol handlers and build the handler chain. */
static void initProtocolHandlers() {
    storageServer.begin(&Serial);
    usbServer.begin(&Serial);
    audioServer.begin(&Serial);
    configServer.begin(&Serial);
    engineServer.begin(&Serial);

    // Handler chain: CoreCommandServer (0xF0-0xFF)
    //              → ConfigServer         (0x90-0x92, 0xAC config)
    //              → HubFxUsbServer       (0xA7-0xA8 USB diag)
    //              → AudioServer          (0x84-0x8B audio)
    //              → EngineServer         (0x8C-0x8F engine FX)
    //              → StorageServer        (0x93-0xA6 SD/flash + files)
    //
    // SlaveServer (0x80-0x83 mgmt + 0x01-0x7F routing) was removed
    // 2026-05-16 and will be replaced by the generic-expander wire
    // layer per instructions/15-GENERIC-EXPANDER-REFACTOR.md.
    server.addModuleHandler(&configServer);
    server.addModuleHandler(&usbServer);
    server.addModuleHandler(&audioServer);
    server.addModuleHandler(&engineServer);
    // Generic battery handler (core BATTERY_CONFIG 0xEE) — sensor is bound to
    // INA226[BATTERY_INA226_CHANNEL] in setup() after initI2CDevices().
    batteryServer.begin(&Serial);
    server.addModuleHandler(&batteryServer);

    server.addModuleHandler(&storageServer);

    // Upload is exclusive: stop any active playback, then suspend the
    // audio pipeline for the duration of the upload (sync or batch). The
    // main loop also short-circuits all non-storage work while
    // isUploadActive() is true.
    storageServer.onUploadStart([]() {
        Mixer& mixer = Mixer::instance();
        if (mixer.isAnyPlaying()) {
            mixer.stopAsync(-1, AudioStopMode::Immediate);
            MIXER_LOG("Audio stop queued — SD upload starting");
        }
        mixer.suspendAudio();
    });

    storageServer.onUploadEnd([]() {
        Mixer::instance().resumeAudio();
    });

    // Suppress STATUS_UPDATE during any file transfer (list, tree,
    // download, upload) to keep the serial channel exclusive.
    storageServer.onTransferStart([]() {
        server.core().setTransferActive(true);
    });

    storageServer.onTransferEnd([]() {
        server.core().setTransferActive(false);
    });
}

/**
 * @brief Bring up the USB host stack (no device classification).
 *
 * The old SlaveManager attached typed BusClients to each detected USB
 * CDC device, ran an IDENTIFY/INIT handshake, and routed slave-range
 * packets to the matching board. All of that was removed 2026-05-16 —
 * the new generic-expander layer (instructions/15-GENERIC-EXPANDER-REFACTOR.md
 * + 17-SYSTEM-SERVICES.md) will rebuild it on top of the same USB host
 * stack. Until that lands, HubFX just stands up USB host so the OTG
 * port can enumerate CDC devices; nothing on the hub uses them yet.
 */
static void initUsbHost() {
    UsbHost& usb = UsbHost::instance();
    if (usb.begin()) {
        SFX_LOG_INFO("USB host: %s backend up", usb.backendName());
        server.core().addCapability(CoreCapability::USB_HOST);
    } else {
        SFX_LOG_WARN("USB host: begin() failed — OTG port unavailable");
    }
}

// NOTE: Engine FX is initialized by the config onLoaded callback
// (EngineFX::applyConfig) — no separate initEngineFx() needed.

/**
 * @brief Wire input dispatcher feature callbacks. Reader is started later by
 *        hubConfig.onLoaded() once /hubfx.yaml is parsed.
 *
 * Call BEFORE initConfig() so the very first onLoaded() sees a dispatcher
 * with feature callbacks already in place.
 */
static void initInputDispatcher() {
    inputDispatcher.onEngineToggle([](uint16_t value_us, bool engaged) {
        // Pass the raw pulse so EngineFX can apply its own threshold +
        // hysteresis (engineConfig is the single source of truth for that).
        // The dispatcher's threshold only drives the edge logging here.
        EngineFX::instance().setToggleValue(value_us);
        SFX_LOG_INFO("[Inputs] engine_on_off %s (%u us)",
                     engaged ? "ENGAGED" : "DISENGAGED", value_us);
    });

    inputDispatcher.onLightProgram([](uint8_t programIndex) {
        // Slave-side push was removed with slave management (2026-05-16).
        // The generic-expander layer will rebind this callback to whichever
        // ExpanderType::LightFX role is registered, keyed by board GUID.
        // The hub-local PCA9685 LED runtime will hook into the same
        // callback once it lands.
        SFX_LOG_INFO("[Inputs] light_program_select → %u (no consumer wired yet)", programIndex);
    });
}

/** @brief Initialize I2C peripherals: INA226 rail monitors + PCA9685 LED PWM driver. */
static void initI2CDevices() {
    // INA226 power monitors at 0x40-0x45.
    // Default calibration: 100 mΩ shunt, 3.2 A max (adjust per board design).
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

    // PCA9685 LED PWM driver at 0x70 — drives the 8 local LED-rail MOSFET
    // gates (LED0..LED7) at 1526 Hz, 12-bit duty. The PCA9685 begin() does
    // a general-call SWRST first (recovers a chip that's silent at its
    // address), then configures PRESCALE + push-pull OUTDRV, then wakes the
    // chip with AI enabled. See tests/hw/hubfx_pca9685_hwtest/README.md for
    // the full init flow + recovery procedure.
    if (ledPwm.begin(Wire, PCA9685Address::HUBFX_ADDR)) {
        SFX_LOG_INFO("PCA9685 @ 0x%02X: OK (16-ch 12-bit PWM @ %u Hz)",
                     ledPwm.address(), ledPwm.frequency());

        // Park all 8 channels at brightness 0 so nothing flashes at boot.
        for (uint8_t ch = 0; ch < LOCAL_LED_COUNT; ch++) {
            ledPwm.setLedBrightness(ch, 0);
        }
        SFX_LOG_INFO("  Local LEDs ch1..ch%u → brightness 0",
                     LOCAL_LED_COUNT);
    } else {
        SFX_LOG_WARN("PCA9685 @ 0x%02X: not found",
                     PCA9685Address::HUBFX_ADDR);
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

    // ---- Phase 1: Codec I2C probe + reset → Deep Sleep ----
    // The codec goes into Deep Sleep (PLL off) — safe before I2S clocks.
    sfx_audio::tas5825::Supply initVoltage = sfx_audio::tas5825::Supply::V12;
    if (hubConfig.isLoaded()) {
        sfx_audio::tas5825::parseSupply(hubConfig.data().codecSupplyVoltage, initVoltage);
    }
    if (tryInitCodec(initVoltage)) {
        SFX_LOG_INFO("TAS5825M codec probed OK, in Deep Sleep (supply=%s)",
                     sfx_audio::tas5825::supplyStr(initVoltage));
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
        server.core().addCapability(CoreCapability::AUDIO);
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
    if (TAS5825MCodec::instance().isInitialized()) {
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
            return;
        }

        // PC sent INIT — new session starting.
        // Full re-initialization of all subsystems to ensure clean state.
        SFX_LOG_INFO("INIT received — re-initializing all subsystems");

        // 1. Cancel any upload left over from a previous session.
        storageServer.cancelActiveUpload();

        // 2. Stop all audio and reset codec to clean power-on state.
        {
            Mixer& mixer = Mixer::instance();
            if (mixer.isInitialized()) {
                mixer.stopAsync(-1, AudioStopMode::Immediate);
                mixer.setMasterVolumeAsync(1.0f);
                mixer.resetUnderruns();
            }

            auto& codec = TAS5825MCodec::instance();
            if (codec.isInitialized()) {
                codec.reset();
                codec.clearFaults();
                SFX_LOG_INFO("Audio codec reset");
            } else if (tryInitCodec(codec.getSupplyVoltage())) {
                activateCodec();
                SFX_LOG_INFO("Audio codec initialized on INIT");
            }
        }

        // 3. Reset engine FX state machine (stops sounds, clears state)
        {
            EngineFX& engine = EngineFX::instance();
            if (engine.isInitialized()) {
                engine.end();
            }
        }

        // 4. Reload every per-domain YAML from flash.
        configServer.loadAll();

        SFX_LOG_INFO("INIT complete — all subsystems re-initialized");
    });

    server.onShutdown([]() {
        storageServer.cancelActiveUpload();

        {
            Mixer& mixer = Mixer::instance();
            if (mixer.isInitialized() && mixer.isAnyPlaying()) {
                mixer.stopAsync(-1, AudioStopMode::Immediate);
            }
        }

        {
            EngineFX& engine = EngineFX::instance();
            if (engine.isActive()) {
                engine.forceStop();
            }
        }

        SFX_LOG_INFO("SHUTDOWN — session ended");
    });

    // ---- STATUS_BROADCAST source ----
    // Tag the periodic broadcast with HUBFX so the Go engine routes it to the
    // hubfx handler's RegisterStatusBroadcastParser (otherwise the default CORE
    // source is dropped by sourceToControllerType in app/go/engine/engine.go).
    server.core().setStatusBroadcastSource(StatusUpdateSource::HUBFX);

    // ---- STATUS callback: module-specific status bytes ----
    // Appended after the 22-byte core header in STATUS responses.
    // Layout: [flags:u8][reserved:u8][loop1Count:u32LE]
    //         [i2cDeviceMask:u8][ina226_mV[0..5]:u16LE x 6] = 6 + 13 = 19 bytes
    //
    // The byte at buf[1] used to carry the slave presence bitmask (one bit
    // per SlaveType). Slave management was removed 2026-05-16 — the byte
    // is reserved as 0 until the generic-expander layer needs it for a
    // GUID-aware presence summary.
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 19) return 0;

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
        if (UsbHost::instance().isInitialized())             flags |= 0x08;
        if (SdCardModule::instance().isInitialized())        flags |= 0x10;
        buf[0] = flags;

        // Reserved — was slave-presence bitmask, retained for wire compat.
        buf[1] = 0;

        // Core 1 loop counter (diagnostic)
        CoreProtocol::putU32LE(&buf[2], loop1Count.load(std::memory_order_relaxed));

        // I2C device presence bitmask (byte 6):
        //   bit 0: PCA9685 LED PWM @ 0x70 (was AW9523B @ 0x58 on prev rev)
        //   bit 1: INA226 @ 0x40
        //   bit 2: INA226 @ 0x41
        //   bit 3: INA226 @ 0x42
        //   bit 4: INA226 @ 0x43
        //   bit 5: INA226 @ 0x44
        //   bit 6: INA226 @ 0x45
        //   bit 7: TAS5825P @ 0x4C (reserved)
        uint8_t i2cMask = 0;
        if (ledPwm.isAvailable()) i2cMask |= 0x01;
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

    // Bind battery monitor to the hardcoded INA226 rail channel.
    batteryMonitor.setSensor(ina226[BATTERY_INA226_CHANNEL]);
    batteryMonitor.begin();
    SFX_LOG_INFO("Battery: INA226[%u] @ 0x%02X (LiPo, auto cells)",
                 BATTERY_INA226_CHANNEL, ina226Addrs[BATTERY_INA226_CHANNEL]);

    initStorage();
    initProtocolHandlers();
    initInputDispatcher();  // Wire input feature callbacks before initConfig() fires onLoaded
    initConfig();       // onLoaded callback initializes EngineFX + starts InputDispatcher reader
    server.markConfigLoaded();  // IDLE → STANDALONE
    initUsbHost();
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
    static uint32_t lastDiag_ms = 0;
    static constexpr uint32_t DIAG_INTERVAL_MS = 30000;  // full diag every 30s

    uint32_t now = millis();

    // If codec not initialized, retry periodically
    auto& codec = TAS5825MCodec::instance();
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

// ============================================================================
// Arduino Main Loop (Core 1 — default ARDUINO_RUNNING_CORE)
// ============================================================================

void loop() {
    // Upload is exclusive: when a file upload is in progress, service only
    // the storage server and upload-timeout check. Audio/engine/USB-host/
    // diagnostics/battery are skipped so the COBS reader, fill buffer, and
    // SD writer get the full Core 1 budget. Audio playback + task were
    // already stopped via onUploadStart.
    if (storageServer.isUploadActive()) {
        if (storageServer.isStreamActive()) {
            storageServer.processStream();
        } else {
            server.loop();
        }
        storageServer.checkUploadTimeout();
        return;
    }

    server.loop();
    storageServer.checkUploadTimeout();

    logDiagnostics();

    checkCodecHealth();

    // Pump RC input reader → fires engine_on_off / light_program_select on edge
    inputDispatcher.process();

    EngineFX::instance().process();

    // Sample the battery rail (cached INA226 read; main loop owns ina226[]).
    batteryMonitor.update();

    vTaskDelay(pdMS_TO_TICKS(1));
}
