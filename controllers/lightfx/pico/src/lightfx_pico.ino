/**
 * LightFX Pico Controller v0.9.0
 * 
 * Server controller for lighting effects - receives commands from HubFX over USB serial.
 * Controls: 8-channel LED outputs with sequences, 3 servos.
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 * 
 * Architecture (Chain of Responsibility):
 *   - CoreCommandServer: Handles INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE
 *   - LightFxServer: Handles LED, SERVO, POWER commands
 *   - CommandRouter: Routes packets to handlers in priority order
 * 
 * Pin Assignments:
 *   LED Channels: GPIO0-7 (8 channels, PWM capable)
 *   Indicator LEDs: GPIO24 (connection), GPIO25 (error)
 *   Battery ADC: GPIO29 (÷5.1 divider)
 *   Servos: GPIO8-10
 */

#include <Arduino.h>
#include <Servo.h>
#include <serial/serial.h>
#include <lightfx/server/lightfx_server.h>
#include <led/led_manager.h>
#include <servo/srv_control.h>
#include <server/sfx_server.h>
#include <power/battery_monitor.h>
#include <power/battery_server.h>
#include <storage/flash.h>
#include <storage/storage_config_bridge.h>
#include <config/config_store.h>
#include <server/config_server.h>
#include <server/storage_server.h>
#include "config/lightfx_config.h"
#include "landing_light.h"

// ============================================================================
//  FIRMWARE INFO
// ============================================================================

#define FIRMWARE_VERSION "0.11.0"
#define BUILD_NUMBER 34

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

// LED Output Channels (active high, PWM capable)
const uint8_t PIN_LED_CH1 = 0;
const uint8_t PIN_LED_CH2 = 1;
const uint8_t PIN_LED_CH3 = 2;
const uint8_t PIN_LED_CH4 = 3;
const uint8_t PIN_LED_CH5 = 4;
const uint8_t PIN_LED_CH6 = 5;
const uint8_t PIN_LED_CH7 = 6;
const uint8_t PIN_LED_CH8 = 7;

// Servos
const uint8_t PIN_SERVO_1 = 8;
const uint8_t PIN_SERVO_2 = 9;
const uint8_t PIN_SERVO_3 = 10;

// Indicator LEDs
const uint8_t PIN_LED_CONN = 24;
const uint8_t PIN_LED_ERR  = 25;

// Battery voltage ADC
const uint8_t PIN_VSENSE = 29;

// Array of LED channel pins
const uint8_t LED_CHANNEL_PINS[8] = {
    PIN_LED_CH1, PIN_LED_CH2, PIN_LED_CH3, PIN_LED_CH4,
    PIN_LED_CH5, PIN_LED_CH6, PIN_LED_CH7, PIN_LED_CH8
};

// ============================================================================
//  CONSTANTS
// ============================================================================

const uint8_t LED_CHANNEL_COUNT = 8;
const uint8_t LANDING_LIGHT_COUNT = 3;

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;
const int SERVO_DEFAULT_MAX_SPEED  = 4000;
const int SERVO_DEFAULT_ACCEL      = 8000;
const int SERVO_DEFAULT_DECEL      = 8000;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Server (serial, core protocol, indicators, connection management)
SfxServer server;
LightFxServer lightfxServer;
// Battery voltage monitor (ADC with ÷5.1 divider, e.g. 41k/10k)
// Plus generic core BATTERY_CONFIG (0xEE) handler.
using BatteryT = AdcDividerBatteryT<5100>;
BatteryT batteryMonitor;
BatteryServerT<BatteryT> batteryServer(batteryMonitor);

// Config server (handles CONFIG_RELOAD/STATUS/SAVE protocol + owns ConfigStore)
using LightFxConfigStore = ConfigStore<LightFxConfigSchema>;
ConfigServerT<LightFxConfigStore> configServer;

// Storage server (handles FILE_LIST/DOWNLOAD/UPLOAD for LittleFS flash — lets
// Studio read/write /lightfx.yaml directly over the wire). Only TARGET_FLASH
// is meaningful here — no SD card on LightFX Pico.
StorageServer storageServer;

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Connection state managed by IndicatorLedManager (indicators)

// LED channel manager (8 GPIO channels, PWM-capable)
LedManager<8> ledManager;

// Servos with motion profiling
ServoControl servos[3];

// Landing light sequencers (bind servo + LED channel)
LandingLight landingLights[LANDING_LIGHT_COUNT];


// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void performSafeShutdown();
void performSafeInit();
void setServoPulse(uint8_t servo_id, int pulse_us);
void setupLightFxCallbacks();
void updateLandingLights();

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void performSafeShutdown() {
    SFX_LOG_INFO("Shutdown — LEDs off, servos center, landing lights off");

    // Shutdown landing lights first (turns off LEDs)
    for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
        landingLights[i].shutdown();
    }

    // Stop all LED sequences, turn off LEDs, reset master brightness
    ledManager.shutdown();

    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }
}

void performSafeInit() {
    SFX_LOG_INFO("Init — safe reset");

    // Init resets everything to a known safe state
    performSafeShutdown();
    
    // Re-enable all LED channels
    ledManager.init();
}

// ============================================================================
//  SERVO CONTROL
// ============================================================================

void setServoPulse(uint8_t servo_id, int pulse_us) {
    if (servo_id < 1 || servo_id > 3) return;
    servos[servo_id - 1].setTarget(pulse_us);
}

void updateServos() {
    for (int i = 0; i < 3; i++) {
        servos[i].update();
    }
}

void updateLandingLights() {
    for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
        landingLights[i].update();
    }
}

// ============================================================================
//  LIGHTFX CALLBACKS SETUP
// ============================================================================

void setupLightFxCallbacks() {
    // SERVO_SET callback (servo ID validated by LightFxSpec before dispatch)
    lightfxServer.onServoSet([](uint8_t id, int pulseUs) -> uint8_t {
        servos[id - 1].setTarget(pulseUs);
        return LightFxError::OK;
    });
    
    // SERVO_SETTINGS callback (servo ID validated by LightFxSpec before dispatch)
    lightfxServer.onServoSettings([](const LightFxServoConfig& cfg) -> uint8_t {

        ServoControl& servo = servos[cfg.servoId - 1];

        if (cfg.minUs >= 0 && cfg.maxUs >= 0) {
            servo.setLimits(cfg.minUs, cfg.maxUs);
        }
        if (cfg.maxSpeedUsPerSec >= 0) {
            servo.setMaxSpeed(cfg.maxSpeedUsPerSec);
        }
        if (cfg.maxAccelUsPerSec2 >= 0) {
            servo.setAcceleration(cfg.maxAccelUsPerSec2);
        }
        if (cfg.maxDecelUsPerSec2 >= 0) {
            servo.setDeceleration(cfg.maxDecelUsPerSec2);
        }
        servo.setReversed(cfg.reversed);
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_BIND callback
    // Wire format: [slot:u8][servoId:u8][channelMask:u8][brightness:u8]
    //   servoId=0: no servo (LED-only group)
    //   channelMask: bitmask, bit0=ch1 .. bit7=ch8
    lightfxServer.onLandingLightBind([](uint8_t slot, uint8_t servoId, uint8_t channelMask,
                                        uint8_t brightness) -> uint8_t {
        // Decode channelMask to LedControl* array
        LedControl* leds[LandingLight::MAX_LEDS];
        uint8_t ledCount = 0;
        for (uint8_t bit = 0; bit < LED_CHANNEL_COUNT; bit++) {
            if (channelMask & (1 << bit)) {
                leds[ledCount++] = &ledManager.channel(bit);
            }
        }
        if (ledCount == 0) return LightFxError::INVALID_CHANNEL;

        // Servo pointer (nullptr if servoId == 0)
        ServoControl* servo = (servoId > 0) ? &servos[servoId - 1] : nullptr;

        landingLights[slot - 1].setSlot(slot);
        landingLights[slot - 1].configure(servo, leds, ledCount, brightness);

        // Register progress callback (emits LANDING_LIGHT_STATUS packets)
        landingLights[slot - 1].onProgress([](const LightFxLandingLightStatus& status) {
            lightfxServer.sendLandingLightStatus(status);
        });
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_UNBIND callback
    lightfxServer.onLandingLightUnbind([](uint8_t slot) -> uint8_t {
        if (slot == 0) {
            for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
                landingLights[i].unconfigure();
            }
        } else {
            landingLights[slot - 1].unconfigure();
        }
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_DEPLOY callback
    lightfxServer.onLandingLightDeploy([](uint8_t slot) -> uint8_t {
        if (slot == 0) {
            for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
                if (landingLights[i].isConfigured()) landingLights[i].deploy();
            }
        } else {
            if (!landingLights[slot - 1].isConfigured()) return LightFxError::INVALID_SLOT;
            landingLights[slot - 1].deploy();
        }
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_RETRACT callback
    lightfxServer.onLandingLightRetract([](uint8_t slot) -> uint8_t {
        if (slot == 0) {
            for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
                if (landingLights[i].isConfigured()) landingLights[i].retract();
            }
        } else {
            if (!landingLights[slot - 1].isConfigured()) return LightFxError::INVALID_SLOT;
            landingLights[slot - 1].retract();
        }
        return LightFxError::OK;
    });
    
}

/** @brief Initialize flash and load config (if present). */
static void initFlashAndConfig() {
    FlashModule& flash = FlashModule::instance();
    if (flash.begin()) {
        FlashStorageInfo info;
        flash.getStorageInfo(info);
        SFX_LOG_INFO("Flash ready: %lu/%lu bytes used",
                     (unsigned long)info.usedBytes, (unsigned long)info.totalBytes);

        // Wire config store to flash I/O
        wireConfigStore<FlashModule>(configServer.store());

        // Callback fires after protocol-triggered config reload
        configServer.onReloaded([](const LightFxConfig&) {
            // TODO: Apply config fields to hardware when schema has real fields
            SFX_LOG_INFO("Config reloaded via protocol");
        });

        // Try loading config (silent if file doesn't exist)
        if (configServer.loadConfig()) {
            server.markConfigLoaded();  // IDLE → STANDALONE
        }
        // If file doesn't exist, defaults are used — that's fine
    } else {
        SFX_LOG_WARN("Flash init failed — running with defaults");
    }
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    // Battery ADC
    analogReadResolution(12);
    batteryMonitor.begin(PIN_VSENSE);

    server.begin("LightFX", FIRMWARE_VERSION, BUILD_NUMBER, PIN_LED_CONN, PIN_LED_ERR);
    server.onInit([](uint8_t mode, uint8_t flags) {
        (void)flags;  // LightFX accepts both SLAVE and DIRECT
        SFX_LOG_INFO("INIT mode=%s", InitMode::getName(mode));
        performSafeInit();
    });
    server.onShutdown([]() { performSafeShutdown(); });

    // Initialize flash storage and load config (standalone mode)
    initFlashAndConfig();
    
    // Initialize LED channels via LedManager
    ledManager.begin(LED_CHANNEL_PINS, false, true);
    
    // Initialize servos
    const uint8_t servo_pins[3] = {PIN_SERVO_1, PIN_SERVO_2, PIN_SERVO_3};
    for (int i = 0; i < 3; i++) {
        servos[i].begin(servo_pins[i], SERVO_DEFAULT_US);
        servos[i].setMotionProfile(SERVO_DEFAULT_MAX_SPEED, SERVO_DEFAULT_ACCEL, SERVO_DEFAULT_DECEL);
    }
    
    // Initialize LightFxServer
    lightfxServer.begin(&Serial);
    lightfxServer.setLedManager(&ledManager);
    setupLightFxCallbacks();
    
    // STATUS: Append LightFX module data to core STATUS response
    // Wire format (20 bytes):
    //   [ledBrightness:u8×8][ledSeqFlags:u8]
    //   [servo0:u16][servo1:u16][servo2:u16]
    //   [landingLightStates:u8×3]
    //   [masterBrightness_pct:u8]
    //   [ledEnabledFlags:u8]  (bit per channel, 1=enabled)
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 20) return 0;
        
        // LED channel brightness (8 bytes)
        for (uint8_t i = 0; i < 8; i++) {
            buf[i] = ledManager.channel(i).brightness();
        }
        
        // LED sequence playing flags (1 byte, bit per channel)
        buf[8] = ledManager.seqPlayingFlags();
        
        // Servo positions (6 bytes)
        CoreProtocol::putU16LE(&buf[9], (uint16_t)servos[0].position());
        CoreProtocol::putU16LE(&buf[11], (uint16_t)servos[1].position());
        CoreProtocol::putU16LE(&buf[13], (uint16_t)servos[2].position());
        
        // Landing light states (3 bytes)
        for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
            buf[15 + i] = (uint8_t)landingLights[i].state();
        }
        
        // Master brightness percentage (1 byte)
        buf[18] = ledManager.masterBrightness();
        
        // LED enabled flags (1 byte, bit per channel, 1=enabled)
        buf[19] = ledManager.enabledFlags();
        
        // Battery (4 bytes)
        CoreProtocol::putU16LE(&buf[20], (uint16_t)batteryMonitor.voltage_mV());
        buf[22] = batteryMonitor.cellCount();
        buf[23] = batteryMonitor.percentage();
        
        return 24;
    });
    
    // Register config server (handles CONFIG_RELOAD/STATUS/SAVE)
    configServer.begin(&Serial);
    server.addModuleHandler(&configServer);

    // Generic battery handler (core BATTERY_CONFIG 0xEE) — register before
    // the module handler so battery commands resolve quickly.
    batteryServer.begin(&Serial);
    server.addModuleHandler(&batteryServer);

    // Finalize router (core handler + LightFX handler)
    server.addModuleHandler(&lightfxServer);

    // Register StorageServer (FILE_* protocol for /lightfx.yaml I/O).
    // FlashModule is already mounted by initFlashAndConfig().
    storageServer.begin(&Serial);
    storageServer.onTransferStart([]() { server.core().setTransferActive(true); });
    storageServer.onTransferEnd  ([]() { server.core().setTransferActive(false); });
    server.addModuleHandler(&storageServer);

    // Advertise interfaces — Pico LightFX has flash + config but no SD slot,
    // no audio/USB host/engine. Used by clients (CLI, Studio file manager)
    // to gate UI without speculative status probes.
    server.core().addCapability(CoreCapability::FLASH | CoreCapability::CONFIG);
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    // Update battery monitor
    batteryMonitor.update();

    // Process protocol, connection timeout, indicators
    server.loop();

    // Clean up stuck file uploads (aborted transfer → delete partial, free buffer)
    storageServer.checkUploadTimeout();
    
    // Update LED sequences
    ledManager.update();
    
    // Update servos
    updateServos();
    
    // Update landing light sequencers
    updateLandingLights();
    
    busy_wait_ms(1);
}
