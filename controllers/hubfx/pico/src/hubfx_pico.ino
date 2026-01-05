/*
 * HubFX Pico - Main Application
 * 
 * Scale model special effects controller for Raspberry Pi Pico
 * 
 * DUAL-CORE ARCHITECTURE:
 *   Core 0: Main loop, serial commands, config, engine FX logic
 *   Core 1: Audio processing (mixing, I2S output)
 * 
 * Features:
 *   - 8-channel audio mixer with I2S output
 *   - WAV file playback from SD card
 *   - Flash-based YAML configuration
 *   - Engine FX (RPM-based sound)
 */

#define FIRMWARE_VERSION "1.1.0"
#define BUILD_NUMBER 127  // Increment this with each build

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

#include "storage/sd_card.h"
#include "audio/audio_codec.h"
#include "audio/wm8960_codec.h"
#include "audio/tas5825_codec.h"
// #include "audio/simple_i2s_codec.h"  // Alternative: simple I2S DACs
#include "audio/audio_mixer.h"
#include "audio/audio_channels.h"
#include "audio/system_sounds.h"
#include "storage/config_reader.h"
#include "effects/engine_fx.h"
#include "effects/gun_fx.h"
#include "debug_config.h"

// USB Host (from serial_common library)
#include <serial_common.h>
#include "tusb_config.h"

// CLI System
#include "cli/command_router.h"
#include "cli/audio_cli.h"
#include "cli/storage_cli.h"
#include "cli/config_cli.h"
#include "cli/engine_cli.h"
#include "cli/gun_cli.h"
#include "cli/system_cli.h"
#include "cli/usb_cli.h"
#if AUDIO_TEST_MODE
#include "cli/test_cli.h"
#endif

// ============================================================================
// Codec Selection
// ============================================================================

// Choose your audio codec by uncommenting ONE of the following:
#define USE_WM8960_CODEC      // Waveshare WM8960 Audio HAT (I2S + I2C)
// #define USE_TAS5825_CODEC     // TI TAS5825M Digital Amp (I2S + I2C, high power)
// #define USE_SIMPLE_I2S_CODEC  // Simple I2S DAC (PCM5102, PT8211, etc.)

// ============================================================================
// Pin Definitions (defaults, can be overridden by config)
// ============================================================================

// I2S Audio Output (to WM8960)
#define DEFAULT_PIN_I2S_DATA    6   // GP6 - I2S DIN (to WM8960 DAC/GPIO21)
#define DEFAULT_PIN_I2S_BCLK    7   // GP7 - I2S BCLK (to WM8960 CLK/GPIO18)
#define DEFAULT_PIN_I2S_LRCLK   8   // GP8 - I2S LRCLK/WS (to WM8960 LRCLK/GPIO19)

// I2C for WM8960 Control
#define DEFAULT_PIN_I2C_SDA     4   // GP4 - I2C SDA (to WM8960 SDA/GPIO2)
#define DEFAULT_PIN_I2C_SCL     5   // GP5 - I2C SCL (to WM8960 SCL/GPIO3)

// SD Card (SPI0)
#define DEFAULT_PIN_SD_CS       17  // GP17 - SD CS
#define DEFAULT_PIN_SD_SCK      18  // GP18 - SD SCK (SPI0)
#define DEFAULT_PIN_SD_MOSI     19  // GP19 - SD MOSI (SPI0)
#define DEFAULT_PIN_SD_MISO     16  // GP16 - SD MISO (SPI0)

// Status LED
#define PIN_STATUS_LED          25  // Onboard LED

// ============================================================================
// Global Objects
// ============================================================================

// SD card
SdCardModule sdCard;

// Audio codec (polymorphic - can be any codec type)
#ifdef USE_WM8960_CODEC
WM8960Codec audioCodec;
AudioCodec* codec = &audioCodec;
#elif defined(USE_TAS5825_CODEC)
TAS5825Codec audioCodec;
AudioCodec* codec = &audioCodec;
#elif defined(USE_SIMPLE_I2S_CODEC)
SimpleI2SCodec audioCodec("PCM5102");
AudioCodec* codec = &audioCodec;
#else
AudioCodec* codec = nullptr;  // No codec (I2S only)
#endif

// Audio mixer
AudioMixer mixer;

// Engine FX
EngineFX engineFx;

// Gun FX (slave controller)
GunFX gunFx;

// USB Host (from serial_common library, runs on Core 1)
UsbHost usbHost;

// Configuration
ConfigReader configReader;

// CLI System
CommandRouter cmdRouter;
AudioCli audioCli(&mixer);
StorageCli storageCli(&sdCard);
ConfigCli configCli(&configReader, &sdCard);
EngineCli engineCli(&engineFx);
GunCli gunCli(&gunFx);
SystemCli systemCli;
UsbCli usbCli(&usbHost);

// State
bool config_loaded = false;
bool audio_initialized = false;
bool engineFxInitialized = false;

// ============================================================================
// Audio Processing (Dual-Core Mode - Core 1)
// ============================================================================

// Audio runs on Core 1 for optimal performance
// Core 0 sends commands via queue, Core 1 does mixing and I2S output
// Uses mutex-protected command queue for thread-safe communication

volatile bool core1_running = false;

// Core 1 setup - called once when Core 1 starts
void setup1() {
    // Wait for Core 0 to complete initialization
    while (!audio_initialized) {
        delay(10);
    }
    CORE1_LOG("Audio processing started");
    
    // Initialize USB Host on Core 1
    // USB PIO timing is critical and runs better on dedicated core
    if (usbHost.init()) {
        CORE1_LOG("USB Host initialized (D+:GP%d, D-:GP%d)", 
                  PIO_USB_DP_PIN_DEFAULT, PIO_USB_DP_PIN_DEFAULT + 1);
    } else {
        CORE1_LOG("WARNING: USB Host init failed");
    }
    
    core1_running = true;
}

// Core 1 loop - audio processing + USB host
void loop1() {
    if (audio_initialized) {
        mixer.process();
    }
    
    // Process USB host tasks (enumeration, hot-plug detection)
    usbHost.process();
}

// ============================================================================
// Status LED
// ============================================================================

static unsigned long last_blink = 0;
static bool led_state = false;

void status_led_update() {
    unsigned long now = millis();
    unsigned long interval;
    
    // Blink pattern based on state
    if (!sdCard.isInitialized()) {
        interval = 100;  // Fast blink - SD error
    } else if (!config_loaded) {
        interval = 250;  // Medium blink - config error
    } else if (!audio_initialized) {
        interval = 500;  // Slow blink - audio error
    } else {
        interval = 1000; // Heartbeat - all OK
    }
    
    if (now - last_blink >= interval) {
        last_blink = now;
        led_state = !led_state;
        digitalWrite(PIN_STATUS_LED, led_state);
    }
}

// ============================================================================
// Configuration Loading
// ============================================================================

bool load_configuration() {
    MAIN_LOG("Loading configuration from SD card...");
    
    // Initialize config reader with SD card
    if (!configReader.begin(&sdCard.getSd())) {
        MAIN_LOG("SD card config init failed");
        configReader.loadDefaults();
        return false;
    }
    
    MAIN_LOG("SD card config initialized successfully");
    
    // Try to load from SD card root
    if (configReader.load("/config.yaml")) {
        MAIN_LOG("Configuration loaded from /config.yaml");
#if MAIN_DEBUG
        configReader.print();
#endif
        return true;
    }
    
    // Load defaults
    configReader.loadDefaults();
    MAIN_LOG("No config file found on SD card, using defaults");
#if MAIN_DEBUG
    configReader.print();
#endif
    return true;  // Still OK, just using defaults
}

// ============================================================================
// Audio Initialization (Dual-Core)
// ============================================================================

bool init_audio() {
    MAIN_LOG("Initializing audio system...");
    
    // Initialize audio codec if configured
    if (codec) {
        MAIN_LOG("Initializing %s codec...", codec->getModelName());
        
        #ifdef USE_WM8960_CODEC
        // WM8960 requires I2C initialization
        if (!audioCodec.begin(Wire, DEFAULT_PIN_I2C_SDA, DEFAULT_PIN_I2C_SCL, AUDIO_SAMPLE_RATE)) {
            MAIN_LOG("Codec initialization failed!");
            MAIN_LOG("Check I2C wiring (GP4=SDA, GP5=SCL)");
            return false;
        }
        
        // Enable outputs and set volume
        audioCodec.enableSpeakers(true);
        audioCodec.enableHeadphones(true);
        audioCodec.setVolume(0.7f);  // 70% default volume
        
        #elif defined(USE_TAS5825_CODEC)
        // TAS5825M requires I2C initialization with supply voltage
        if (!audioCodec.begin(Wire, DEFAULT_PIN_I2C_SDA, DEFAULT_PIN_I2C_SCL, 
                              AUDIO_SAMPLE_RATE, TAS5825M_20V)) {  // 20V supply default
            MAIN_LOG("Codec initialization failed!");
            MAIN_LOG("Check I2C wiring (GP4=SDA, GP5=SCL)");
            return false;
        }
        
        // Set initial volume
        audioCodec.setVolumeDB(0.0f);  // 0dB default
        
        #elif defined(USE_SIMPLE_I2S_CODEC)
        // Simple I2S codec (auto-configure from I2S signals)
        if (!audioCodec.begin(AUDIO_SAMPLE_RATE)) {
            MAIN_LOG("Codec initialization failed!");
            return false;
        }
        #endif
        
        MAIN_LOG("%s codec initialized", codec->getModelName());
    } else {
        MAIN_LOG("No codec configured (I2S only mode)");
    }
    
    // Initialize I2S output and mixer
    MAIN_LOG("Initializing audio mixer...");
    if (!mixer.begin(&sdCard.getSd(), DEFAULT_PIN_I2S_DATA, DEFAULT_PIN_I2S_BCLK, DEFAULT_PIN_I2S_LRCLK, codec)) {
        MAIN_LOG("Audio mixer initialization failed!");
        return false;
    }
    
    // Set mixer volume
    mixer.setVolume(-1, 0.8f);
    
    MAIN_LOG("Audio system initialized");
    return true;
}

// ============================================================================
// Engine FX Initialization
// ============================================================================

bool init_engine_fx() {
    MAIN_LOG("Initializing Engine FX...");
    
    const EngineFXSettings& settings = configReader.engineSettings();
    
    // Check if engine FX is enabled and sounds are configured
    if (!settings.enabled) {
        MAIN_LOG("Engine FX disabled in config");
        return false;
    }
    
    if (!settings.startupSound.filename && !settings.runningSound.filename) {
        MAIN_LOG("No engine sounds configured");
        return false;
    }
    
    // Create settings with GPIO pin mapping
    // Input channels are abstract (1-10), map to GPIO pins: channel 1 = GP10, etc.
    EngineFXSettings efxSettings = settings;
    if (settings.togglePin > 0) {
        efxSettings.togglePin = 10 + (settings.togglePin - 1);  // Map channel to GPIO
    }
    
    // Initialize EngineFX with the OOP API
    if (!engineFx.begin(efxSettings, &mixer)) {
        MAIN_LOG("Engine FX initialization failed!");
        return false;
    }
    
    MAIN_LOG("Engine FX initialized");
    return true;
}

// ============================================================================
// Serial Command Handler
// ============================================================================

void handle_serial_command() {
    if (!Serial.available()) return;
    
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();  // Make case-insensitive
    
    if (cmd.length() == 0) return;
    
    Serial.printf("> %s\n", cmd.c_str());
    
    // Route command to appropriate handler
    if (!cmdRouter.routeCommand(cmd)) {
        Serial.println("Unknown command. Type 'help' for available commands.");
    }
}

// ============================================================================
// Arduino Setup
// ============================================================================

void setup() {
    // Initialize status LED
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, HIGH);
    
    // Initialize serial
    Serial.begin(115200);
    
    // Wait for serial (with timeout)
    unsigned long start = millis();
    while (!Serial && (millis() - start < 3000)) {
        delay(10);
    }
    
    Serial.println();
    Serial.println("========================================");
    Serial.println("  HubFX Pico - Scale Model FX Controller");
    Serial.printf("  Version %s (Build %d)\n", FIRMWARE_VERSION, BUILD_NUMBER);
    Serial.println("  Dual-Core + USB Host");
    Serial.printf("  Core 0: %d MHz\n", rp2040.f_cpu() / 1000000);
    Serial.println("========================================");
    Serial.println();
    
    // Initialize SD card first - try multiple speeds on failure
    const uint8_t sd_speeds[] = {20, 15, 10, 5};  // MHz - try fastest first
    bool sd_success = false;
    for (uint8_t i = 0; i < sizeof(sd_speeds); i++) {
        MAIN_LOG("Attempting SD init at %d MHz...", sd_speeds[i]);
        
        sdCard.begin(DEFAULT_PIN_SD_CS, DEFAULT_PIN_SD_SCK, DEFAULT_PIN_SD_MOSI, DEFAULT_PIN_SD_MISO, sd_speeds[i]);
        if (sdCard.isInitialized()) {
            MAIN_LOG("✓ SD card initialized at %d MHz", sd_speeds[i]);
            sd_success = true;
            break;
        }
        delay(100);  // Brief delay before retry
    }
    
    if (!sd_success) {
        MAIN_LOG("WARNING: Running without SD card (audio playback disabled)");
    }
    
    // Load configuration from SD card (after SD initialization)
    if (sd_success) {
        config_loaded = load_configuration();
    } else {
        MAIN_LOG("Skipping config load - SD card not available");
        configReader.loadDefaults();
        config_loaded = false;
    }
    
    // Initialize audio codec (codec doesn't need SD card - only playback does)
    MAIN_LOG("Initializing audio codec...");
    audio_initialized = init_audio();
    
    // Register codec with CLI after initialization
    if (codec) {
        audioCli.setCodec(codec);
        MAIN_LOG("✓ Codec registered with CLI");
    } else {
        MAIN_LOG("WARNING: No codec available");
    }
    
    // Core 1 will automatically start running setup1() and loop1()
    // Audio processing happens there via mixer.process()
    MAIN_LOG("Audio processing delegated to Core 1");
    
    // Initialize USB Host (Core 0 setup, Core 1 will call init())
    if (usbHost.begin()) {
        MAIN_LOG("USB Host configured (init on Core 1)");
    } else {
        MAIN_LOG("WARNING: USB Host config failed");
    }
    
#if AUDIO_TEST_MODE
    if (audio_initialized) {
        MAIN_LOG("Use 'test' commands to generate mock audio");
        AudioTestCLI::setMixer(&mixer);
    }
#endif
    
    // Initialize Engine FX
    if (audio_initialized) {
        engineFxInitialized = init_engine_fx();
    }
    
    // Initialize CLI command router
    MAIN_LOG("Initializing CLI system...");
    
    // Configure system CLI with version and slave references
    systemCli.setVersion(FIRMWARE_VERSION, BUILD_NUMBER);
    systemCli.setSdCard(&sdCard);
    systemCli.setGunFX(&gunFx);
    
    // Register all command handlers (order matters - first match wins)
    cmdRouter.addHandler(&systemCli);   // System commands (help, version, etc.)
    cmdRouter.addHandler(&audioCli);    // Audio commands (play, stop, etc.)
    cmdRouter.addHandler(&storageCli);  // Storage commands (ls, cat, etc.)
    cmdRouter.addHandler(&configCli);   // Config commands
    cmdRouter.addHandler(&engineCli);   // Engine commands
    cmdRouter.addHandler(&gunCli);      // Gun commands (slave control)
    cmdRouter.addHandler(&usbCli);      // USB host commands
    
    // Register handlers with system CLI for help display
    std::vector<CommandHandler*> allHandlers = {
        &systemCli, &audioCli, &storageCli, &configCli, &engineCli, &gunCli, &usbCli
    };
    systemCli.registerHandlers(allHandlers);
    
    MAIN_LOG("CLI system ready");
    
    Serial.println();
    MAIN_LOG("Initialization complete");
    Serial.println("Type 'help' for commands");
    Serial.println();
    
    // Play HubFX initialization sound on system channel
    if (audio_initialized && sdCard.isInitialized()) {
        AudioPlaybackOptions opts;
        opts.volume = 0.8f;
        opts.loop = false;
        mixer.playAsync(SystemSounds::CHANNEL, SystemSounds::HUBFX_INITIALIZED, opts);
        MAIN_LOG("Queued initialization sound");
    }
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

void loop() {
    // Audio processing moved to Core 1 (loop1)
    // Core 0 handles commands and state machines
    
    // Update status LED
    status_led_update();
    
    // Process Engine FX state machine
    if (engineFxInitialized) {
        engineFx.process();
    }
    
    // Handle serial commands
    handle_serial_command();
}
