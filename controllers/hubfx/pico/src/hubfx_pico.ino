/*
 * HubFX Pico - Main Application
 * 
 * Scale model special effects controller for Raspberry Pi Pico
 * 
 * DUAL-CORE ARCHITECTURE:
 *   Core 0: Main loop, serial commands, config, engine/gun FX logic
 *   Core 1: Dedicated audio processing AND USB host task
 * 
 * Features:
 *   - 8-channel audio mixer with I2S output
 *   - WAV file playback from SD card
 *   - USB HOST for connecting to GunFX boards (via PIO-USB)
 *   - YAML configuration
 *   - Engine FX (RPM-based sound)
 *   - Gun FX (with optional slave controller)
 * 
 * NOTE: CPU must run at 120MHz or 240MHz for USB host timing.
 */

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

#include "audio_mixer.h"
#include "config_reader.h"
#include <serial_common.h>
#include "engine_fx.h"
#include "gun_fx.h"

// ============================================================================
// Pin Definitions (defaults, can be overridden by config)
// ============================================================================

// I2S Audio Output (must NOT conflict with USB D+ pins)
// USB uses GP2/GP3, so use GP6+ for I2S
#define DEFAULT_PIN_I2S_DATA    6   // GP6 - I2S DIN
#define DEFAULT_PIN_I2S_BCLK    7   // GP7 - I2S BCLK
#define DEFAULT_PIN_I2S_LRCLK   8   // GP8 - I2S LRCLK/WS

// USB Host (PIO-USB)
#define DEFAULT_PIN_USB_DP      2   // GP2 - USB D+ (D- is GP3)

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
SdFat sd;

// Audio mixer
AudioMixer mixer;

// USB host
UsbHost usb_host;

// Engine FX
EngineFX engineFx;

// Gun FX
GunFX gunFx;

// Configuration
ConfigReader configReader;

// State
bool sd_initialized = false;
bool audio_initialized = false;
bool config_loaded = false;
bool usb_host_initialized = false;
bool engineFxInitialized = false;
bool gunFxInitialized = false;

// ============================================================================
// Core 1 Combined Task (Audio + USB Host)
// ============================================================================

#include <pico/multicore.h>

static volatile bool g_core1Running = false;

/**
 * Combined Core 1 task that handles both audio processing and USB host.
 * 
 * TIMING NOTES:
 *   - Audio: 44.1kHz stereo, 512-sample buffer = 11.6ms per refill
 *   - USB: SOF packets handled by 1ms hardware timer (NOT tuh_task)
 *   - tuh_task() handles enumeration/CDC data - safe at 2-10ms intervals
 * 
 * The PIO-USB library uses an alarm_pool repeating timer for SOF (Start of
 * Frame) packets, which runs every 1ms via interrupt. This is independent
 * of how often tuh_task() is called.
 * 
 * Loop timing: ~3-6ms per iteration is acceptable.
 */
static void core1CombinedTask() {
    Serial.println("[Core1] Combined task starting (Audio + USB)");
    
    // Initialize USB host (PIO-USB requires init on Core 1)
    if (!usb_host.init()) {
        Serial.println("[Core1] USB host init failed!");
        g_core1Running = false;
        return;
    }
    
    g_core1Running = true;
    Serial.println("[Core1] Running audio + USB processing");
    
    while (g_core1Running) {
        // Process audio - mixes channels, outputs to I2S DMA
        if (audio_initialized) {
            mixer.process();
        }
        
        // Process USB host - enumeration, CDC data transfer, callbacks
        usb_host.process();
        
        // Small yield when idle to prevent tight spinning
        if (!audio_initialized) {
            delayMicroseconds(1000);  // 1ms polling when no audio
        }
    }
    
    Serial.println("[Core1] Task stopped");
}

/**
 * Start the combined Core 1 task
 */
bool startCore1Task() {
    if (g_core1Running) return true;
    
    Serial.println("[Core1] Launching combined task...");
    multicore_launch_core1(core1CombinedTask);
    
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
    if (!sd_initialized) {
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
// SD Card Initialization
// ============================================================================

bool init_sd_card() {
    Serial.println("[MAIN] Initializing SD card...");
    
    // Configure SPI pins
    SPI.setRX(DEFAULT_PIN_SD_MISO);
    SPI.setTX(DEFAULT_PIN_SD_MOSI);
    SPI.setSCK(DEFAULT_PIN_SD_SCK);
    
    // Initialize SD card at 25MHz
    if (!sd.begin(DEFAULT_PIN_SD_CS, SD_SCK_MHZ(25))) {
        Serial.println("[MAIN] SD card initialization failed!");
        Serial.println("[MAIN] Check wiring and card format (FAT32)");
        return false;
    }
    
    // Print card info
    uint32_t size = sd.card()->sectorCount();
    if (size > 0) {
        Serial.printf("[MAIN] SD card: %lu MB\n", (unsigned long)(size / 2048));
    }
    
    Serial.println("[MAIN] SD card initialized");
    return true;
}

// ============================================================================
// Configuration Loading
// ============================================================================

bool load_configuration() {
    Serial.println("[MAIN] Loading configuration...");
    
    // Initialize config reader with SD card
    if (!configReader.begin(&sd)) {
        Serial.println("[MAIN] Config reader init failed");
        return false;
    }
    
    // Try to load from SD card
    if (configReader.load("/config.yaml")) {
        Serial.println("[MAIN] Configuration loaded from /config.yaml");
        configReader.print();
        return true;
    }
    
    // Try alternative path
    if (configReader.load("/hubfx.yaml")) {
        Serial.println("[MAIN] Configuration loaded from /hubfx.yaml");
        configReader.print();
        return true;
    }
    
    // Load defaults
    configReader.loadDefaults();
    Serial.println("[MAIN] No config file found, using defaults");
    configReader.print();
    return true;  // Still OK, just using defaults
}

// ============================================================================
// Audio Initialization (Dual-Core)
// ============================================================================

bool init_audio() {
    Serial.println("[MAIN] Initializing audio mixer (dual-core mode)...");
    
    // Initialize for dual-core operation (prepares mutex, command queue)
    // Note: Core 1 is started later by startCore1Task() after USB is also ready
    if (!mixer.beginDualCore(&sd)) {
        Serial.println("[MAIN] Audio mixer initialization failed!");
        return false;
    }
    
    // Set master volume (default)
    mixer.setVolume(-1, 0.8f);
    
    Serial.println("[MAIN] Audio mixer initialized (awaiting Core 1 start)");
    return true;
}

// ============================================================================
// USB Host Initialization
// ============================================================================

// Callback when USB device is mounted
void on_usb_device_mount(uint8_t dev_addr, uint16_t vid, uint16_t pid) {
    Serial.printf("[USB] Device connected: addr=%d, VID=%04X, PID=%04X\n", 
                  dev_addr, vid, pid);
}

// Callback when USB device is unmounted
void on_usb_device_unmount(uint8_t dev_addr) {
    Serial.printf("[USB] Device disconnected: addr=%d\n", dev_addr);
}

// Callback when CDC data is received
void on_usb_cdc_receive(uint8_t dev_addr, const uint8_t* data, size_t len) {
    Serial.printf("[USB] CDC RX from addr=%d (%zu bytes): ", dev_addr, len);
    for (size_t i = 0; i < len && i < 64; i++) {
        if (data[i] >= 32 && data[i] < 127) {
            Serial.print((char)data[i]);
        } else {
            Serial.printf("[%02X]", data[i]);
        }
    }
    if (len > 64) Serial.print("...");
    Serial.println();
}

bool init_usb_host() {
    Serial.println("[MAIN] Initializing USB host...");
    Serial.println("[MAIN] Note: Use USB hub to connect multiple GunFX boards");
    
    // Configure single USB port (hardcoded - USB host is required for GunFX)
    UsbPortConfig port_configs[USB_HOST_MAX_PORTS];
    
    // Single USB host port (PIO0 used by I2S, PIO1 for USB)
    port_configs[0].enabled = true;
    port_configs[0].dp_pin = DEFAULT_PIN_USB_DP;
    strncpy(port_configs[0].name, "GunFX-Hub", sizeof(port_configs[0].name));
    
    // Initialize USB host
    if (!usb_host.begin(port_configs, USB_HOST_MAX_PORTS)) {
        Serial.println("[MAIN] USB host initialization failed!");
        return false;
    }
    
    // Set callbacks
    usb_host.onMount(on_usb_device_mount);
    usb_host.onUnmount(on_usb_device_unmount);
    usb_host.onCdcReceive(on_usb_cdc_receive);
    
    // Note: USB task will be started on Core 1 by startCore1Task()
    // along with audio processing
    
    Serial.println("[MAIN] USB host initialized (awaiting Core 1 start)");
    usb_host.printStatus();
    
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
// Gun FX Initialization
// ============================================================================

bool init_gun_fx() {
    Serial.println("[MAIN] Initializing Gun FX...");
    
    const GunFXSettings& settings = configReader.gunSettings();
    
    // Check if gun FX is enabled
    if (!settings.enabled) {
        Serial.println("[MAIN] Gun FX disabled in config");
        return false;
    }
    
    // Check if any rates of fire are configured
    if (settings.rateCount == 0) {
        Serial.println("[MAIN] No rates of fire configured");
        return false;
    }
    
    // Check if USB host has a connected device
    if (!usb_host_initialized || usb_host.cdcDeviceCount() == 0) {
        Serial.println("[MAIN] No USB CDC devices connected");
        Serial.println("[MAIN] Gun FX will initialize when GunFX board connects");
        return false;
    }
    
    // Find the first CDC device (GunFX board)
    int deviceIndex = 0;
    
    // Create settings with GPIO pin mapping
    // Input channels are abstract (1-10), map to GPIO pins: channel 1 = GP10, etc.
    GunFXSettings gfxSettings = settings;
    if (settings.triggerChannel > 0) {
        gfxSettings.triggerChannel = 10 + (settings.triggerChannel - 1);
    }
    if (settings.smoke.heaterToggleChannel > 0) {
        gfxSettings.smoke.heaterToggleChannel = 10 + (settings.smoke.heaterToggleChannel - 1);
    }
    if (settings.pitch.inputChannel > 0) {
        gfxSettings.pitch.inputChannel = 10 + (settings.pitch.inputChannel - 1);
    }
    if (settings.yaw.inputChannel > 0) {
        gfxSettings.yaw.inputChannel = 10 + (settings.yaw.inputChannel - 1);
    }
    
    // Initialize Gun FX with OOP API
    if (!gunFx.begin(&usb_host, deviceIndex, 
                     audio_initialized ? &mixer : nullptr, gfxSettings)) {
        Serial.println("[MAIN] Gun FX initialization failed!");
        return false;
    }
    
    // Start the gun FX system (sends INIT to GunFX Pico)
    if (!gunFx.start()) {
        Serial.println("[MAIN] Gun FX start failed!");
        gunFx.end();
        return false;
    }
    
    Serial.println("[MAIN] Gun FX initialized");
    return true;
}

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
    
    if (cmd.length() == 0) return;
    
    Serial.printf("> %s\n", cmd.c_str());
    
    // Parse command
    if (cmd.startsWith("play ")) {
        // play <channel> <filename> [loop] [volume] [output]
        // Example: play 0 /sounds/engine.wav loop 0.5 left
        int channel = cmd.charAt(5) - '0';
        if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) {
            Serial.println("Invalid channel (0-7)");
            return;
        }
        
        int space = cmd.indexOf(' ', 7);
        String filename;
        if (space > 0) {
            filename = cmd.substring(7, space);
        } else {
            filename = cmd.substring(7);
        }
        
        AudioPlaybackOptions opts;
        opts.loop = cmd.indexOf("loop") > 0;
        opts.volume = 1.0f;
        opts.output = AudioOutput::Stereo;
        opts.startOffsetMs = 0;
        
        // Check for output routing
        if (cmd.indexOf("left") > 0) opts.output = AudioOutput::Left;
        else if (cmd.indexOf("right") > 0) opts.output = AudioOutput::Right;
        
        // Check for volume
        int vol_idx = cmd.indexOf("vol ");
        if (vol_idx > 0) {
            opts.volume = cmd.substring(vol_idx + 4).toFloat();
        }
        
        // Use async API for dual-core safety
        if (mixer.playAsync(channel, filename.c_str(), opts)) {
            Serial.printf("Playing on channel %d\n", channel);
        } else {
            Serial.println("Failed to queue play command");
        }
    }
    else if (cmd.startsWith("stop ")) {
        int channel = cmd.charAt(5) - '0';
        if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) {
            mixer.stopAsync(-1, AudioStopMode::Immediate);
            Serial.println("Stopped all channels");
        } else {
            mixer.stopAsync(channel, AudioStopMode::Immediate);
            Serial.printf("Stopped channel %d\n", channel);
        }
    }
    else if (cmd.startsWith("fade ")) {
        int channel = cmd.charAt(5) - '0';
        if (channel >= 0 && channel < AUDIO_MAX_CHANNELS) {
            mixer.stopAsync(channel, AudioStopMode::Fade);
            Serial.printf("Fading channel %d\n", channel);
        }
    }
    else if (cmd.startsWith("volume ")) {
        float vol = cmd.substring(7).toFloat();
        mixer.setVolumeAsync(-1, vol);
        Serial.printf("Master volume: %.0f%%\n", vol * 100);
    }
    else if (cmd == "status") {
        Serial.println("=== Channel Status ===");
        Serial.printf("Core 1: %s\n", g_core1Running ? "running" : "stopped");
        for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
            // Use async-safe status query
            bool playing = mixer.isPlaying(i);
            int remaining = mixer.remainingMs(i);
            if (playing) {
                Serial.printf("Ch%d: playing", i);
                if (remaining >= 0) {
                    Serial.printf(" (%d ms remaining)", remaining);
                } else {
                    Serial.print(" (looping)");
                }
                Serial.println();
            }
        }
        Serial.println("=====================");
    }
    else if (cmd == "ls" || cmd == "dir") {
        File32 root = sd.open("/", O_RDONLY);
        if (!root) {
            Serial.println("Error: Failed to open root directory");
            return;
        }
        Serial.println("=== SD Card Contents ===");
        while (true) {
            File32 entry = root.openNextFile();
            if (!entry) break;
            char name[64];
            entry.getName(name, sizeof(name));
            Serial.printf("  %s%s (%lu bytes)\n", 
                         name,
                         entry.isDirectory() ? "/" : "",
                         (unsigned long)entry.size());
            entry.close();
        }
        root.close();
        Serial.println("========================");
    }
    else if (cmd == "config") {
        configReader.print();
    }
    else if (cmd == "usb") {
        usb_host.printStatus();
    }
    else if (cmd == "usb list") {
        // Quick device listing
        int count = usb_host.cdcDeviceCount();
        Serial.printf("USB CDC devices: %d\n", count);
        for (int i = 0; i < count; i++) {
            const CdcDeviceInfo* dev = usb_host.getCdcDevice(i);
            if (dev) {
                const char* stateStr = "?";
                switch (dev->state) {
                    case UsbDeviceState::Disconnected: stateStr = "disconnected"; break;
                    case UsbDeviceState::Connected: stateStr = "connected"; break;
                    case UsbDeviceState::Mounted: stateStr = "mounted"; break;
                    case UsbDeviceState::Ready: stateStr = "ready"; break;
                }
                Serial.printf("  [%d] VID=%04X PID=%04X addr=%d %s\n",
                              i, dev->vid, dev->pid, dev->dev_addr, stateStr);
            }
        }
    }
    else if (cmd.startsWith("usend ")) {
        // usend <dev> <message>
        // Example: usend 0 Hello GunFX
        int dev_idx = cmd.charAt(6) - '0';
        if (dev_idx < 0 || dev_idx >= usb_host.cdcDeviceCount()) {
            Serial.printf("Invalid device (0-%d)\n", usb_host.cdcDeviceCount() - 1);
            return;
        }
        String message = cmd.substring(8);
        int sent = usb_host.cdcPrintln(dev_idx, message.c_str());
        if (sent > 0) {
            Serial.printf("Sent to USB device %d: %s\n", dev_idx, message.c_str());
        } else {
            Serial.printf("Failed to send to USB device %d\n", dev_idx);
        }
    }
    else if (cmd.startsWith("urecv ")) {
        // urecv <dev> - read available data from USB CDC device
        int dev_idx = cmd.charAt(6) - '0';
        if (dev_idx < 0 || dev_idx >= usb_host.cdcDeviceCount()) {
            Serial.printf("Invalid device (0-%d)\n", usb_host.cdcDeviceCount() - 1);
            return;
        }
        int available = usb_host.cdcAvailable(dev_idx);
        if (available <= 0) {
            Serial.printf("USB device %d: no data available\n", dev_idx);
            return;
        }
        Serial.printf("USB device %d (%d bytes): ", dev_idx, available);
        uint8_t buf[256];
        int read = usb_host.cdcRead(dev_idx, buf, sizeof(buf));
        for (int i = 0; i < read; i++) {
            if (buf[i] >= 32 && buf[i] < 127) {
                Serial.print((char)buf[i]);
            } else {
                Serial.printf("[%02X]", buf[i]);
            }
        }
        Serial.println();
    }
    else if (cmd == "engine") {
        if (engineFxInitialized) {
            Serial.printf("Engine state: %s\n", EngineFX::stateString(engineFx.state()));
        } else {
            Serial.println("Engine FX not initialized");
        }
    }
    else if (cmd == "engine start") {
        if (engineFxInitialized) {
            engineFx.forceStart();
            Serial.println("Engine start commanded");
        } else {
            Serial.println("Engine FX not initialized");
        }
    }
    else if (cmd == "engine stop") {
        if (engineFxInitialized) {
            engineFx.forceStop();
            Serial.println("Engine stop commanded");
        } else {
            Serial.println("Engine FX not initialized");
        }
    }
    else if (cmd == "gun") {
        if (gunFxInitialized) {
            gunFx.printStatus();
        } else {
            Serial.println("Gun FX not initialized");
        }
    }
    else if (cmd.startsWith("gun fire ")) {
        int rpm = cmd.substring(9).toInt();
        if (gunFxInitialized) {
            gunFx.trigger(rpm);
            Serial.printf("Gun fire at %d RPM commanded\n", rpm);
        } else {
            Serial.println("Gun FX not initialized");
        }
    }
    else if (cmd == "gun stop") {
        if (gunFxInitialized) {
            gunFx.ceaseFire();
            Serial.println("Gun cease fire commanded");
        } else {
            Serial.println("Gun FX not initialized");
        }
    }
    else if (cmd == "gun heater on") {
        if (gunFxInitialized) {
            gunFx.setSmokeHeater(true);
            Serial.println("Smoke heater ON");
        } else {
            Serial.println("Gun FX not initialized");
        }
    }
    else if (cmd == "gun heater off") {
        if (gunFxInitialized) {
            gunFx.setSmokeHeater(false);
            Serial.println("Smoke heater OFF");
        } else {
            Serial.println("Gun FX not initialized");
        }
    }
    else if (cmd.startsWith("gun servo ")) {
        // gun servo <id> <pulse_us>
        // Example: gun servo 1 1500
        int space = cmd.indexOf(' ', 10);
        if (space > 0) {
            int servo_id = cmd.substring(10, space).toInt();
            int pulse_us = cmd.substring(space + 1).toInt();
            if (gunFxInitialized) {
                gunFx.setServo(servo_id, pulse_us);
                Serial.printf("Servo %d set to %d us\n", servo_id, pulse_us);
            } else {
                Serial.println("Gun FX not initialized");
            }
        } else {
            Serial.println("Usage: gun servo <id> <pulse_us>");
        }
    }
    else if (cmd == "help") {
        Serial.println("=== HubFX Commands ===");
        Serial.println("Audio:");
        Serial.println("  play <ch> <file> [loop] [vol X.X] [left|right]");
        Serial.println("  stop <ch|all>    - stop playback");
        Serial.println("  fade <ch>        - fade out channel");
        Serial.println("  volume <0.0-1.0> - set master volume");
        Serial.println("  status           - show audio channel status");
        Serial.println("Files:");
        Serial.println("  ls / dir         - list SD card contents");
        Serial.println("  config           - show loaded configuration");
        Serial.println("USB:");
        Serial.println("  usb              - detailed USB host status");
        Serial.println("  usb list         - quick device list");
        Serial.println("  usend <dev> <msg> - send to USB device");
        Serial.println("  urecv <dev>      - read from USB device");
        Serial.println("Engine:");
        Serial.println("  engine           - show engine state");
        Serial.println("  engine start     - force engine start");
        Serial.println("  engine stop      - force engine stop");
        Serial.println("Gun:");
        Serial.println("  gun              - show gun FX status");
        Serial.println("  gun fire <rpm>   - manual fire at RPM");
        Serial.println("  gun stop         - cease fire");
        Serial.println("  gun heater on/off - smoke heater control");
        Serial.println("  gun servo <id> <us> - set servo position");
        Serial.println("======================");
    }
    else {
        Serial.println("Unknown command. Type 'help' for commands.");
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
    
    // Initialize SD card
    sd_initialized = init_sd_card();
    if (!sd_initialized) {
        Serial.println("[MAIN] WARNING: Running without SD card");
    }
    
    // Load configuration
    if (sd_initialized) {
        config_loaded = load_configuration();
    } else {
        configReader.loadDefaults();
        config_loaded = true;
    }
    
    // Initialize audio (prepares for Core 1, doesn't start it yet)
    if (sd_initialized) {
        audio_initialized = init_audio();
    }
    
    // Initialize USB host (prepares for Core 1, doesn't start it yet)
    usb_host_initialized = init_usb_host();
    
    // START CORE 1: Combined audio + USB task
    // This must happen AFTER both audio and USB are initialized but BEFORE
    // we try to play sounds or communicate with GunFX devices
    if (audio_initialized || usb_host_initialized) {
        if (!startCore1Task()) {
            Serial.println("[MAIN] CRITICAL: Failed to start Core 1!");
            Serial.println("[MAIN] Audio and USB will not function properly.");
        } else {
            Serial.println("[MAIN] Core 1 started (Audio + USB processing)");
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
    
    // Initialize Gun FX (requires USB host)
    if (usb_host_initialized) {
        gunFxInitialized = init_gun_fx();
    }
    
    Serial.println();
    Serial.println("[MAIN] Initialization complete");
    Serial.println("[MAIN] Type 'help' for commands");
    Serial.println();
}

// ============================================================================
// Arduino Main Loop (Core 0)
// ============================================================================

// Tracking for late GunFX initialization (device hot-plug)
static unsigned long lastGunFxCheckMs = 0;

void loop() {
    // Update status LED
    status_led_update();
    
    // Process Engine FX state machine
    if (engineFxInitialized) {
        engineFx.process();
    }
    
    // Process Gun FX
    if (gunFxInitialized) {
        gunFx.process();
    } else if (usb_host_initialized) {
        // Check periodically for newly connected GunFX board
        unsigned long now = millis();
        if (now - lastGunFxCheckMs >= 2000) {
            lastGunFxCheckMs = now;
            if (usb_host.cdcDeviceCount() > 0) {
                Serial.println("[MAIN] USB device detected, trying to initialize Gun FX...");
                gunFxInitialized = init_gun_fx();
            }
        }
    }
    
    // Handle serial commands
    handle_serial_command();
}
