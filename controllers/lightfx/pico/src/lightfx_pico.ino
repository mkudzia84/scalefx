/**
 * LightFX Pico Controller v0.7.0
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
#include "landing_light.h"

// ============================================================================
//  FIRMWARE INFO
// ============================================================================

#define FIRMWARE_VERSION "0.7.0"
#define BUILD_NUMBER 19

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
BatteryMonitor batteryMonitor;

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
    lightfxServer.onServoSettings([](uint8_t id, int minUs, int maxUs, 
                                     int speed, int accel, int decel) -> uint8_t {
        
        ServoControl& servo = servos[id - 1];
        
        if (minUs >= 0 && maxUs >= 0) {
            servo.setLimits(minUs, maxUs);
        }
        if (speed >= 0) {
            servo.setMaxSpeed(speed);
        }
        if (accel >= 0) {
            servo.setAcceleration(accel);
        }
        if (decel >= 0) {
            servo.setDeceleration(decel);
        }
        return LightFxError::OK;
    });
    
    // LANDING_LIGHT_BIND callback
    lightfxServer.onLandingLightBind([](uint8_t slot, uint8_t servoId, uint8_t ledChannel,
                                        uint16_t deployUs, uint16_t retractUs,
                                        uint8_t brightness) -> uint8_t {
        landingLights[slot - 1].setSlot(slot);  // Set slot ID for progress reporting
        landingLights[slot - 1].configure(
            &servos[servoId - 1], &ledManager.channel(ledChannel - 1),
            deployUs, retractUs, brightness);
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

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    // Battery ADC
    analogReadResolution(12);
    batteryMonitor.begin(PIN_VSENSE, 5.1f);

    server.begin("LightFX", FIRMWARE_VERSION, BUILD_NUMBER, PIN_LED_CONN, PIN_LED_ERR);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });
    
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
    
    // Finalize router (core handler + LightFX handler)
    server.addModuleHandler(&lightfxServer);
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    // Update battery monitor
    batteryMonitor.update();
    
    // Process protocol, connection timeout, indicators
    server.loop();
    
    // Update LED sequences
    ledManager.update();
    
    // Update servos
    updateServos();
    
    // Update landing light sequencers
    updateLandingLights();
    
    busy_wait_ms(1);
}
