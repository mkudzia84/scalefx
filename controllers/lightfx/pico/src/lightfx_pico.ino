/**
 * LightFX Pico Controller v0.2.0
 * 
 * Server controller for lighting effects - receives commands from HubFX over USB serial.
 * Controls: 8-channel LED outputs with sequences, 3 servos, power monitoring via INA226.
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
#define BUILD_NUMBER 4

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

// INA226 configuration (can be updated via POWER_CONFIG command)
float ina226ShuntOhms = 0.1f;
float ina226MaxCurrent = 3.2f;

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
CoreCommandServer coreServer;
LightFxServer lightfxServer;

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
    if (coreServer.checkTimeout(CONNECTION_TIMEOUT_MS)) {
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
    if (!coreServer.isInitialized()) {
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
    // LED_SET callback
    lightfxServer.onLedSet([](uint8_t channel, uint8_t brightness) -> uint8_t {
        if (channel < 1 || channel > LED_CHANNEL_COUNT) {
            return LightFxError::INVALID_CHANNEL;
        }
        
        // Stop any running sequence on this channel
        ledSequences[channel - 1].stop();
        ledChannels[channel - 1].setBrightness(brightness);
        return LightFxError::OK;
    });
    
    // LED_OFF callback
    lightfxServer.onLedOff([](uint8_t channel) -> uint8_t {
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
    lightfxServer.onLedSeqClear([](uint8_t channel) -> uint8_t {
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
    lightfxServer.onLedSeqAdd([](uint8_t channel, uint8_t eventType,
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
    lightfxServer.onLedSeqStart([](uint8_t channel) -> uint8_t {
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
    lightfxServer.onLedSeqStop([](uint8_t channel) -> uint8_t {
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
    lightfxServer.onLedSeqRestart([](uint8_t channel) -> uint8_t {
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
    
    // SERVO_SET callback
    lightfxServer.onServoSet([](uint8_t id, int pulseUs) -> uint8_t {
        if (id < 1 || id > 3) return LightFxError::INVALID_SERVO;
        servos[id - 1].setTarget(pulseUs);
        return LightFxError::OK;
    });
    
    // SERVO_SETTINGS callback
    lightfxServer.onServoSettings([](uint8_t id, int minUs, int maxUs, 
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
    lightfxServer.onPowerStatus([](LightFxPowerStatus& status) {
        status.available = powerMonitor.isAvailable();
        if (status.available) {
            status.voltage = powerMonitor.busVoltage();
            status.current = powerMonitor.current() * 1000.0f;  // A to mA
            status.power = powerMonitor.power() * 1000.0f;      // W to mW
        }
        // Include current shunt config
        status.shuntMohm = (uint16_t)(ina226ShuntOhms * 1000.0f);  // Ohms to milliohms
        status.maxCurrentMa = (uint16_t)(ina226MaxCurrent * 1000.0f);  // A to mA
    });
    
    // POWER_CONFIG callback - set INA226 shunt resistance and max current
    lightfxServer.onPowerConfig([](uint16_t shuntMohm, uint16_t maxCurrentMa) -> uint8_t {
        if (shuntMohm == 0 || maxCurrentMa == 0) {
            return LightFxError::INVALID_PARAM;
        }
        
        ina226ShuntOhms = shuntMohm / 1000.0f;  // milliohms to ohms
        ina226MaxCurrent = maxCurrentMa / 1000.0f;  // mA to A
        
        // Recalibrate INA226 with new values
        if (powerMonitor.isAvailable()) {
            powerMonitor.setCalibration(ina226ShuntOhms, ina226MaxCurrent);
        }
        
        return LightFxError::OK;
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
    powerMonitor.begin(Wire, INA226Address::DEFAULT, ina226ShuntOhms, ina226MaxCurrent);
    
    // Initialize status LEDs (always on)
    ledBlue.begin(PIN_LED_BLUE, false, false);
    ledYellow.begin(PIN_LED_YELLOW, false, false);
    ledBlue.on();
    ledYellow.on();
    
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
    // Initialize CoreCommandServer (system commands)
    // ========================================================================
    coreServer.begin(&Serial);
    coreServer.setBoardInfo(deviceName, FIRMWARE_VERSION, "RP2040",
                              133, 256*1024, BUILD_NUMBER);
    
    coreServer.onInit([]() {
        performSafeInit();
    });
    
    coreServer.onShutdown([]() {
        performSafeShutdown();
    });
    
    coreServer.onReboot([]() {
        performSafeShutdown();
        delay(100);
        rp2040.reboot();
    });
    
    coreServer.onBootsel([]() {
        performSafeShutdown();
        delay(500);
        rp2040.rebootToBootloader();
    });
    
    // ========================================================================
    // Initialize LightFxServer (LightFX-specific commands)
    // ========================================================================
    lightfxServer.begin(&Serial);
    setupLightFxCallbacks();
    
    // STATUS: Append LightFX module data to core STATUS response
    // Wire format (22 bytes):
    //   [ledBrightness:u8×8][ledSeqFlags:u8]
    //   [servo0:u16][servo1:u16][servo2:u16]
    //   [voltage:u16(mV)][current:i16(mA)][power:u16(mW)][powerAvail:u8]
    coreServer.onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 22) return 0;
        
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
        
        // Power monitor (7 bytes)
        if (powerMonitor.isAvailable()) {
            CoreProtocol::putU16LE(&buf[15], (uint16_t)(powerMonitor.busVoltage() * 1000.0f));
            CoreProtocol::putI16LE(&buf[17], (int16_t)(powerMonitor.current() * 1000.0f));
            CoreProtocol::putU16LE(&buf[19], (uint16_t)(powerMonitor.power() * 1000.0f));
            buf[21] = 1;
        } else {
            CoreProtocol::putU16LE(&buf[15], 0);
            CoreProtocol::putI16LE(&buf[17], 0);
            CoreProtocol::putU16LE(&buf[19], 0);
            buf[21] = 0;
        }
        
        return 22;
    });
    
    // ========================================================================
    // Initialize CommandRouter (routes packets to handlers)
    // ========================================================================
    commandRouter.begin(&Serial, [](uint8_t code, uint8_t type) {
        lightfxServer.sendNack(code);
    });
    
    // Add handlers to router (order = priority)
    commandRouter.addHandler(&coreServer);      // Priority 1: core/system commands
    commandRouter.addHandler(&lightfxServer);    // Priority 2: LightFX commands
    
    // Initial LED blink
    ledYellow.off();
    delay(200);
    ledYellow.on();
}

// ============================================================================
//  MAIN LOOP
// ============================================================================

void loop() {
    uint32_t now = millis();
    
    // Process serial commands via CommandRouter
    commandRouter.process();
    
    // Update activity timestamp for core handler timeout detection
    if (commandRouter.lastActivityMs() > coreServer.lastActivityMs()) {
        coreServer.updateActivity();
    }
    
    // Keep free RAM current for STATUS response
    coreServer.updateFreeRam(rp2040.getFreeHeap());
    
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
    
}
