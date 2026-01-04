/*
 * HubFX Pico - Main Application
 * 
 * Scale model special effects controller for Raspberry Pi Pico
 * 
 * DUAL-CORE ARCHITECTURE:
 *   Core 0: Main loop, serial commands, config, engine FX logic
 *   Core 1: Dedicated audio processing
 * 
 * Features:
 *   - 8-channel audio mixer with I2S output
 *   - WAV file playback from SD card
 *   - Flash-based YAML configuration
 *   - Engine FX (RPM-based sound)
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

#include "storage/sd_card.h"
#include "audio/audio_codec.h"
#include "audio/wm8960_codec.h"
#include "audio/tas5825_codec.h"
// #include "audio/simple_i2s_codec.h"  // Alternative: simple I2S DACs
#include "audio/audio_mixer.h"
#include "storage/config_reader.h"
#include "effects/engine_fx.h"
#include "effects/gun_fx.h"

// CLI System
#include "cli/command_router.h"
#include "cli/audio_cli.h"
#include "cli/storage_cli.h"
#include "cli/config_cli.h"
#include "cli/engine_cli.h"
#include "cli/system_cli.h"

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

// Configuration
ConfigReader configReader;

// CLI System
CommandRouter cmdRouter;
AudioCli audioCli(&mixer);
StorageCli storageCli(&sdCard);
ConfigCli configCli(&configReader, &sdCard);
EngineCli engineCli(&engineFx);
SystemCli systemCli;

// State
bool config_loaded = false;
bool audio_initialized = false;
bool engineFxInitialized = false;

// ============================================================================
// Core 1 Audio Task
// ============================================================================

#include <pico/multicore.h>

static volatile bool g_core1Running = false;

/**
 * Core 1 task that handles audio processing.
 * 
 * TIMING:
 *   - Audio: 44.1kHz stereo, 512-sample buffer = 11.6ms per refill
 *   - Double-buffered DMA refills every ~5.8ms
 *   - Loop takes ~3-6ms, primarily gated by I2S DMA blocking
 */
static void core1AudioTask() {
    Serial.println("[Core1] Audio task starting");
    
    g_core1Running = true;
    
    while (g_core1Running) {
        // Process audio - mixes channels, outputs to I2S DMA
        if (audio_initialized) {
            mixer.process();
        } else {
            // Small yield when idle
            delayMicroseconds(1000);  // 1ms polling when no audio
        }
    }
    
    Serial.println("[Core1] Task stopped");
}

/**
 * Start the Core 1 audio task
 */
bool startCore1Task() {
    if (g_core1Running) return true;
    
    Serial.println("[Core1] Launching audio task...");
    multicore_launch_core1(core1AudioTask);
    
    // Wait for startup
    int timeout = 100;
    while (!g_core1Running && timeout > 0) {
        delay(10);
        timeout--;
    }
    
    return g_core1Running;
}

/**
 * Stop the combined Core 1 task
 */
void stopCore1Task() {
    if (!g_core1Running) return;
    
    g_core1Running = false;
    delay(50);
    multicore_reset_core1();
    Serial.println("[Core1] Task stopped");
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
    Serial.println("[MAIN] Loading configuration from SD card...");
    
    // Initialize config reader with SD card
    if (!configReader.begin(&sdCard.getSd())) {
        Serial.println("[MAIN] SD card config init failed");
        configReader.loadDefaults();
        return false;
    }
    
    Serial.println("[MAIN] SD card config initialized successfully");
    
    // Try to load from SD card root
    if (configReader.load("/config.yaml")) {
        Serial.println("[MAIN] Configuration loaded from /config.yaml");
        configReader.print();
        return true;
    }
    
    // Load defaults
    configReader.loadDefaults();
    Serial.println("[MAIN] No config file found on SD card, using defaults");
    configReader.print();
    return true;  // Still OK, just using defaults
}

// ============================================================================
// Audio Initialization (Dual-Core)
// ============================================================================

bool init_audio() {
    Serial.println("[MAIN] Initializing audio system...");
    
    // Initialize audio codec if configured
    if (codec) {
        Serial.printf("[MAIN] Initializing %s codec...\n", codec->getModelName());
        
        #ifdef USE_WM8960_CODEC
        // WM8960 requires I2C initialization
        if (!audioCodec.begin(Wire, DEFAULT_PIN_I2C_SDA, DEFAULT_PIN_I2C_SCL, AUDIO_SAMPLE_RATE)) {
            Serial.println("[MAIN] Codec initialization failed!");
            Serial.println("[MAIN] Check I2C wiring (GP4=SDA, GP5=SCL)");
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
            Serial.println("[MAIN] Codec initialization failed!");
            Serial.println("[MAIN] Check I2C wiring (GP4=SDA, GP5=SCL)");
            return false;
        }
        
        // Set initial volume
        audioCodec.setVolumeDB(0.0f);  // 0dB default
        
        #elif defined(USE_SIMPLE_I2S_CODEC)
        // Simple I2S codec (auto-configure from I2S signals)
        if (!audioCodec.begin(AUDIO_SAMPLE_RATE)) {
            Serial.println("[MAIN] Codec initialization failed!");
            return false;
        }
        #endif
        
        Serial.printf("[MAIN] %s codec initialized\n", codec->getModelName());
    } else {
        Serial.println("[MAIN] No codec configured (I2S only mode)");
    }
    
    // Initialize I2S output and mixer
    Serial.println("[MAIN] Initializing audio mixer...");
    if (!mixer.begin(&sdCard.getSd(), DEFAULT_PIN_I2S_DATA, DEFAULT_PIN_I2S_BCLK, DEFAULT_PIN_I2S_LRCLK, codec)) {
        Serial.println("[MAIN] Audio mixer initialization failed!");
        return false;
    }
    
    // Set mixer volume
    mixer.setVolume(-1, 0.8f);
    
    Serial.println("[MAIN] Audio system initialized (dual-core ready)");
    return true;
}

// ============================================================================
// Engine FX Initialization
// ============================================================================

bool init_engine_fx() {
    Serial.println("[MAIN] Initializing Engine FX...");
    
    const EngineFXSettings& settings = configReader.engineSettings();
    
    // Check if engine FX is enabled and sounds are configured
    if (!settings.enabled) {
        Serial.println("[MAIN] Engine FX disabled in config");
        return false;
    }
    
    if (!settings.startupSound.filename && !settings.runningSound.filename) {
        Serial.println("[MAIN] No engine sounds configured");
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
        Serial.println("[MAIN] Engine FX initialization failed!");
        return false;
    }
    
    Serial.println("[MAIN] Engine FX initialized");
    return true;
}

// ============================================================================
// (Gun FX removed - not supported in simplified version)
// ============================================================================

// ============================================================================
// Test Functions
// ============================================================================

void play_startup_sound() {
    // Try to play a startup sound
    const char* test_files[] = {
        "/sounds/startup.wav",
        "/startup.wav",
        "/test.wav",
        "/sounds/test.wav"
    };
    
    AudioPlaybackOptions opts;
    opts.loop = false;
    opts.volume = 0.8f;
    opts.output = AudioOutput::Stereo;
    opts.startOffsetMs = 0;
    
    for (const char* file : test_files) {
        // Use async version for dual-core safety
        if (mixer.playAsync(0, file, opts)) {
            Serial.printf("[MAIN] Playing startup sound: %s\n", file);
            return;
        }
    }
    
    Serial.println("[MAIN] No startup sound found");
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
    Serial.println("  Version 1.1.0 (Dual-Core + USB Host)");
    Serial.printf("  Core 0: %d MHz\n", rp2040.f_cpu() / 1000000);
    Serial.println("========================================");
    Serial.println();
    
    // Load configuration from flash (always available)
    config_loaded = load_configuration();
    
    // Initialize SD card (for audio files only) - try multiple speeds on failure
    const uint8_t sd_speeds[] = {20, 15, 10, 5};  // MHz - try fastest first
    bool sd_success = false;
    for (uint8_t i = 0; i < sizeof(sd_speeds); i++) {
        Serial.print("[MAIN] Attempting SD init at ");
        Serial.print(sd_speeds[i]);
        Serial.println(" MHz...");
        
        sdCard.begin(DEFAULT_PIN_SD_CS, DEFAULT_PIN_SD_SCK, DEFAULT_PIN_SD_MOSI, DEFAULT_PIN_SD_MISO, sd_speeds[i]);
        if (sdCard.isInitialized()) {
            Serial.print("[MAIN] ✓ SD card initialized at ");
            Serial.print(sd_speeds[i]);
            Serial.println(" MHz");
            sd_success = true;
            break;
        }
        delay(100);  // Brief delay before retry
    }
    
    if (!sd_success) {
        Serial.println("[MAIN] WARNING: Running without SD card (audio disabled)");
    }
    
    // Initialize audio (prepares for Core 1, doesn't start it yet)
    if (sdCard.isInitialized()) {
        audio_initialized = init_audio();
    }
    
    // START CORE 1: Audio task only
    // Must happen AFTER audio init but BEFORE we try to play sounds
    if (audio_initialized) {
        if (!startCore1Task()) {
            Serial.println("[MAIN] CRITICAL: Failed to start Core 1!");
            Serial.println("[MAIN] Audio will not function properly.");
        } else {
            Serial.println("[MAIN] Core 1 started (Audio processing)");
        }
    }
    
    // Play startup sound (now that Core 1 is running)
    if (audio_initialized) {
        play_startup_sound();
    }
    
    // Initialize Engine FX
    if (audio_initialized) {
        engineFxInitialized = init_engine_fx();
    }
    
    // Initialize CLI command router
    Serial.println("[MAIN] Initializing CLI system...");
    
    // Register all command handlers (order matters - first match wins)
    cmdRouter.addHandler(&systemCli);   // System commands (help, version, etc.)
    cmdRouter.addHandler(&audioCli);    // Audio commands (play, stop, etc.)
    cmdRouter.addHandler(&storageCli);  // Storage commands (ls, cat, etc.)
    cmdRouter.addHandler(&configCli);   // Config commands
    cmdRouter.addHandler(&engineCli);   // Engine commands
    
    // Register handlers with system CLI for help display
    std::vector<CommandHandler*> allHandlers = {
        &systemCli, &audioCli, &storageCli, &configCli, &engineCli
    };
    systemCli.registerHandlers(allHandlers);
    
    Serial.println("[MAIN] CLI system ready");
    
    Serial.println();
    Serial.println("[MAIN] Initialization complete");
    Serial.println("[MAIN] Type 'help' for commands");
    Serial.println();
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

void loop() {
    // Update status LED
    status_led_update();
    
    // Process Engine FX state machine
    if (engineFxInitialized) {
        engineFx.process();
    }
    
    // Handle serial commands
    handle_serial_command();
}
