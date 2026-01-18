/**
 * LightFX Pico Controller v0.2.0
 * 
 * Slave controller for lighting effects - receives commands from HubFX over USB serial.
 * Controls: 8-channel LED outputs with sequences, 3 servos, power monitoring via INA226.
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 * 
 * Architecture (Chain of Responsibility):
 *   - CoreCommandHandler: Handles INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE
 *   - LightFxSlave: Handles LED, SERVO, POWER commands
 *   - CommandRouter: Routes packets to handlers in priority order
 * 
 * Pin Assignments:
 *   LED Channels: GPIO21-28 (8 channels, PWM capable)
 *   Status LEDs: GPIO13 (blue), GPIO14 (yellow)
 *   Servos: GPIO1-3
 *   I2C (INA226): SDA=GPIO4, SCL=GPIO5
 */

#include <Arduino.h>
#include <Servo.h>
#include <Wire.h>
#include <serial.h>
#include <led_control.h>
#include <led_event_seq.h>
#include <led_events.h>
#include <srv_control.h>
#include <pico/unique_id.h>
#include "ina226.h"

// ============================================================================
//  FIRMWARE INFO
// ============================================================================

#define FIRMWARE_VERSION "0.2.0"
#define BUILD_NUMBER 1

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

// Status LEDs
const uint8_t PIN_LED_BLUE   = 13;
const uint8_t PIN_LED_YELLOW = 14;

// Servos
const uint8_t PIN_SERVO_1 = 1;
const uint8_t PIN_SERVO_2 = 2;
const uint8_t PIN_SERVO_3 = 3;

// I2C for INA226 Power Monitor
const uint8_t PIN_I2C_SDA = 4;
const uint8_t PIN_I2C_SCL = 5;

// Array of LED channel pins
const uint8_t LED_CHANNEL_PINS[8] = {
    PIN_LED_CH1, PIN_LED_CH2, PIN_LED_CH3, PIN_LED_CH4,
    PIN_LED_CH5, PIN_LED_CH6, PIN_LED_CH7, PIN_LED_CH8
};

// INA226 configuration
const float INA226_SHUNT_OHMS = 0.1f;
const float INA226_MAX_CURRENT = 3.2f;

// ============================================================================
//  CONSTANTS
// ============================================================================

const uint32_t SERIAL_BAUD = 115200;
const uint8_t LED_CHANNEL_COUNT = 8;

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;
const int SERVO_DEFAULT_MAX_SPEED  = 4000;
const int SERVO_DEFAULT_ACCEL      = 8000;
const int SERVO_DEFAULT_DECEL      = 8000;

// Status LED timing
const uint32_t BLUE_LED_WATCHDOG_ON_MS  = 1000;
const uint32_t BLUE_LED_WATCHDOG_OFF_MS = 2000;

// Connection timeout
const unsigned long CONNECTION_TIMEOUT_MS = 15000;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Serial protocol handlers
CommandRouter commandRouter;
CoreCommandHandler coreHandler;
LightFxSlave lightfxSlave;

// Device identification
char deviceName[24];

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Connection state
bool watchdog_triggered = false;

// LED channels with PWM control
LedControl ledChannels[LED_CHANNEL_COUNT];
LedEventSeq ledSequences[LED_CHANNEL_COUNT];

// Status LEDs
LedControl ledBlue, ledYellow;
uint32_t blue_led_next_toggle_ms = 0;

// Servos with motion profiling
ServoControl servos[3];

// INA226 power monitor
INA226 powerMonitor;
uint32_t last_power_read_ms = 0;
const uint32_t POWER_READ_INTERVAL_MS = 100;

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void buildDeviceName();
void performSafeShutdown();
void performSafeInit();
void checkConnectionStatus();
void updateBlueLED(uint32_t now_ms);
void setLedChannel(uint8_t channel, uint8_t brightness);
void stopAllLedSequences();
void setServoPulse(uint8_t servo_id, int pulse_us);
void setupLightFxCallbacks();

// ============================================================================
//  DEVICE NAME
// ============================================================================

void buildDeviceName() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(deviceName, sizeof(deviceName), "LightFX-%02X%02X", 
             id.id[6], id.id[7]);
}

// ============================================================================
//  LED CHANNEL CONTROL
// ============================================================================

void setLedChannel(uint8_t channel, uint8_t brightness) {
    if (channel < 1 || channel > LED_CHANNEL_COUNT) return;
    ledChannels[channel - 1].setBrightness(brightness);
}

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

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void checkConnectionStatus() {
    if (coreHandler.checkTimeout(CONNECTION_TIMEOUT_MS)) {
        if (!watchdog_triggered) {
            performSafeShutdown();
            watchdog_triggered = true;
        }
    }
}

void performSafeShutdown() {
    stopAllLedSequences();
    
    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }
}

void performSafeInit() {
    stopAllLedSequences();
    watchdog_triggered = false;
    
    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }
}

// ============================================================================
//  STATUS LED CONTROL
// ============================================================================

void updateBlueLED(uint32_t now_ms) {
    if (!coreHandler.isInitialized()) {
        // Not connected: slow blink
        if (now_ms >= blue_led_next_toggle_ms) {
            if (ledBlue.isOn()) {
                ledBlue.off();
                blue_led_next_toggle_ms = now_ms + BLUE_LED_WATCHDOG_OFF_MS;
            } else {
                ledBlue.on();
                blue_led_next_toggle_ms = now_ms + BLUE_LED_WATCHDOG_ON_MS;
            }
        }
    } else {
        // Connected: solid on
        if (!ledBlue.isOn()) {
            ledBlue.on();
        }
    }
}

// ============================================================================
//  LIGHTFX CALLBACKS SETUP
// ============================================================================

void setupLightFxCallbacks() {
    // LED_SET callback
    lightfxSlave.onLedSet([](uint8_t channel, uint8_t brightness) -> uint8_t {
        if (channel < 1 || channel > LED_CHANNEL_COUNT) {
            return LightFxError::INVALID_CHANNEL;
        }
        
        // Stop any running sequence on this channel
        ledSequences[channel - 1].stop();
        ledChannels[channel - 1].setBrightness(brightness);
        return LightFxError::OK;
    });
    
    // LED_OFF callback
    lightfxSlave.onLedOff([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            // All channels
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                ledSequences[i].stop();
                ledChannels[i].off();
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            ledSequences[channel - 1].stop();
            ledChannels[channel - 1].off();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_CLEAR callback
    lightfxSlave.onLedSeqClear([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                ledSequences[i].stop();
                ledSequences[i].clear();
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            ledSequences[channel - 1].stop();
            ledSequences[channel - 1].clear();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_ADD callback (binary format)
    lightfxSlave.onLedSeqAdd([](uint8_t channel, uint8_t eventType,
                                 uint16_t param1, uint16_t param2,
                                 uint8_t param3, uint8_t param4) -> uint8_t {
        if (channel < 1 || channel > LED_CHANNEL_COUNT) {
            return LightFxError::INVALID_CHANNEL;
        }
        
        LedEventSeq& seq = ledSequences[channel - 1];
        if (seq.isFull()) return LightFxError::SEQ_FULL;
        
        ILedEvent* event = nullptr;
        
        switch (eventType) {
            case LightFxEventType::ON:
                // param1=duration, param3=brightness
                event = new LedOn(param1, param3 > 0 ? param3 : 255);
                break;
            case LightFxEventType::OFF:
                // param1=duration
                event = new LedOff(param1);
                break;
            case LightFxEventType::FLASH:
                // param1=interval, param2=duration, param3=brightness, param4=duty
                event = new LedFlashing(param1, param2, param3 > 0 ? param3 : 255, param4 > 0 ? param4 : 50);
                break;
            case LightFxEventType::FADE_IN:
                // param1=duration, param3=brightness
                event = new LedFadeIn(param1, param3 > 0 ? param3 : 255);
                break;
            case LightFxEventType::FADE_OUT:
                // param1=duration, param3=brightness
                event = new LedFadeOut(param1, param3 > 0 ? param3 : 255);
                break;
            case LightFxEventType::FADING:
                // param1=cycle, param2=duration, param3=min, param4=max
                event = new LedFading(param1, param2, param3, param4 > 0 ? param4 : 255);
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
    lightfxSlave.onLedSeqStart([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (ledSequences[i].count() > 0) {
                    ledSequences[i].start();
                }
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            ledSequences[channel - 1].start();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_STOP callback
    lightfxSlave.onLedSeqStop([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                ledSequences[i].stop();
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            ledSequences[channel - 1].stop();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_RESTART callback
    lightfxSlave.onLedSeqRestart([](uint8_t channel) -> uint8_t {
        if (channel == 0) {
            for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
                if (ledSequences[i].count() > 0) {
                    ledSequences[i].start();
                }
            }
        } else if (channel <= LED_CHANNEL_COUNT) {
            ledSequences[channel - 1].start();
        } else {
            return LightFxError::INVALID_CHANNEL;
        }
        return LightFxError::OK;
    });
    
    // LED_SEQ_STATUS callback
    lightfxSlave.onLedSeqStatus([](uint8_t channel, LightFxSeqStatus& status) {
        if (channel >= 1 && channel <= LED_CHANNEL_COUNT) {
            LedEventSeq& seq = ledSequences[channel - 1];
            status.channel = channel;
            status.playing = seq.isPlaying();
            status.eventCount = seq.count();
            status.currentIndex = seq.currentIndex();
            status.loopCount = seq.loopCount();
        }
    });
    
    // LED_STATUS callback
    lightfxSlave.onLedStatus([](uint8_t channel, LightFxChannelStatus& status) {
        if (channel >= 1 && channel <= LED_CHANNEL_COUNT) {
            status.channel = channel;
            status.brightness = ledChannels[channel - 1].brightness();
            status.seqPlaying = ledSequences[channel - 1].isPlaying();
            status.seqEventCount = ledSequences[channel - 1].count();
        }
    });
    
    // SERVO_SET callback
    lightfxSlave.onServoSet([](uint8_t id, int pulseUs) -> uint8_t {
        if (id < 1 || id > 3) return LightFxError::INVALID_SERVO;
        servos[id - 1].setTarget(pulseUs);
        return LightFxError::OK;
    });
    
    // SERVO_SETTINGS callback
    lightfxSlave.onServoSettings([](uint8_t id, int minUs, int maxUs, 
                                     int speed, int accel, int decel) -> uint8_t {
        if (id < 1 || id > 3) return LightFxError::INVALID_SERVO;
        
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
    
    // POWER_STATUS callback
    lightfxSlave.onPowerStatus([](LightFxPowerStatus& status) {
        status.available = powerMonitor.isAvailable();
        if (status.available) {
            status.voltage = powerMonitor.busVoltage();
            status.current = powerMonitor.current() * 1000.0f;  // A to mA
            status.power = powerMonitor.power() * 1000.0f;      // W to mW
        }
    });
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Build unique device name
    buildDeviceName();
    
    // Initialize serial
    Serial.begin(SERIAL_BAUD);
    while (!Serial && millis() < 3000) delay(10);
    
    // Initialize I2C for INA226
    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    Wire.begin();
    Wire.setClock(400000);
    
    // Initialize INA226 power monitor
    powerMonitor.begin(Wire, INA226Address::DEFAULT, INA226_SHUNT_OHMS, INA226_MAX_CURRENT);
    
    // Initialize status LEDs
    ledBlue.begin(PIN_LED_BLUE, false, false);
    ledYellow.begin(PIN_LED_YELLOW, false, false);
    ledBlue.off();
    ledYellow.off();
    
    // Initialize LED channels with PWM
    for (uint8_t i = 0; i < LED_CHANNEL_COUNT; i++) {
        ledChannels[i].begin(LED_CHANNEL_PINS[i], false, true);
        ledChannels[i].off();
        ledSequences[i].attachLed(&ledChannels[i]);
    }
    
    // Initialize servos
    const uint8_t servo_pins[3] = {PIN_SERVO_1, PIN_SERVO_2, PIN_SERVO_3};
    for (int i = 0; i < 3; i++) {
        servos[i].begin(servo_pins[i], SERVO_DEFAULT_US);
        servos[i].setMotionProfile(SERVO_DEFAULT_MAX_SPEED, SERVO_DEFAULT_ACCEL, SERVO_DEFAULT_DECEL);
    }
    
    // ========================================================================
    // Initialize CoreCommandHandler (system commands)
    // ========================================================================
    coreHandler.begin(&Serial);
    coreHandler.setBoardInfo(deviceName, FIRMWARE_VERSION, "RP2040",
                              133, 256*1024, BUILD_NUMBER);
    
    coreHandler.onInit([]() {
        performSafeInit();
    });
    
    coreHandler.onShutdown([]() {
        performSafeShutdown();
    });
    
    coreHandler.onReboot([]() {
        performSafeShutdown();
        delay(100);
        rp2040.reboot();
    });
    
    coreHandler.onBootsel([]() {
        performSafeShutdown();
        delay(500);
        rp2040.rebootToBootloader();
    });
    
    // ========================================================================
    // Initialize LightFxSlave (LightFX-specific commands)
    // ========================================================================
    lightfxSlave.begin(&Serial);
    setupLightFxCallbacks();
    
    // ========================================================================
    // Initialize CommandRouter (routes packets to handlers)
    // ========================================================================
    commandRouter.begin(&Serial, [](uint8_t code, uint8_t type) {
        lightfxSlave.sendNack(code);
    });
    
    // Add LightFX handler to router
    commandRouter.addHandler(&lightfxSlave);
    
    // Initial LED blink
    ledYellow.on();
    delay(200);
    ledYellow.off();
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    uint32_t now = millis();
    
    // Process serial commands via CommandRouter
    commandRouter.process();
    
    // Update activity timestamp for core handler timeout detection
    if (commandRouter.lastActivityMs() > coreHandler.lastActivityMs()) {
        coreHandler.updateActivity();
    }
    
    // Check connection status
    checkConnectionStatus();
    
    // Update LED sequences
    updateLedSequences();
    
    // Update servos
    updateServos();
    
    // Update power monitoring
    if (now - last_power_read_ms >= POWER_READ_INTERVAL_MS) {
        last_power_read_ms = now;
        powerMonitor.update();
    }
    
    // Update status LED
    updateBlueLED(now);
}
