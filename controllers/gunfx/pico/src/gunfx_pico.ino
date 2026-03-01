/**
 * GunFX Pico Controller v0.3.0
 * 
 * Server controller for gun effects - receives commands from HubFX over USB serial.
 * Controls: muzzle flash LED (PWM), smoke heater/fan, 3x gun servos with motion profiling.
 * 
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 * 
 * Architecture (Chain of Responsibility):
 *   - CoreCommandServer: Handles INIT, SHUTDOWN, REBOOT, BOOTSEL, KEEPALIVE
 *   - GunFxServer: Handles TRIGGER, SERVO, SMOKE commands
 *   - CommandRouter: Routes packets to handlers in priority order
 */

#include <Arduino.h>
#include <Servo.h>
#include <math.h>
#include <serial.h>
#include <led_control.h>
#include <srv_control.h>
#include <pico/unique_id.h>

// Firmware version
#define FIRMWARE_VERSION "0.3.0"
#define BUILD_NUMBER 3

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

const uint8_t PIN_GUN_SRV_1    =  1;   // Gun servo 1
const uint8_t PIN_GUN_SRV_2    =  2;   // Gun servo 2
const uint8_t PIN_GUN_SRV_3    =  3;   // Gun servo 3
const uint8_t PIN_LED_BLUE     = 13;   // Status LED (connection/firing)
const uint8_t PIN_LED_YELLOW   = 14;   // Heater indicator LED
const uint8_t PIN_SMOKE_FAN    = 16;   // Smoke fan motor relay
const uint8_t PIN_SMOKE_HEATER = 17;   // Smoke heater relay
const uint8_t PIN_NOZZLE_FLASH = 25;   // Muzzle flash LED (PWM)

// ============================================================================
//  CONSTANTS
// ============================================================================

// Serial communication
const uint32_t SERIAL_BAUD = 115200;

// Muzzle flash timing
const uint8_t  FLASH_PWM_DUTY   = 255;    // Full brightness
const uint16_t FLASH_PULSE_MS   = 30;     // Duration at full brightness
const uint16_t FLASH_FADE_MS    = 80;     // Fade-out duration

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;    // Center position
const int SERVO_DEFAULT_MAX_SPEED  = 4000;    // μs/sec
const int SERVO_DEFAULT_ACCEL      = 8000;    // μs/sec²
const int SERVO_DEFAULT_DECEL      = 8000;    // μs/sec²

// Smoke fan defaults
const uint8_t  SMOKE_FAN_DEFAULT_SPEED      = 255;
const uint8_t  SMOKE_FAN_DEFAULT_PULSE_HIGH = 255;
const uint8_t  SMOKE_FAN_DEFAULT_PULSE_LOW  = 80;
const uint16_t SMOKE_FAN_DEFAULT_PULSE_MS   = 50;
const uint16_t SMOKE_FAN_DEFAULT_SPINDOWN_MS = 5000;

// Connection timeout
const unsigned long CONNECTION_TIMEOUT_MS = 15000;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Serial protocol handlers
CommandRouter commandRouter;
CoreCommandServer coreServer;
GunFxServer gunfxServer;

// Device identification
char deviceName[24];

// ============================================================================
//  STATE VARIABLES
// ============================================================================

// Connection state
bool watchdog_triggered = false;

// Firing state
bool is_firing = false;
int rate_of_fire_rpm = 0;
uint32_t shot_interval_ms = 0;
uint32_t next_shot_time_ms = 0;

// Muzzle flash state
bool flash_active = false;
bool flash_fading = false;
uint32_t flash_off_time_ms = 0;
uint32_t fade_start_time_ms = 0;

// Smoke generator state
bool smoke_heater_on = false;
bool smoke_fan_on = false;
uint8_t smoke_fan_speed = SMOKE_FAN_DEFAULT_SPEED;
bool smoke_fan_pending_off = false;
uint32_t smoke_fan_off_time_ms = 0;

// Session metrics
uint32_t total_shots_fired = 0;
uint32_t heater_on_start_ms = 0;
uint32_t total_heater_on_time_ms = 0;

// Smoke fan mode
enum class SmokeFanMode : uint8_t { Constant = 0, Pulsing = 1 };
SmokeFanMode smoke_fan_mode = SmokeFanMode::Constant;
bool smoke_fan_pulse_active = false;
uint32_t smoke_fan_pulse_end_ms = 0;

// Smoke fan configuration
uint8_t smoke_fan_cfg_speed = SMOKE_FAN_DEFAULT_SPEED;
uint8_t smoke_fan_cfg_pulse_high = SMOKE_FAN_DEFAULT_PULSE_HIGH;
uint8_t smoke_fan_cfg_pulse_low = SMOKE_FAN_DEFAULT_PULSE_LOW;
uint16_t smoke_fan_cfg_pulse_ms = SMOKE_FAN_DEFAULT_PULSE_MS;
uint16_t smoke_fan_cfg_spindown_ms = SMOKE_FAN_DEFAULT_SPINDOWN_MS;

// Servos with motion profiling
ServoControl gun_servos[3];

struct RecoilJerkConfig { int jerk_us, variance_us; };
RecoilJerkConfig servo_jerk_configs[3] = {{0, 0}, {0, 0}, {0, 0}};

// Status LEDs
LedControl led_blue, led_yellow;
uint32_t blue_led_next_toggle_ms = 0;
const uint32_t BLUE_LED_WATCHDOG_ON_MS  = 1000;
const uint32_t BLUE_LED_WATCHDOG_OFF_MS = 2000;

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void setSmokeHeater(bool on);
void setSmokeFan(bool on, uint8_t speed = SMOKE_FAN_DEFAULT_SPEED);
void setNozzleFlash(bool on);
void stopFiring(uint16_t fanDelayMs);
void startFiring(int rpm);
void setServoPulse(uint8_t servo_id, int pulse_us);
void performSafeShutdown();
void performSafeInit();
GunFxStatus buildCurrentStatus(uint32_t now_ms);

// ============================================================================
//  DEVICE NAME
// ============================================================================

void buildDeviceName() {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(deviceName, sizeof(deviceName), "GunFX-%02X%02X", 
             id.id[6], id.id[7]);
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
    stopFiring(0);
    setSmokeHeater(false);
    setSmokeFan(false, 0);
    setNozzleFlash(false);
    
    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }
}

void performSafeInit() {
    stopFiring(0);
    setSmokeHeater(false);
    watchdog_triggered = false;
    
    for (int i = 0; i < 3; i++) {
        setServoPulse(i + 1, SERVO_DEFAULT_US);
    }
}

// ============================================================================
//  LED CONTROL
// ============================================================================

void updateYellowLED() {
    led_yellow.set(smoke_heater_on);
}

void updateBlueLED(uint32_t now_ms) {
    if (watchdog_triggered) {
        if (now_ms >= blue_led_next_toggle_ms) {
            led_blue.toggle();
            blue_led_next_toggle_ms = now_ms + 
                (led_blue.isOn() ? BLUE_LED_WATCHDOG_ON_MS : BLUE_LED_WATCHDOG_OFF_MS);
        }
    } else if (is_firing && (flash_active || flash_fading)) {
        led_blue.on();
    } else {
        led_blue.off();
    }
}

void updateLEDs(uint32_t now_ms) {
    updateYellowLED();
    updateBlueLED(now_ms);
}

// ============================================================================
//  SERVO CONTROL
// ============================================================================

void setServoPulse(uint8_t servo_id, int pulse_us) {
    if (servo_id == 0 || servo_id > 3) return;
    ServoControl* servo = &gun_servos[servo_id - 1];
    int target = constrain(pulse_us, servo->minLimit(), servo->maxLimit());
    servo->setTarget(target);
}

void applyRecoilJerk() {
    for (int i = 0; i < 3; i++) {
        RecoilJerkConfig* jerk = &servo_jerk_configs[i];
        if (jerk->jerk_us == 0) {
            gun_servos[i].clearJerk();
        } else {
            int direction = (random(2) == 0) ? 1 : -1;
            int variance = jerk->variance_us > 0 ? random(jerk->variance_us + 1) : 0;
            gun_servos[i].applyJerk(direction * (jerk->jerk_us + variance));
        }
    }
}

void clearRecoilJerk() {
    for (int i = 0; i < 3; i++) gun_servos[i].clearJerk();
}

void updateAllServos() {
    for (int i = 0; i < 3; i++) {
        gun_servos[i].update();
    }
}

// ============================================================================
//  HARDWARE OUTPUT CONTROL
// ============================================================================

void setNozzleFlash(bool on) {
    analogWrite(PIN_NOZZLE_FLASH, on ? FLASH_PWM_DUTY : 0);
}

void setSmokeHeater(bool on) {
    if (on && !smoke_heater_on) {
        heater_on_start_ms = millis();
    } else if (!on && smoke_heater_on && heater_on_start_ms > 0) {
        total_heater_on_time_ms += millis() - heater_on_start_ms;
        heater_on_start_ms = 0;
    }
    
    smoke_heater_on = on;
    digitalWrite(PIN_SMOKE_HEATER, on ? HIGH : LOW);
}

void setSmokeFan(bool on, uint8_t speed) {
    smoke_fan_on = on;
    smoke_fan_speed = speed;
    smoke_fan_pending_off = false;
    smoke_fan_pulse_active = false;
    analogWrite(PIN_SMOKE_FAN, on ? speed : 0);
}

void scheduleSmokeFanOff(uint16_t delay_ms) {
    if (delay_ms == 0) {
        setSmokeFan(false, 0);
    } else {
        smoke_fan_pending_off = true;
        smoke_fan_off_time_ms = millis() + delay_ms;
    }
}

void triggerSmokeFanPulse() {
    if (smoke_fan_mode != SmokeFanMode::Pulsing || !smoke_fan_on) return;
    
    smoke_fan_pulse_active = true;
    smoke_fan_pulse_end_ms = millis() + smoke_fan_cfg_pulse_ms;
    analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_high);
    smoke_fan_speed = smoke_fan_cfg_pulse_high;
}

// ============================================================================
//  FIRING CONTROL
// ============================================================================

void startFiring(int rpm) {
    if (rpm <= 0) return;
    is_firing = true;
    rate_of_fire_rpm = rpm;
    shot_interval_ms = 60000UL / rpm;
    next_shot_time_ms = millis();
    
    if (smoke_fan_mode == SmokeFanMode::Pulsing) {
        setSmokeFan(true, smoke_fan_cfg_pulse_low);
    } else {
        setSmokeFan(true, smoke_fan_cfg_speed);
    }
}

void stopFiring(uint16_t fan_delay_ms) {
    is_firing = false;
    rate_of_fire_rpm = 0;
    setNozzleFlash(false);
    flash_active = false;
    scheduleSmokeFanOff(fan_delay_ms);
}

// ============================================================================
//  UPDATE FUNCTIONS
// ============================================================================

void updateMuzzleFlash() {
    if (!is_firing) {
        if (flash_active || flash_fading) {
            setNozzleFlash(false);
            flash_active = false;
            flash_fading = false;
            clearRecoilJerk();
        }
        return;
    }
    
    uint32_t now = millis();
    
    if (flash_fading) {
        uint32_t fade_elapsed = now - fade_start_time_ms;
        if (fade_elapsed >= FLASH_FADE_MS) {
            setNozzleFlash(false);
            flash_fading = false;
            clearRecoilJerk();
        } else {
            uint16_t brightness = map(fade_elapsed, 0, FLASH_FADE_MS, FLASH_PWM_DUTY, 0);
            analogWrite(PIN_NOZZLE_FLASH, (uint8_t)brightness);
        }
    } else if (flash_active) {
        if (now >= flash_off_time_ms) {
            flash_active = false;
            flash_fading = true;
            fade_start_time_ms = now;
        }
    } else if (now >= next_shot_time_ms) {
        setNozzleFlash(true);
        flash_active = true;
        flash_off_time_ms = now + FLASH_PULSE_MS;
        next_shot_time_ms = now + shot_interval_ms;
        applyRecoilJerk();
        triggerSmokeFanPulse();
        total_shots_fired++;
    }
}

void updateSmokeFan() {
    if (smoke_fan_pending_off) {
        if (millis() >= smoke_fan_off_time_ms) {
            setSmokeFan(false, 0);
        }
        return;
    }
    
    if (smoke_fan_mode == SmokeFanMode::Pulsing && smoke_fan_pulse_active) {
        if (millis() >= smoke_fan_pulse_end_ms) {
            smoke_fan_pulse_active = false;
            if (smoke_fan_on && is_firing) {
                analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_low);
                smoke_fan_speed = smoke_fan_cfg_pulse_low;
            }
        }
    }
}

GunFxStatus buildCurrentStatus(uint32_t now_ms) {
    GunFxStatus status;
    
    status.firing = is_firing;
    status.flashActive = flash_active;
    status.flashFading = flash_fading;
    status.heaterOn = smoke_heater_on;
    status.fanOn = smoke_fan_on;
    status.fanSpindown = smoke_fan_pending_off;
    status.fanSpeed = smoke_fan_speed;

    if (smoke_fan_pending_off && smoke_fan_off_time_ms > now_ms) {
        status.fanOffRemainingMs = (uint16_t)(smoke_fan_off_time_ms - now_ms);
    } else {
        status.fanOffRemainingMs = 0;
    }

    status.servoUs[0] = (uint16_t)constrain(gun_servos[0].position(), 0, 3000);
    status.servoUs[1] = (uint16_t)constrain(gun_servos[1].position(), 0, 3000);
    status.servoUs[2] = (uint16_t)constrain(gun_servos[2].position(), 0, 3000);

    status.rateOfFireRpm = (uint16_t)rate_of_fire_rpm;
    status.shotsFired = total_shots_fired;
    
    status.heaterOnTimeMs = total_heater_on_time_ms;
    if (smoke_heater_on && heater_on_start_ms > 0) {
        status.heaterOnTimeMs += now_ms - heater_on_start_ms;
    }
    
    status.uptimeMs = now_ms;
    status.freeRam = rp2040.getFreeHeap();

    return status;
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize USB serial
    Serial.begin(SERIAL_BAUD);
    while (!Serial && millis() < 3000) delay(10);
    
    // Initialize GPIO outputs
    pinMode(PIN_NOZZLE_FLASH, OUTPUT);
    pinMode(PIN_SMOKE_FAN, OUTPUT);
    pinMode(PIN_SMOKE_HEATER, OUTPUT);
    analogWrite(PIN_NOZZLE_FLASH, 0);
    analogWrite(PIN_SMOKE_FAN, 0);
    digitalWrite(PIN_SMOKE_HEATER, LOW);
    
    // Initialize status LEDs
    led_blue.begin(PIN_LED_BLUE);
    led_yellow.begin(PIN_LED_YELLOW);
    
    // Initialize servos
    const uint8_t servoPins[] = {PIN_GUN_SRV_1, PIN_GUN_SRV_2, PIN_GUN_SRV_3};
    for (int i = 0; i < 3; i++) {
        gun_servos[i].begin(servoPins[i], 500, 2500, SERVO_DEFAULT_US);
        gun_servos[i].setId(i + 1);
    }
    
    // Build unique device name
    buildDeviceName();
    
    // ========================================================================
    // Initialize CoreCommandServer (system commands)
    // ========================================================================
    coreServer.begin(&Serial);
    coreServer.setBoardInfo(deviceName, FIRMWARE_VERSION, "RP2040", 
                              F_CPU / 1000000, rp2040.getFreeHeap(), BUILD_NUMBER);
    
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
    // Initialize GunFxServer (GunFX-specific commands)
    // ========================================================================
    gunfxServer.begin(&Serial, deviceName);
    
    // TRIGGER_ON: Start firing at specified RPM
    gunfxServer.onTriggerOn([](uint16_t rpm) -> uint8_t {
        if (rpm < 1 || rpm > 3000) {
            return GunFxError::INVALID_RPM;
        }
        startFiring(rpm);
        return SerialError::OK;
    });
    
    // TRIGGER_OFF: Stop firing with optional fan delay
    gunfxServer.onTriggerOff([](uint16_t delay) -> uint8_t {
        stopFiring(delay);
        return SerialError::OK;
    });
    
    // SERVO_SET: Set servo position
    gunfxServer.onServoSet([](uint8_t id, uint16_t us) -> uint8_t {
        if (id < 1 || id > 3) {
            return GunFxError::SERVO_INVALID_ID;
        }
        if (us < 500 || us > 2500) {
            return GunFxError::SERVO_PULSE_RANGE;
        }
        setServoPulse(id, us);
        return SerialError::OK;
    });
    
    // SERVO_SETTINGS: Configure servo motion profile
    gunfxServer.onServoSettings([](const GunFxServoConfig& cfg) -> uint8_t {
        if (cfg.servoId < 1 || cfg.servoId > 3) {
            return GunFxError::SERVO_INVALID_ID;
        }
        uint8_t idx = cfg.servoId - 1;
        
        if (cfg.minUs > 0 && (cfg.minUs < 500 || cfg.minUs > 2500)) {
            return GunFxError::SERVO_PULSE_RANGE;
        }
        if (cfg.maxUs > 0 && (cfg.maxUs < 500 || cfg.maxUs > 2500)) {
            return GunFxError::SERVO_PULSE_RANGE;
        }
        if (cfg.minUs > 0 && cfg.maxUs > 0 && cfg.minUs >= cfg.maxUs) {
            return GunFxError::SERVO_PULSE_RANGE;
        }
        
        if (cfg.recoilJerkUs > 0 || cfg.recoilJerkVarianceUs > 0) {
            servo_jerk_configs[idx].jerk_us = cfg.recoilJerkUs;
            servo_jerk_configs[idx].variance_us = cfg.recoilJerkVarianceUs;
        }
        
        if (cfg.minUs > 0 || cfg.maxUs > 0) {
            gun_servos[idx].setLimits(cfg.minUs, cfg.maxUs);
        }
        if (cfg.maxSpeedUsPerSec > 0 || cfg.maxAccelUsPerSec2 > 0 || cfg.maxDecelUsPerSec2 > 0) {
            gun_servos[idx].setMotionProfile(cfg.maxSpeedUsPerSec, cfg.maxAccelUsPerSec2, cfg.maxDecelUsPerSec2);
        }
        return SerialError::OK;
    });
    
    // SMOKE_HEAT: Enable/disable smoke heater
    gunfxServer.onSmokeHeat([](bool on) -> uint8_t {
        setSmokeHeater(on);
        return SerialError::OK;
    });
    
    // SMOKE_SETTINGS: Configure smoke fan parameters
    gunfxServer.onSmokeSettings([](const GunFxSmokeConfig& cfg) -> uint8_t {
        if (cfg.fanPulseMs > 10000) {
            return GunFxError::INVALID_FAN_SPEED;
        }
        if (cfg.fanSpindownMs > 60000) {
            return GunFxError::INVALID_FAN_SPEED;
        }
        
        smoke_fan_mode = cfg.fanPulsing ? SmokeFanMode::Pulsing : SmokeFanMode::Constant;
        smoke_fan_cfg_speed = cfg.fanSpeed;
        smoke_fan_cfg_pulse_high = cfg.fanPulseHigh;
        smoke_fan_cfg_pulse_low = cfg.fanPulseLow;
        smoke_fan_cfg_pulse_ms = cfg.fanPulseMs;
        smoke_fan_cfg_spindown_ms = cfg.fanSpindownMs;
        
        if (is_firing && smoke_fan_on) {
            if (cfg.fanPulsing) {
                analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_pulse_low);
                smoke_fan_speed = smoke_fan_cfg_pulse_low;
            } else {
                analogWrite(PIN_SMOKE_FAN, smoke_fan_cfg_speed);
                smoke_fan_speed = smoke_fan_cfg_speed;
            }
        }
        return SerialError::OK;
    });
    
    // STATUS_REQ: Return current status (via GunFxServer - used when standalone)
    gunfxServer.onStatusRequest([]() -> GunFxStatus {
        return buildCurrentStatus(millis());
    });
    
    // STATUS: Append GunFX module data to core STATUS response
    // Wire format (20 bytes):
    //   [flags:u8][fanSpeed:u8][fanOffMs:u16][servo0:u16][servo1:u16][servo2:u16]
    //   [rpm:u16][shots:u32][heaterMs:u32]
    coreServer.onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 20) return 0;
        
        GunFxStatus s = buildCurrentStatus(millis());
        
        uint8_t flags = 0;
        if (s.firing)       flags |= 0x01;
        if (s.flashActive)  flags |= 0x02;
        if (s.flashFading)  flags |= 0x04;
        if (s.heaterOn)     flags |= 0x08;
        if (s.fanOn)        flags |= 0x10;
        if (s.fanSpindown)  flags |= 0x20;
        
        buf[0] = flags;
        buf[1] = s.fanSpeed;
        CoreProtocol::putU16LE(&buf[2], s.fanOffRemainingMs);
        CoreProtocol::putU16LE(&buf[4], s.servoUs[0]);
        CoreProtocol::putU16LE(&buf[6], s.servoUs[1]);
        CoreProtocol::putU16LE(&buf[8], s.servoUs[2]);
        CoreProtocol::putU16LE(&buf[10], s.rateOfFireRpm);
        CoreProtocol::putU32LE(&buf[12], s.shotsFired);
        CoreProtocol::putU32LE(&buf[16], s.heaterOnTimeMs);
        
        return 20;
    });
    
    // ========================================================================
    // Initialize CommandRouter (routes packets to handlers)
    // ========================================================================
    commandRouter.begin(&Serial, [](uint8_t code, uint8_t type) {
        gunfxServer.sendNack(code);
    });
    
    // Add handlers to router (order = priority)
    commandRouter.addHandler(&coreServer);      // Priority 1: core/system commands
    commandRouter.addHandler(&gunfxServer);      // Priority 2: GunFX commands
}

// ============================================================================
//  LOOP
// ============================================================================

void loop() {
    // Process incoming serial packets via CommandRouter
    // The router decodes COBS packets and routes to handlers
    commandRouter.process();
    
    // Update activity timestamp for core handler timeout detection
    if (commandRouter.lastActivityMs() > coreServer.lastActivityMs()) {
        coreServer.updateActivity();
    }
    
    // Keep free RAM current for STATUS response
    coreServer.updateFreeRam(rp2040.getFreeHeap());
    
    // Update hardware states
    updateMuzzleFlash();
    updateSmokeFan();
    updateAllServos();
    
    uint32_t now = millis();
    updateLEDs(now);
    
    // Check connection timeout
    checkConnectionStatus();
    
    delay(1);
}
