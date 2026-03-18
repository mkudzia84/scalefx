/**
 * GunFX Pico Controller v0.5.0
 *
 * Server controller for gun effects - receives commands from HubFX over USB serial.
 * Controls: muzzle flash LED (PWM), smoke heater/fan, 3× gun servos with motion profiling.
 *
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 *
 * Architecture (Chain of Responsibility):
 *   - SfxServer: Common server boilerplate (serial, indicators, core protocol)
 *   - GunFxServer: Handles TRIGGER, SERVO, SMOKE commands
 *   - CommandRouter: Routes packets to handlers in priority order
 *
 * Module Architecture:
 *   - MuzzleFlash: Flash LED pulse/fade, per-shot recoil jerk on servos
 *   - SmokeGenerator: Heater relay + PWM fan (constant/pulsing modes)
 *   - ServoControl (lib): Motion-profiled servo positioning
 *
 * GPIO Pin Mapping (using GearControl board — LEDs match motor channels):
 *   GP1-3:   Gun servos 1-3 (GearControl door servos 1-3)
 *   GP4:     I2C SDA (INA226 current monitors)
 *   GP5:     I2C SCL
 *   GP9:     Nozzle flash LED (PWM) (GearControl yaw servo output)
 *   GP13:    Indicator LED (connection status)
 *   GP14:    Indicator LED (error status)
 *   GP15:    Smoke heater relay (GearControl motor 0 CW)
 *   GP16:    Motor 0 CCW — held LOW for single-direction drive
 *   GP17:    Smoke fan motor (PWM) (GearControl motor 1 CW)
 *   GP18:    Motor 1 CCW — held LOW for single-direction drive
 *   GP21:    Status LED — heater on (Motor 0 CW LED)
 *   GP23:    Status LED — fan running (Motor 1 CW LED)
 *   GP24:    Status LED — fan spinning down (Motor 1 CCW LED)
 *   GP25:    Status LED — firing active (Motor 2 CW LED)
 *   GP26:    Status LED — flash pulse (Motor 2 CCW LED)
 *   GP29:    Battery voltage ADC (÷6 divider)
 */

#include <Arduino.h>
#include <Wire.h>
#include <serial/serial.h>
#include <gunfx/server/gunfx_server.h>
#include <servo/srv_control.h>
#include <server/sfx_server.h>
#include <power/ina226.h>
#include <power/battery_monitor.h>
#include "muzzle_flash.h"
#include "smoke_generator.h"

// Firmware version
#define FIRMWARE_VERSION "0.6.0"
#define BUILD_NUMBER 15

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

const uint8_t PIN_GUN_SRV_1    =  1;   // Gun servo 1 (GearControl door servo 1)
const uint8_t PIN_GUN_SRV_2    =  2;   // Gun servo 2 (GearControl door servo 2)
const uint8_t PIN_GUN_SRV_3    =  3;   // Gun servo 3 (GearControl door servo 3)
const uint8_t PIN_SMOKE_HEATER = 15;   // Smoke heater relay (GearControl motor 0 CW)
const uint8_t PIN_SMOKE_HTR_B  = 16;   // Motor 0 CCW — held LOW
const uint8_t PIN_SMOKE_FAN    = 17;   // Smoke fan motor PWM (GearControl motor 1 CW)
const uint8_t PIN_SMOKE_FAN_B  = 18;   // Motor 1 CCW — held LOW
const uint8_t PIN_NOZZLE_FLASH =  9;   // Muzzle flash LED PWM (GearControl yaw servo)

// Status LEDs (GearControl motor channel LEDs — match motor assignments)
const uint8_t PIN_LED_HEATER   = 21;   // Heater on (Motor 0 CW LED)
const uint8_t PIN_LED_FAN      = 23;   // Fan running (Motor 1 CW LED)
const uint8_t PIN_LED_SPINDOWN = 24;   // Fan spinning down (Motor 1 CCW LED)
const uint8_t PIN_LED_FIRING   = 25;   // Firing active (Motor 2 CW LED)
const uint8_t PIN_LED_FLASH    = 26;   // Flash pulse (Motor 2 CCW LED)

// I2C bus (INA226 current monitors)
const uint8_t PIN_SDA          =  4;   // I2C SDA
const uint8_t PIN_SCL          =  5;   // I2C SCL

// Battery voltage sense
const uint8_t PIN_VSENSE       = 29;   // ADC input (÷6 divider)

// ============================================================================
//  CONSTANTS
// ============================================================================

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;    // Center position

// INA226 current monitors (GearControl board shunts)
const float SHUNT_RESISTANCE_OHMS  = 0.005f;  // 5mΩ shunt (§7.6.2 INA226 datasheet)
const float MAX_CURRENT_A          = 10.0f;   // 10A max expected

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Server (serial, core protocol, indicators, connection management)
SfxServer server;
GunFxServer gunfxServer;

// Gun effect modules
MuzzleFlash muzzleFlash;
SmokeGenerator smokeGen;

// Servos with motion profiling
ServoControl gunServos[3];

// INA226 current monitors: [0]=heater (Motor 0), [1]=fan (Motor 1)
INA226 ina226[2];
bool ina226Available[2] = { false, false };
const uint8_t INA226_ADDR[2] = {
    INA226Address::GND_GND,   // 0x40 - Motor 0 (heater)
    INA226Address::VS_GND,    // 0x44 - Motor 1 (fan)
};

// Battery voltage monitor (ADC ÷6 divider)
BatteryMonitor batteryMonitor;

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void performSafeShutdown();
void performSafeInit();

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void performSafeShutdown() {
    SFX_LOG_INFO("Shutdown — stop firing, heater off, servos center");

    // Stop firing (flash off, jerk cleared)
    muzzleFlash.stopFiring();

    // Smoke: heater off, fan off immediately
    smokeGen.shutdown();

    // Return all servos to center
    for (int i = 0; i < 3; i++) {
        gunServos[i].setTarget(SERVO_DEFAULT_US);
    }
}

void performSafeInit() {
    SFX_LOG_INFO("Init — safe reset");

    // Stop any active firing
    muzzleFlash.stopFiring();

    // Heater off (fan left alone — may be spinning down)
    smokeGen.setHeater(false);

    // Return all servos to center
    for (int i = 0; i < 3; i++) {
        gunServos[i].setTarget(SERVO_DEFAULT_US);
    }

    // Clear smoke protection errors
    smokeGen.resetErrors();
}

// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    server.begin("GunFX", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });

    // Hold H-bridge CCW pins LOW (GearControl motor outputs are CW/CCW pairs)
    pinMode(PIN_SMOKE_FAN_B, OUTPUT);
    digitalWrite(PIN_SMOKE_FAN_B, LOW);
    pinMode(PIN_SMOKE_HTR_B, OUTPUT);
    digitalWrite(PIN_SMOKE_HTR_B, LOW);

    // Initialize status LEDs (GearControl board motor channel LEDs)
    const uint8_t statusPins[] = { PIN_LED_HEATER, PIN_LED_FAN, PIN_LED_SPINDOWN, PIN_LED_FIRING, PIN_LED_FLASH };
    for (auto pin : statusPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }

    // Initialize I2C bus (INA226 current monitors)
    Wire.setSDA(PIN_SDA);
    Wire.setSCL(PIN_SCL);
    Wire.begin();
    Wire.setClock(400000);  // 400kHz fast mode

    // Initialize INA226 current monitors (fan + heater shunts)
    for (int i = 0; i < 2; i++) {
        INA226Config cfg;
        cfg.address = INA226_ADDR[i];
        cfg.shuntResistance_ohms = SHUNT_RESISTANCE_OHMS;
        cfg.maxCurrent_A = MAX_CURRENT_A;
        ina226Available[i] = ina226[i].begin(Wire, cfg);
        SFX_LOG_INFO("INA226[%d] (0x%02X): %s", i, INA226_ADDR[i],
                     ina226Available[i] ? "OK" : "NOT FOUND");
    }

    // Initialize battery voltage monitor (ADC ÷6 divider)
    analogReadResolution(12);
    batteryMonitor.begin(PIN_VSENSE, 6.0f);

    // Initialize servos
    const uint8_t servoPins[] = { PIN_GUN_SRV_1, PIN_GUN_SRV_2, PIN_GUN_SRV_3 };
    for (int i = 0; i < 3; i++) {
        gunServos[i].begin(servoPins[i], 500, 2500, SERVO_DEFAULT_US);
        gunServos[i].setId(i + 1);
    }

    // Initialize muzzle flash module
    muzzleFlash.begin(PIN_NOZZLE_FLASH);
    for (int i = 0; i < 3; i++) {
        muzzleFlash.attachServo(i, &gunServos[i]);
    }

    // Initialize smoke generator module
    smokeGen.begin(PIN_SMOKE_HEATER, PIN_SMOKE_FAN);

    // Attach INA226 current monitors to smoke generator
    if (ina226Available[0]) smokeGen.attachCurrentMonitor(SmokeChannel::Heater, &ina226[0]);
    if (ina226Available[1]) smokeGen.attachCurrentMonitor(SmokeChannel::Fan,    &ina226[1]);

    // Wire muzzle flash shot event to smoke pulse
    muzzleFlash.onShot([]() {
        smokeGen.triggerPulse();
    });

    // ========================================================================
    // Initialize GunFxServer (GunFX-specific commands)
    // ========================================================================
    gunfxServer.begin(&Serial);

    // TRIGGER_ON: Start firing at specified RPM
    gunfxServer.onTriggerOn([](uint16_t rpm) -> uint8_t {
        muzzleFlash.startFiring(rpm);
        smokeGen.startFan(rpm);
        return SerialError::OK;
    });

    // TRIGGER_OFF: Stop firing with optional fan delay
    gunfxServer.onTriggerOff([](uint16_t delay) -> uint8_t {
        muzzleFlash.stopFiring();
        smokeGen.stopFan(delay);
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
        uint8_t idx = id - 1;
        int target = constrain(us, gunServos[idx].minLimit(), gunServos[idx].maxLimit());
        gunServos[idx].setTarget(target);
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

        if (cfg.minUs > 0 || cfg.maxUs > 0) {
            gunServos[idx].setLimits(cfg.minUs, cfg.maxUs);
        }
        if (cfg.maxSpeedUsPerSec > 0 || cfg.maxAccelUsPerSec2 > 0 || cfg.maxDecelUsPerSec2 > 0) {
            gunServos[idx].setMotionProfile(cfg.maxSpeedUsPerSec, cfg.maxAccelUsPerSec2, cfg.maxDecelUsPerSec2);
        }
        return SerialError::OK;
    });

    // SRV_RECOIL_JERK: Configure recoil jerk per servo (dedicated callback)
    gunfxServer.onRecoilJerk([](uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) -> uint8_t {
        if (servoId < 1 || servoId > 3) {
            return GunFxError::SERVO_INVALID_ID;
        }
        uint8_t idx = servoId - 1;
        muzzleFlash.setRecoilJerk(idx, jerkUs, varianceUs);
        return SerialError::OK;
    });

    // SMOKE_HEAT: Enable/disable smoke heater
    gunfxServer.onSmokeHeat([](bool on) -> uint8_t {
        smokeGen.setHeater(on);
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
        smokeGen.configure(cfg);
        return SerialError::OK;
    });

    // SMOKE_RESET: Clear smoke error states
    gunfxServer.onSmokeReset([]() -> uint8_t {
        smokeGen.resetErrors();
        return SerialError::OK;
    });

    // SMOKE_CURRENT_LIMIT: Set overcurrent protection limit per channel
    gunfxServer.onSmokeCurrentLimit([](uint8_t channel, uint16_t limit_mA) -> uint8_t {
        smokeGen.setCurrentLimit(SmokeChannel(channel), limit_mA);
        return SerialError::OK;
    });

    // STATUS: Append GunFX module data to core STATUS response
    // Wire format (46 bytes):
    //   [flags:u8][fanSpeed:u8][fanOffMs:u16LE]
    //   [servo0:u16LE][servo1:u16LE][servo2:u16LE]
    //   [rpm:u16LE][shots:u32LE][heaterMs:u32LE]
    //   Heater INA226: [busV_mV:u16LE][current_mA:u16LE][power_mW:u16LE]
    //   Fan INA226:    [busV_mV:u16LE][current_mA:u16LE][power_mW:u16LE]
    //   [batteryV_mV:u16LE][inaFlags:u8][shuntR_mohm:u16LE]
    //   [ledFlags:u8][cellCount:u8][batteryPct:u8]
    //   [heaterError:u8][fanError:u8]
    //   [heaterDuty:u8][fanDuty:u8]
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 46) return 0;

        uint8_t flags = 0;
        if (muzzleFlash.isFiring())    flags |= 0x01;
        if (muzzleFlash.isFlashOn())   flags |= 0x02;
        if (muzzleFlash.isFading())    flags |= 0x04;
        if (smokeGen.isHeaterOn())     flags |= 0x08;
        if (smokeGen.isFanOn())        flags |= 0x10;
        if (smokeGen.isSpinningDown()) flags |= 0x20;

        buf[0] = flags;
        buf[1] = smokeGen.fanSpeed();
        CoreProtocol::putU16LE(&buf[2], smokeGen.spindownRemaining_ms());
        CoreProtocol::putU16LE(&buf[4], (uint16_t)constrain(gunServos[0].position(), 0, 3000));
        CoreProtocol::putU16LE(&buf[6], (uint16_t)constrain(gunServos[1].position(), 0, 3000));
        CoreProtocol::putU16LE(&buf[8], (uint16_t)constrain(gunServos[2].position(), 0, 3000));
        CoreProtocol::putU16LE(&buf[10], muzzleFlash.rpm());
        CoreProtocol::putU32LE(&buf[12], muzzleFlash.shotsFired());
        CoreProtocol::putU32LE(&buf[16], smokeGen.heaterOnTime_ms());

        // Heater INA226 (Motor 0 shunt) — bytes 20-25                       // mV, mA, mW
        if (ina226Available[0]) {
            CoreProtocol::putU16LE(&buf[20], (uint16_t)constrain(ina226[0].busVoltage_mV(), 0, 65535));
            CoreProtocol::putU16LE(&buf[22], (uint16_t)constrain(fabsf(ina226[0].current_mA()), 0, 65535));
            CoreProtocol::putU16LE(&buf[24], (uint16_t)constrain(ina226[0].power_mW(), 0, 65535));
        } else {
            memset(&buf[20], 0, 6);
        }

        // Fan INA226 (Motor 1 shunt) — bytes 26-31                          // mV, mA, mW
        if (ina226Available[1]) {
            CoreProtocol::putU16LE(&buf[26], (uint16_t)constrain(ina226[1].busVoltage_mV(), 0, 65535));
            CoreProtocol::putU16LE(&buf[28], (uint16_t)constrain(fabsf(ina226[1].current_mA()), 0, 65535));
            CoreProtocol::putU16LE(&buf[30], (uint16_t)constrain(ina226[1].power_mW(), 0, 65535));
        } else {
            memset(&buf[26], 0, 6);
        }

        // Battery voltage — bytes 32-33                                      // mV
        CoreProtocol::putU16LE(&buf[32], batteryMonitor.voltage_mV());

        // INA availability flags — byte 34 (bit 0=heater, bit 1=fan)
        uint8_t inaFlags = 0;
        if (ina226Available[0]) inaFlags |= 0x01;
        if (ina226Available[1]) inaFlags |= 0x02;
        buf[34] = inaFlags;

        // Shunt resistance — bytes 35-36                                     // mΩ
        CoreProtocol::putU16LE(&buf[35], (uint16_t)(SHUNT_RESISTANCE_OHMS * 1000.0f));

        // Status LED states — byte 37
        uint8_t ledFlags = 0;
        if (smokeGen.isHeaterOn())       ledFlags |= 0x01;  // Motor 0: heater
        if (smokeGen.isFanOn())          ledFlags |= 0x02;  // Motor 1: fan
        if (smokeGen.isSpinningDown())   ledFlags |= 0x04;  // Motor 1: spindown
        if (muzzleFlash.isFiring())      ledFlags |= 0x08;  // Motor 2: firing
        if (muzzleFlash.isFlashOn())     ledFlags |= 0x10;  // Motor 2: flash
        buf[37] = ledFlags;

        // Battery cell info — bytes 38-39
        buf[38] = batteryMonitor.cellCount();
        buf[39] = batteryMonitor.percentage();

        // Smoke error reasons — bytes 40-41 (SmokeErrorReason codes)
        buf[40] = smokeGen.errorReason(SmokeChannel::Heater);
        buf[41] = smokeGen.errorReason(SmokeChannel::Fan);

        // Overcurrent throttle state — bytes 42-43 (effective PWM duty, 255=no throttle)
        buf[42] = smokeGen.cappedDuty(SmokeChannel::Heater);
        buf[43] = smokeGen.cappedDuty(SmokeChannel::Fan);

        // Current limits — bytes 44-45 (mA, high byte only for compact encoding)
        // Full u16 limits reported via SMOKE_CURRENT_LIMIT response if needed

        return 44;
    });

    // I2C bus scan — handled by SfxServer (shared infrastructure)
    server.enableI2CScan(Wire);
    for (int i = 0; i < 2; i++) {
        server.addExpectedI2CDevice(INA226_ADDR[i], &ina226[i]);
    }

    // Finalize router (core handler + GunFX handler)
    server.addModuleHandler(&gunfxServer);
}

// ============================================================================
//  LOOP
// ============================================================================

void loop() {
    // Process protocol, connection timeout, indicators
    server.loop();

    // Update hardware modules
    muzzleFlash.update();
    smokeGen.update();

    // Update INA226 current monitors
    for (int i = 0; i < 2; i++) {
        if (ina226Available[i]) ina226[i].update();
    }

    // Update battery voltage monitor
    batteryMonitor.update();

    // Update servo motion profiling
    for (int i = 0; i < 3; i++) {
        gunServos[i].update();
    }

    // Update status LEDs (match motor channels)
    digitalWrite(PIN_LED_HEATER,   smokeGen.isHeaterOn()        ? HIGH : LOW);  // Motor 0
    digitalWrite(PIN_LED_FAN,      smokeGen.isFanOn()           ? HIGH : LOW);  // Motor 1
    digitalWrite(PIN_LED_SPINDOWN, smokeGen.isSpinningDown()    ? HIGH : LOW);  // Motor 1
    digitalWrite(PIN_LED_FIRING,   muzzleFlash.isFiring()       ? HIGH : LOW);  // Motor 2
    digitalWrite(PIN_LED_FLASH,    muzzleFlash.isFlashOn()      ? HIGH : LOW);  // Motor 2

    // Error indicator LED (GP14) — blink if any smoke channel has error
    server.indicators().setErrorCondition(smokeGen.hasError());

    busy_wait_ms(1);
}
