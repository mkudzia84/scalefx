/**
 * GunFX Pico Controller v0.7.0
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
 * GPIO Pin Mapping:
 *   GP0:     Trigger input — RC PWM capture (active in non-SLAVE modes).
 *            Pulse width reported in STATUS; threshold-to-fire mapping is
 *            left to the host until a standalone trigger policy is added.
 *   GP1:     Servo 1 (SRV1)
 *   GP2:     Servo 2 (SRV2)
 *   GP3:     Servo 3 (SRV3)
 *   GP13:    Indicator LED — connection (SfxServer, blue)
 *   GP14:    Indicator LED — error (SfxServer, yellow)
 *   GP16:    Smoke fan motor (PWM)
 *   GP17:    Smoke heater relay
 *   GP25:    Muzzle flash LED (PWM)
 *   GP29:    Battery voltage ADC (÷6 divider)
 */

#include <Arduino.h>
#include <serial/serial.h>
#include <gunfx/server/gunfx_server.h>
#include <servo/srv_control.h>
#include <server/sfx_server.h>
#include <power/battery_monitor.h>
#include "muzzle_flash.h"
#include "smoke_generator.h"

// Firmware version
#define FIRMWARE_VERSION "0.8.1"
#define BUILD_NUMBER 29

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

const uint8_t PIN_TRIGGER_INPUT =  0;   // RC PWM trigger input (capture only)
const uint8_t PIN_GUN_SRV_1     =  1;   // Gun servo 1 (SRV1)
const uint8_t PIN_GUN_SRV_2     =  2;   // Gun servo 2 (SRV2)
const uint8_t PIN_GUN_SRV_3     =  3;   // Gun servo 3 (SRV3)
const uint8_t PIN_SMOKE_FAN     = 16;   // Smoke fan motor (PWM)
const uint8_t PIN_SMOKE_HEATER  = 17;   // Smoke heater relay
const uint8_t PIN_NOZZLE_FLASH  = 25;   // Muzzle flash LED (PWM)
const uint8_t PIN_VSENSE        = 29;   // Battery voltage ADC (÷6 divider)

// ============================================================================
//  CONSTANTS
// ============================================================================

// Servo defaults
const uint16_t SERVO_DEFAULT_US    = 1500;    // Center position


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

// Battery voltage monitor (ADC ÷6 divider, e.g. 50k/10k)
AdcDividerBatteryT<6000> batteryMonitor;

// ----------------------------------------------------------------------------
// GP0 RC PWM trigger input — edge ISR captures pulse width. No threshold
// action is wired yet; the host reads the captured µs from STATUS and can
// decide whether to fire (mirrors the GearControl gear_input capture pattern
// without the auto deploy/retract dispatch).
// ----------------------------------------------------------------------------
volatile uint32_t triggerInputRiseUs   = 0;
volatile uint16_t triggerInputPulse_us = 0;   // 0 == no pulse seen yet
volatile uint32_t triggerInputEdgeMs   = 0;   // for stale-pulse detection

static void triggerInputIsr() {
    const uint32_t now = micros();
    if (digitalRead(PIN_TRIGGER_INPUT)) {
        triggerInputRiseUs = now;
    } else if (triggerInputRiseUs != 0) {
        const uint32_t pulse = now - triggerInputRiseUs;
        if (pulse >= 500 && pulse <= 2500) {
            triggerInputPulse_us = (uint16_t)pulse;
            triggerInputEdgeMs   = millis();
        }
    }
}

static uint16_t triggerInputCurrent_us() {
    noInterrupts();
    const uint16_t pulse = triggerInputPulse_us;
    const uint32_t edge  = triggerInputEdgeMs;
    interrupts();
    if (millis() - edge > 200) return 0;   // stale → no signal
    return pulse;
}

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
    server.onInit([](uint8_t mode, uint8_t flags) {
        (void)flags;  // GunFX accepts both SLAVE and DIRECT
        SFX_LOG_INFO("INIT mode=%s", InitMode::getName(mode));
        performSafeInit();
    });
    server.onShutdown([]() { performSafeShutdown(); });

    // Initialize battery voltage monitor (ADC ÷6 divider)
    analogReadResolution(12);
    batteryMonitor.begin(PIN_VSENSE);

    // Arm the GP0 RC PWM trigger input. Pulled low at rest so a disconnected
    // input reads as 0. Edge interrupt runs in ISR context — keep it short.
    pinMode(PIN_TRIGGER_INPUT, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_TRIGGER_INPUT),
                    triggerInputIsr, CHANGE);

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
        gunServos[idx].setReversed(cfg.reversed);
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

    // STATUS: Append GunFX module data to core STATUS response.
    // Wire format (69 bytes — 28 base + 39 servo config tail + 2 trigger input):
    //   byte 0      [flags:u8]
    //   byte 1      [fanSpeed:u8]
    //   byte 2..3   [fanOffMs:u16LE]
    //   byte 4..9   [servo0:u16LE][servo1:u16LE][servo2:u16LE]
    //   byte 10..11 [rpm:u16LE]
    //   byte 12..15 [shots:u32LE]
    //   byte 16..19 [heaterMs:u32LE]
    //   byte 20..21 [heaterError:u8][fanError:u8]
    //   byte 22..23 [heaterDuty:u8][fanDuty:u8]
    //   byte 24..27 [batteryV_mV:u16LE][cellCount:u8][batteryPct:u8]
    //   byte 28..66 [servoConfig:13×3]  min:u16 max:u16 target:u16
    //                                   speed:u16 accel:u16 decel:u16 rev:u8
    //   byte 67..68 [triggerInput_us:u16LE]  GP0 RC pulse (v0.8.1, Rule 11)
    // Legacy decoders that stop at byte 28 or 67 see valid earlier prefixes.
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 28) return 0;
        const bool haveRoomForCfg     = (maxLen >= 67);
        const bool haveRoomForTrigger = (maxLen >= 69);

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

        // Smoke error reasons — bytes 20-21 (SmokeErrorReason codes)
        buf[20] = smokeGen.errorReason(SmokeChannel::Heater);
        buf[21] = smokeGen.errorReason(SmokeChannel::Fan);

        // Overcurrent throttle state — bytes 22-23 (effective PWM duty, 255=no throttle)
        buf[22] = smokeGen.cappedDuty(SmokeChannel::Heater);
        buf[23] = smokeGen.cappedDuty(SmokeChannel::Fan);

        // Battery — bytes 24-27                                              // mV
        CoreProtocol::putU16LE(&buf[24], batteryMonitor.voltage_mV());
        buf[26] = batteryMonitor.cellCount();
        buf[27] = batteryMonitor.percentage();

        // Per-servo configuration block (Rule 11 extension, 13×3 = 39 bytes).
        // Emitted when the core has room; legacy hubs / tight buffers drop the
        // tail and old decoders treat the payload as a 28-byte prefix.
        if (haveRoomForCfg) {
            size_t off = 28;
            for (uint8_t i = 0; i < 3; i++) {
                const auto& s = gunServos[i];
                CoreProtocol::putU16LE(&buf[off + 0],  (uint16_t)s.minLimit());
                CoreProtocol::putU16LE(&buf[off + 2],  (uint16_t)s.maxLimit());
                CoreProtocol::putU16LE(&buf[off + 4],  (uint16_t)s.target());
                CoreProtocol::putU16LE(&buf[off + 6],  (uint16_t)s.maxSpeed());
                CoreProtocol::putU16LE(&buf[off + 8],  (uint16_t)s.acceleration());
                CoreProtocol::putU16LE(&buf[off + 10], (uint16_t)s.deceleration());
                buf[off + 12] = s.isReversed() ? 1 : 0;
                off += 13;
            }
            if (haveRoomForTrigger) {
                CoreProtocol::putU16LE(&buf[67], triggerInputCurrent_us());
                return 69;
            }
            return 67;
        }
        return 28;
    });

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

    // Update battery voltage monitor
    batteryMonitor.update();

    // Update servo motion profiling
    for (int i = 0; i < 3; i++) {
        gunServos[i].update();
    }

    // Error indicator LED (GP14) — blink if any smoke channel has error
    server.indicators().setErrorCondition(smokeGen.hasError());

    busy_wait_ms(1);
}
