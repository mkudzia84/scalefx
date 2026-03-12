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
 *   LED Channels: GPIO21-28 (8 channels, PWM capable)
 *   Indicator LEDs: GPIO13 (connection), GPIO14 (error)
 *   Servos: GPIO1-3
 */

#include <Arduino.h>
#include <Servo.h>
#include <serial/serial.h>
#include <led/led_control.h>
#include <led/led_event_seq.h>
#include <led/led_events.h>
#include <servo/srv_control.h>
#include <server/sfx_server.h>
#include "landing_light.h"

// ============================================================================
//  FIRMWARE INFO
// ============================================================================

#define FIRMWARE_VERSION "0.7.0"
#define BUILD_NUMBER 13

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

// LED Output Channels (active high, PWM capable)
const uint8_t PIN_LED_CH1 = 28;
const uint8_t PIN_LED_CH2 = 27;
const uint8_t PIN_LED_CH3 = 26;
const uint8_t PIN_LED_CH4 = 25;
const uint8_t PIN_LED_CH5 = 24;
const uint8_t PIN_LED_CH6 = 23;
const uint8_t PIN_LED_CH7 = 22;
const uint8_t PIN_LED_CH8 = 21;

// Servos
const uint8_t PIN_SERVO_1 = 1;
const uint8_t PIN_SERVO_2 = 2;
const uint8_t PIN_SERVO_3 = 3;

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

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Connection state managed by IndicatorLedManager (indicators)

// LED channels with PWM control
LedControl ledChannels[LED_CHANNEL_COUNT];
LedEventSeq ledSequences[LED_CHANNEL_COUNT];

// Servos with motion profiling
ServoControl servos[3];

// Landing light sequencers (bind servo + LED channel)
LandingLight landingLights[LANDING_LIGHT_COUNT];

// LED channel enable/disable state (default: all enabled)
bool ledEnabled[LED_CHANNEL_COUNT];


// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void performSafeShutdown();
void performSafeInit();
void stopAllLedSequences();
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
    
    stopAllLedSequences();
    
    // Reset master brightness to 100%
    for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
        ledChannels[i].setMasterBrightness_pct(100);
    }
    
    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }
}

void performSafeInit() {
    SFX_LOG_INFO("Init — safe reset");

    // Init resets everything to a known safe state — same as shutdown
    performSafeShutdown();
    
    // Re-enable all LED channels (enable/disable is not persisted)
    for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
        ledEnabled[i] = true;
    }
}

// ============================================================================
//  LED CHANNEL CONTROL
// ============================================================================

void stopAllLedSequences() {
    for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
        ledSequences[i].stop();
        ledChannels[i].off();
    }
}

void updateLedSequences() {
    for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
        if (ledSequences[i].isPlaying()) {
            ledSequences[i].update();
        }
    }
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
//  EVENT TYPE HELPER
// ============================================================================

/**
 * @brief Convert LED event class name to protocol event type constant
 */
uint8_t eventNameToType(const char* name) {
    if (strcmp(name, "LedOn") == 0) return LightFxEventType::ON;
    if (strcmp(name, "LedOff") == 0) return LightFxEventType::OFF;
    if (strcmp(name, "LedFlashing") == 0) return LightFxEventType::FLASH;
    if (strcmp(name, "LedFadeIn") == 0) return LightFxEventType::FADE_IN;
    if (strcmp(name, "LedFadeOut") == 0) return LightFxEventType::FADE_OUT;
    if (strcmp(name, "LedFading") == 0) return LightFxEventType::FADING;
    return 0xFF;  // Unknown
}

// ============================================================================
//  LIGHTFX CALLBACKS SETUP
// ============================================================================

void setupLightFxCallbacks() {
    // LED_SET callback (channel validated by LightFxSpec before dispatch)
    lightfxServer.onLedSet([](uint8_t channel, uint8_t brightness) -> uint8_t {
        if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
        ledSequences[channel - 1].stop();
        ledChannels[channel - 1].setBrightness(brightness);
        return LightFxError::OK;
    });
    
    // LED_OFF callback
    lightfxServer.onLedOff([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            // All channels — skip disabled silently
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (!ledEnabled[i]) continue;
                ledSequences[i].stop();
                ledChannels[i].off();
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
            ledSequences[channel - 1].stop();
            ledChannels[channel - 1].off();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_CLEAR callback
    lightfxServer.onLedSeqClear([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (!ledEnabled[i]) continue;
                ledSequences[i].stop();
                ledSequences[i].clear();
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
            ledSequences[channel - 1].stop();
            ledSequences[channel - 1].clear();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_ADD callback (binary format)
    lightfxServer.onLedSeqAdd([](uint8_t channel, uint8_t eventType,
                                 uint16_t param1, uint16_t param2,
                                 uint8_t param3, uint8_t param4) -> uint8_t {
        if (channel < 1 || channel > LED_CHANNEL_COUNT) {
            return LightFxError::INVALID_CHANNEL;
        }
        if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
        
        LedEventSeq& seq = ledSequences[channel - 1];
        if (seq.isFull()) return LightFxError::SEQ_FULL;
        
        ILedEvent* event = nullptr;
        
        switch (eventType) {
            case LightFxEventType::ON:
                // param1=duration, param3=brightness
                event = new LedOn(param1, param3 > 0 ? param3 : 100);
                break;
            case LightFxEventType::OFF:
                // param1=duration
                event = new LedOff(param1);
                break;
            case LightFxEventType::FLASH:
                // param1=interval, param2=duration, param3=brightness, param4=duty
                event = new LedFlashing(param1, param2, param3 > 0 ? param3 : 100, param4 > 0 ? param4 : 50);
                break;
            case LightFxEventType::FADE_IN:
                // param1=duration, param3=brightness
                event = new LedFadeIn(param1, param3 > 0 ? param3 : 100);
                break;
            case LightFxEventType::FADE_OUT:
                // param1=duration, param3=brightness
                event = new LedFadeOut(param1, param3 > 0 ? param3 : 100);
                break;
            case LightFxEventType::FADING:
                // param1=cycle, param2=duration, param3=min, param4=max
                event = new LedFading(param1, param2, param3, param4 > 0 ? param4 : 100);
                break;
            default:
                return LightFxError::INVALID_EVENT;
        }
        
        if (!event) return LightFxError::INVALID_EVENT;
        
        if (!seq.add(event)) {
            delete event;
            return LightFxError::SEQ_FULL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_START callback
    lightfxServer.onLedSeqStart([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (!ledEnabled[i]) continue;
                if (ledSequences[i].count() > 0) {
                    ledSequences[i].start();
                }
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
            ledSequences[channel - 1].start();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_STOP callback
    lightfxServer.onLedSeqStop([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (!ledEnabled[i]) continue;
                ledSequences[i].stop();
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
            ledSequences[channel - 1].stop();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_RESTART callback
    lightfxServer.onLedSeqRestart([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (!ledEnabled[i]) continue;
                if (ledSequences[i].count() > 0) {
                    ledSequences[i].start();
                }
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            if (!ledEnabled[channel - 1]) return LightFxError::CHANNEL_DISABLED;
            ledSequences[channel - 1].start();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_STATUS callback
    lightfxServer.onLedSeqStatus([](uint8_t channel, LightFxSeqStatus& status) {
        if (channel >= 1 && channel <= LED_CHANNEL_COUNT) {
            LedEventSeq& seq = ledSequences[channel - 1];
            status.channel = channel;
            status.playing = seq.isPlaying();
            status.eventCount = seq.count();
            status.currentIndex = seq.currentIndex();
            status.loopCount = seq.loopCount();
        }
    });
    
    // LED_SEQ_QUEUE callback - returns detailed event queue information
    lightfxServer.onLedSeqQueue([](uint8_t channel, LightFxSeqQueue& queue) {
        if (channel >= 1 && channel <= LED_CHANNEL_COUNT) {
            LedEventSeq& seq = ledSequences[channel - 1];
            queue.channel = channel;
            queue.count = seq.count();
            queue.currentIndex = seq.currentIndex();
            queue.playing = seq.isPlaying();
            
            // Populate event details
            for (uint8_t i = 0; i < queue.count && i < 24; i++) {
                ILedEvent* event = seq.eventAt(i);
                if (event) {
                    queue.events[i].type = eventNameToType(event->name());
                    queue.events[i].duration = (uint16_t)event->duration();
                    queue.events[i].param1 = 0;  // Param not accessible from interface
                }
            }
        }
    });
    
    // LED_STATUS callback
    lightfxServer.onLedStatus([](uint8_t channel, LightFxChannelStatus& status) {
        if (channel >= 1 && channel <= LED_CHANNEL_COUNT) {
            status.channel = channel;
            status.brightness = ledChannels[channel - 1].brightness();
            status.seqPlaying = ledSequences[channel - 1].isPlaying();
            status.seqEventCount = ledSequences[channel - 1].count();
        }
    });
    
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
            &servos[servoId - 1], &ledChannels[ledChannel - 1],
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
    
    // LED_MASTER_BRIGHTNESS callback — sets master scaling on all LED channels
    lightfxServer.onLedMasterBrightness([](uint8_t pct) -> uint8_t {
        for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
            ledChannels[i].setMasterBrightness_pct(pct);
        }
        return LightFxError::OK;
    });
    
    // LED_RESET callback — stop sequence, clear, turn off, re-enable
    lightfxServer.onLedReset([](uint8_t channel) -> uint8_t {
        auto resetChannel = [](uint8_t idx) {
            ledSequences[idx].stop();
            ledSequences[idx].clear();
            ledChannels[idx].off();
            ledEnabled[idx] = true;
        };
        
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                resetChannel(i);
            }
            // Also reset master brightness
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                ledChannels[i].setMasterBrightness_pct(100);
            }
        } else {
            resetChannel(channel - 1);
        }
        return LightFxError::OK;
    });
    
    // LED_ENABLE callback — enable/disable LED channel
    lightfxServer.onLedEnable([](uint8_t channel, uint8_t enabled) -> uint8_t {
        bool en = (enabled != 0);
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (!en) {
                    // Disable: stop activity first
                    ledSequences[i].stop();
                    ledChannels[i].off();
                }
                ledEnabled[i] = en;
            }
        } else {
            if (!en) {
                // Disable: stop activity first
                ledSequences[channel - 1].stop();
                ledChannels[channel - 1].off();
            }
            ledEnabled[channel - 1] = en;
        }
        return LightFxError::OK;
    });
    
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    server.begin("LightFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });
    
    // Initialize LED channels with PWM
    for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
        ledChannels[i].begin(LED_CHANNEL_PINS[i], false, true);
        ledChannels[i].off();
        ledSequences[i].attachLed(&ledChannels[i]);
        ledEnabled[i] = true;  // All channels enabled by default
    }
    
    // Initialize servos
    const uint8_t servo_pins[3] = {PIN_SERVO_1, PIN_SERVO_2, PIN_SERVO_3};
    for (int i = 0; i < 3; i++) {
        servos[i].begin(servo_pins[i], SERVO_DEFAULT_US);
        servos[i].setMotionProfile(SERVO_DEFAULT_MAX_SPEED, SERVO_DEFAULT_ACCEL, SERVO_DEFAULT_DECEL);
    }
    
    // Initialize LightFxServer
    lightfxServer.begin(&Serial);
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
            buf[i] = ledChannels[i].brightness();
        }
        
        // LED sequence playing flags (1 byte, bit per channel)
        uint8_t seqFlags = 0;
        for (uint8_t i = 0; i < 8; i++) {
            if (ledSequences[i].isPlaying()) seqFlags |= (1 << i);
        }
        buf[8] = seqFlags;
        
        // Servo positions (6 bytes)
        CoreProtocol::putU16LE(&buf[9], (uint16_t)servos[0].position());
        CoreProtocol::putU16LE(&buf[11], (uint16_t)servos[1].position());
        CoreProtocol::putU16LE(&buf[13], (uint16_t)servos[2].position());
        
        // Landing light states (3 bytes)
        for (uint8_t i = 0; i < LANDING_LIGHT_COUNT; i++) {
            buf[15 + i] = (uint8_t)landingLights[i].state();
        }
        
        // Master brightness percentage (1 byte)
        buf[18] = ledChannels[0].masterBrightness_pct();  // all channels share same value
        
        // LED enabled flags (1 byte, bit per channel, 1=enabled)
        uint8_t enabledFlags = 0;
        for (uint8_t i = 0; i < 8; i++) {
            if (ledEnabled[i]) enabledFlags |= (1 << i);
        }
        buf[19] = enabledFlags;
        
        return 20;
    });
    
    // Finalize router (core handler + LightFX handler)
    server.addModuleHandler(&lightfxServer);
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    // Process protocol, connection timeout, indicators
    server.loop();
    
    // Update LED sequences
    updateLedSequences();
    
    // Update servos
    updateServos();
    
    // Update landing light sequencers
    updateLandingLights();
    
    busy_wait_ms(1);
}
