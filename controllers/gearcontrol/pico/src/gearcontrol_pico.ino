/**
 * GearControl Pico Controller v0.6.0
 *
 * Server controller for landing gear effects - receives commands from HubFX over USB serial.
 * Controls: 3 landing gear units (via LandingGear module), yaw steering servo,
 *           2 indicator LEDs (bi-color), battery voltage sense (ADC).
 *
 * Hardware: Raspberry Pi Pico (RP2040) + earlephilhower/arduino-pico core
 * Protocol: Binary COBS with CRC-8
 *
 * Architecture (Chain of Responsibility):
 *   - SfxServer: Common server boilerplate (serial, indicators, core protocol)
 *   - GearControlServer: Handles GEAR, SERVO, YAW commands
 *   - CommandRouter: Routes packets to handlers in priority order
 *
 * Landing Gear Module (per gear):
 *   - 2× ServoControl for door servos (motion profiled)
 *   - 2× LedControl for status LEDs (CW/CCW)
 *   - Motor H-bridge (CW/CCW GPIO pair)
 *   - INA226 current monitoring (stall detection)
 *
 * GPIO Pin Mapping:
 *   GP1-3:   Servos 1-3 (door servos)
 *   GP4-5:   I2C SDA/SCL (INA226 monitors)
 *   GP6-9:   Servos 4-7 (door/yaw)
 *   GP13-14: Indicator LEDs (bi-color RED/GREEN)
 *   GP15-20: Motor CW/CCW pairs (3 motors × 2)
 *   GP21-26: Status LEDs (CW/CCW per motor × 3)
 *   GP29:    Voltage sense (ADC, ÷6 divider)
 *
 * Servo Mapping:
 *   0-1: Gear 0 door servos (nose)       [GP1-2]
 *   2-3: Gear 1 door servos (left main)  [GP3,GP6]
 *   4-5: Gear 2 door servos (right main) [GP7-8]
 *   6:   Yaw servo (steering)            [GP9]
 *
 * Status LED Mapping:
 *   0-1: Motor 0 CW/CCW (deploy/retract)  [GP21-22]
 *   2-3: Motor 1 CW/CCW (deploy/retract)  [GP23-24]
 *   4-5: Motor 2 CW/CCW (deploy/retract)  [GP25-26]
 *
 * Indicator LED Mapping:
 *   0: Connection status (blink=waiting, solid=connected, off=lost)  [GP13]
 *   1: Error status (off=normal, blink=error/abnormal)               [GP14]
 */

#include <Arduino.h>
#include <Wire.h>
#include <serial/serial.h>
#include <gearcontrol/server/gearcontrol_server.h>
#include <led/led_control.h>
#include <servo/srv_control.h>
#include <power/ina226.h>
#include <power/i2c_device.h>
#include <power/battery_monitor.h>
#include <server/sfx_server.h>
#include "landing_gear.h"

// Firmware version
#define FIRMWARE_VERSION "0.10.0"
#define BUILD_NUMBER 56

// ============================================================================
//  PIN CONFIGURATION
// ============================================================================

// Door servo pins (2 per gear × 3 gears = 6)
const uint8_t PIN_DOOR_SERVO[3][2] = {
    { 1, 2 },    // Gear 0 (nose): GP1, GP2
    { 3, 6 },    // Gear 1 (left main): GP3, GP6
    { 7, 8 },    // Gear 2 (right main): GP7, GP8
};

// Yaw servo pin
const uint8_t PIN_YAW_SERVO = 9;

// I2C pins for INA226
const uint8_t PIN_SDA = 4;
const uint8_t PIN_SCL = 5;

// Motor H-bridge pins (CW/CCW per motor)
const uint8_t PIN_MOTOR[3][2] = {
    { 15, 16 },  // Motor 0: CW=GP15, CCW=GP16
    { 17, 18 },  // Motor 1: CW=GP17, CCW=GP18
    { 19, 20 },  // Motor 2: CW=GP19, CCW=GP20
};

// Status LED pins (2 per motor × 3 = 6)
const uint8_t PIN_STATUS_LED[3][2] = {
    { 21, 22 },  // Motor 0: CW=GP21, CCW=GP22
    { 23, 24 },  // Motor 1: CW=GP23, CCW=GP24
    { 25, 26 },  // Motor 2: CW=GP25, CCW=GP26
};

// Voltage sense (ADC with ÷6 divider, 12-bit)
const uint8_t PIN_VSENSE = 29;

// ============================================================================
//  CONSTANTS
// ============================================================================

// INA226 shunt resistance (depends on hardware)
const float SHUNT_RESISTANCE_OHMS = 0.005f;    // 5mΩ shunt (max ≈16.4A with INA226)
const float MAX_CURRENT_A = 10.0f;             // 10A max expected

// Servo ID mapping: IDs 0-5 = door servos, 6 = yaw, 7 = spare
const uint8_t SERVO_ID_YAW   = 6;
const uint8_t SERVO_ID_SPARE = 7;

// ============================================================================
//  GLOBAL INSTANCES
// ============================================================================

// Server (serial, core protocol, indicators, connection management)
SfxServer server;
GearControlServer gearControlServer;

// Landing gear modules (one per gear)
LandingGear gears[3];

// INA226 current monitors (one per motor)
INA226 ina226[3];
bool ina226Available[3] = { false, false, false };

// Yaw servo (uses ServoControl for configurability)
ServoControl yawServo;

// Battery voltage monitor (ADC with ÷6 divider)
BatteryMonitor batteryMonitor;

// Yaw configuration
GearControlYawConfig yawConfig;
bool yawConfigured = false;

// Battery auto-deploy configuration
bool batteryEnabled = false;               // Battery monitoring disabled until host enables via BATTERY_CONFIG
bool autoDeployOnLowVoltage = false;
bool lowVoltageTriggered = false;  // Set when auto-deploy fires (persists until reset)

// Per-gear error reason tracking (for STATUS diagnostic reporting)
uint8_t gearErrorReason[3] = { GearErrorReason::NONE, GearErrorReason::NONE, GearErrorReason::NONE };

// Expected I2C addresses for the 3 INA226 monitors
// Binary:  1000000, 1000100, 1000001 → 0x40, 0x44, 0x41
const uint8_t INA226_ADDR[3] = {
    INA226Address::GND_GND,   // 0x40 - Motor 0 (nose)
    INA226Address::VS_GND,    // 0x44 - Motor 1 (left main)
    INA226Address::GND_VS,    // 0x41 - Motor 2 (right main)
};

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================

void performSafeShutdown();
void performSafeInit();
uint8_t buildLedFlags();

// ============================================================================
//  CONNECTION MANAGEMENT
// ============================================================================

void performSafeShutdown() {
    SFX_LOG_INFO("Shutdown — motors off, servos center, LEDs off");

    // Shutdown all gear modules (stops motors, returns servos to center, LEDs off)
    for (int i = 0; i < 3; i++) {
        gears[i].shutdown();
    }

    // Return yaw to center
    yawServo.setPositionImmediate(1500);

    // Disable battery monitoring and auto-deploy (requires re-configuration after reconnect)
    batteryEnabled = false;
    autoDeployOnLowVoltage = false;
    lowVoltageTriggered = false;
}

void performSafeInit() {
    SFX_LOG_INFO("Init — reset gears, check monitors");

    // Reset all gear modules
    for (int i = 0; i < 3; i++) {
        gears[i].reset();
        // Re-flag monitor fault if INA226 was unavailable at boot
        if (!ina226Available[i]) {
            gears[i].flagMonitorFault();
            gearErrorReason[i] = GearErrorReason::MONITOR_FAULT;
            SFX_LOG_WARN("Gear %d: INA226 monitor fault (0x%02X)", i, INA226_ADDR[i]);
        } else {
            gearErrorReason[i] = GearErrorReason::NONE;
        }
    }

    lowVoltageTriggered = false;
}

// ============================================================================
//  STATUS BUILDING
// ============================================================================

/**
 * @brief Build LED flags byte from current LED states
 */
uint8_t buildLedFlags() {
    uint8_t flags = 0;
    // Bits 0-5: per-gear status LEDs (2 per gear)
    // Derived from gear state since LEDs are managed by LandingGear module
    for (int i = 0; i < 3; i++) {
        uint8_t deployBit = i * 2;
        uint8_t retractBit = i * 2 + 1;
        GearState state = gears[i].state();

        if (state == GearState::DEPLOYED || state == GearState::DEPLOYING) {
            flags |= (1 << deployBit);
        }
        if (state == GearState::RETRACTED || state == GearState::RETRACTING) {
            flags |= (1 << retractBit);
        }
        if (state == GearState::ERROR || state == GearState::CALIBRATING) {
            flags |= (1 << deployBit) | (1 << retractBit);
        }
    }
    // Bits 6-7: indicator LEDs (connection/error)
    if (server.indicators().connectionLed().isOn()) flags |= (1 << 6);
    if (server.indicators().errorLed().isOn()) flags |= (1 << 7);
    return flags;
}



// ============================================================================
//  SETUP
// ============================================================================

void setup() {
    // Initialize server (serial, device name, indicators, core callbacks)
    server.begin("GearControl", FIRMWARE_VERSION, BUILD_NUMBER);
    server.onInit([]() { performSafeInit(); });
    server.onShutdown([]() { performSafeShutdown(); });

    // Initialize I2C for INA226
    Wire.setSDA(PIN_SDA);
    Wire.setSCL(PIN_SCL);
    Wire.begin();
    Wire.setClock(400000);  // 400kHz fast mode

    // Initialize INA226 monitors
    for (int i = 0; i < 3; i++) {
        INA226Config cfg;
        cfg.address = INA226_ADDR[i];
        cfg.shuntResistance_ohms = SHUNT_RESISTANCE_OHMS;
        cfg.maxCurrent_A = MAX_CURRENT_A;
        ina226Available[i] = ina226[i].begin(Wire, cfg);
        SFX_LOG_INFO("INA226[%d] (0x%02X): %s", i, INA226_ADDR[i],
                     ina226Available[i] ? "OK" : "NOT FOUND");
    }

    // Initialize landing gear modules
    for (int i = 0; i < 3; i++) {
        gears[i].begin(i, PIN_MOTOR[i][0], PIN_MOTOR[i][1]);

        // Attach door servos
        gears[i].attachDoorServo(0, PIN_DOOR_SERVO[i][0]);
        gears[i].attachDoorServo(1, PIN_DOOR_SERVO[i][1]);

        // Attach status LEDs
        gears[i].attachStatusLed(0, PIN_STATUS_LED[i][0]);
        gears[i].attachStatusLed(1, PIN_STATUS_LED[i][1]);

        // Attach current monitor (always — INA226 is soldered on board)
        gears[i].attachCurrentMonitor(&ina226[i]);
        if (!ina226Available[i]) {
            // INA226 failed I2C init — board fault
            gears[i].flagMonitorFault();
            gearErrorReason[i] = GearErrorReason::MONITOR_FAULT;
        }

        // Register calibration progress callback (emits GEAR_CALIB_STATUS packets)
        gears[i].onCalibrationProgress([](const GearControlCalibStatus& status) {
            gearControlServer.sendCalibStatus(status);
        });

        // Register sequence progress callback (emits GEAR_SEQ_STATUS packets)
        gears[i].onSequenceProgress([](const GearControlSeqStatus& status) {
            gearControlServer.sendGearSeqStatus(status);
        });

        // Register door status callback (emits GEAR_DOOR_STATUS packets)
        gears[i].onDoorStatus([](const GearControlDoorStatus& status) {
            gearControlServer.sendDoorStatus(status);
        });
    }

    // Initialize yaw servo (uses ServoControl for configurability)
    yawServo.begin(PIN_YAW_SERVO);

    // Initialize battery voltage monitor (ADC with ÷6 divider on GP29)
    analogReadResolution(12);
    batteryMonitor.begin(PIN_VSENSE, 6.0f);

    // Auto-deploy all gears on low battery voltage (safety feature)
    batteryMonitor.onLowVoltage([](uint16_t voltage_mV, uint8_t cellCount) {
        if (batteryEnabled && autoDeployOnLowVoltage && server.indicators().isConnected()) {
            SFX_LOG_WARN("LOW BATTERY %u mV (%uS) — emergency deploy all gears",
                         voltage_mV, cellCount);
            lowVoltageTriggered = true;
            for (int i = 0; i < 3; i++) {
                gears[i].markEmergencyDeploy();
                gears[i].deploy();
            }
        }
    });

    // ========================================================================
    // Initialize GearControlServer (GearControl-specific commands)
    // ========================================================================
    gearControlServer.begin(&Serial);

    // GEAR_DEPLOY: Deploy landing gear (individual, no sync)
    gearControlServer.onGearDeploy([](uint8_t gearId) -> uint8_t {
        gears[gearId].setSyncMode(false);
        return gears[gearId].deploy();
    });

    // GEAR_RETRACT: Retract landing gear (individual, no sync)
    gearControlServer.onGearRetract([](uint8_t gearId) -> uint8_t {
        gears[gearId].setSyncMode(false);
        // If yaw is configured for this gear, center yaw before retract
        if (yawConfigured && yawConfig.gearId == gearId) {
            yawServo.setTarget(yawConfig.neutral_us);
        }
        return gears[gearId].retract();
    });

    // GEAR_STOP: Emergency stop
    gearControlServer.onGearStop([](uint8_t gearId) -> uint8_t {
        if (gearId >= 3) return GearControlError::INVALID_GEAR_ID;
        gears[gearId].stop();
        return SerialError::OK;
    });

    // GEAR_ALL: Deploy/retract/stop all gears (synchronized)
    gearControlServer.onGearAll([](uint8_t action) -> uint8_t {
        // Enable sync mode for deploy/retract so gears wait at phase barriers
        if (action == GearControlSpec::ACTION_DEPLOY || action == GearControlSpec::ACTION_RETRACT) {
            for (int i = 0; i < 3; i++) gears[i].setSyncMode(true);
        }

        uint8_t result = SerialError::OK;
        for (int i = 0; i < 3; i++) {
            uint8_t r;
            if (action == GearControlSpec::ACTION_DEPLOY) {
                r = gears[i].deploy();
            } else if (action == GearControlSpec::ACTION_RETRACT) {
                if (yawConfigured && yawConfig.gearId == (uint8_t)i) {
                    yawServo.setTarget(yawConfig.neutral_us);
                }
                r = gears[i].retract();
            } else {
                gears[i].stop();
                r = SerialError::OK;
            }
            if (r != SerialError::OK && result == SerialError::OK) {
                result = r;  // Return first error
            }
        }
        return result;
    });

    // SERVO_SET: Direct servo control
    gearControlServer.onServoSet([](uint8_t servoId, uint16_t pulse_us) -> uint8_t {
        if (servoId <= 5) {
            // Door servo: servoId 0-5 → gear index = servoId / 2, door index = servoId % 2
            uint8_t gearIdx = servoId / 2;
            uint8_t doorIdx = servoId % 2;
            gears[gearIdx].setDoorPosition(doorIdx, pulse_us);
        } else if (servoId == SERVO_ID_YAW) {
            yawServo.setTarget(pulse_us);
        } else {
            // Spare servo (ID 7) - not currently implemented
            return GearControlError::INVALID_SERVO_ID;
        }
        return SerialError::OK;
    });

    // SRV_SETTINGS: Configure servo parameters (GunFX/LightFX pattern)
    gearControlServer.onServoSettings([](const GearControlServoConfig& cfg) -> uint8_t {
        if (cfg.servoId <= 5) {
            // Door servo: apply limits and motion profile via LandingGear module
            uint8_t gearIdx = cfg.servoId / 2;
            uint8_t doorIdx = cfg.servoId % 2;
            gears[gearIdx].configureDoorServo(doorIdx,
                cfg.minUs, cfg.maxUs,
                cfg.maxSpeedUsPerSec, cfg.maxAccelUsPerSec2, cfg.maxDecelUsPerSec2);
        } else if (cfg.servoId == SERVO_ID_YAW) {
            // Yaw servo: apply limits and motion profile
            yawServo.setLimits(cfg.minUs, cfg.maxUs);
            yawServo.setMotionProfile(cfg.maxSpeedUsPerSec,
                                       cfg.maxAccelUsPerSec2,
                                       cfg.maxDecelUsPerSec2);
        } else {
            return GearControlError::INVALID_SERVO_ID;
        }
        return SerialError::OK;
    });

    // GEAR_CONFIG: Configure gear behavior
    gearControlServer.onGearConfig([](const GearControlGearConfig& cfg) -> uint8_t {
        gears[cfg.gearId].setGearConfig(cfg);
        return SerialError::OK;
    });

    // DOOR_CONFIG: Configure door servo positions
    gearControlServer.onDoorConfig([](const GearControlDoorConfig& cfg) -> uint8_t {
        gears[cfg.gearId].setDoorConfig(cfg);
        return SerialError::OK;
    });

    // YAW_CONFIG: Configure yaw servo
    gearControlServer.onYawConfig([](const GearControlYawConfig& cfg) -> uint8_t {
        yawConfig = cfg;
        yawConfigured = true;
        // Set to neutral initially
        yawServo.setTarget(cfg.neutral_us);
        return SerialError::OK;
    });

    // YAW_INPUT: Set yaw position (only effective when associated gear is deployed)
    gearControlServer.onYawInput([](uint16_t position_us) -> uint8_t {
        if (!yawConfigured) return GearControlError::YAW_NOT_AVAILABLE;

        // Only apply yaw when associated gear is deployed
        if (!gears[yawConfig.gearId].isDeployed()) {
            return SerialError::OK;  // Silently ignore (not an error)
        }

        // Clamp to configured range
        uint16_t clamped = constrain(position_us, yawConfig.min_us, yawConfig.max_us);
        yawServo.setTarget(clamped);
        return SerialError::OK;
    });

    // GEAR_CALIBRATE: Start stall current calibration (optional timeout in seconds)
    gearControlServer.onGearCalibrate([](uint8_t gearId, uint8_t timeout_s) -> uint8_t {
        uint32_t timeout_ms = (uint32_t)timeout_s * 1000;
        return gears[gearId].calibrate(timeout_ms);
    });

    // GEAR_CALIB_CANCEL: Cancel calibration in progress
    gearControlServer.onGearCalibCancel([](uint8_t gearId) -> uint8_t {
        return gears[gearId].cancelCalibration();
    });

    // BATTERY_CONFIG: Enable/disable battery monitoring and auto-deploy on low voltage
    gearControlServer.onBatteryConfig([](bool enabled, bool autoDeploy) -> uint8_t {
        batteryEnabled = enabled;
        autoDeployOnLowVoltage = autoDeploy;
        if (!enabled) {
            lowVoltageTriggered = false;  // Reset low-voltage state when disabling
        }
        return SerialError::OK;
    });

    // DOOR_MODE: Configure door activation modes per gear
    gearControlServer.onDoorMode([](const GearControlDoorModeConfig& cfg) -> uint8_t {
        gears[cfg.gearId].setDoorMode(cfg.preDeployMode, cfg.postDeployMode, cfg.delay_ms);
        return SerialError::OK;
    });

    // GEAR_RESET: Clear error state (ERROR → UNKNOWN)
    gearControlServer.onGearReset([](uint8_t gearId) -> uint8_t {
        gears[gearId].clearError();
        gearErrorReason[gearId] = GearErrorReason::NONE;
        return SerialError::OK;
    });

    // GEAR_ENABLE: Enable/disable gear channel
    gearControlServer.onGearEnable([](uint8_t gearId, bool enabled) -> uint8_t {
        gears[gearId].setEnabled(enabled);
        return SerialError::OK;
    });

    // I2C bus scan — handled by SfxServer (shared infrastructure)
    server.enableI2CScan(Wire);
    for (int i = 0; i < 3; i++) {
        server.addExpectedI2CDevice(INA226_ADDR[i], &ina226[i]);
    }

    // STATUS: Append GearControl module data to core STATUS response
    // Wire format (53 bytes):
    //   Per gear (3 × 11 = 33 bytes):
    //     [state:u8][motorCurrent_mA:u16LE][door0Pos_us:u16LE][door1Pos_us:u16LE]
    //     [calibratedStall_mA:u16LE][shuntVoltage_10uV:i16LE]
    //   Yaw + LEDs + Voltage (6 bytes):
    //     [yawPos_us:u16LE][ledFlags:u8][batteryVoltage_mV:u16LE][batteryConfigFlags:u8]
    //   Per-gear error reasons (3 bytes):
    //     [gear0ErrorReason:u8][gear1ErrorReason:u8][gear2ErrorReason:u8]
    //   Shunt config (2 bytes):
    //     [shuntResistance_mohm:u16LE]
    //   Per-gear packed door modes (3 bytes):
    //     [gear0DoorModes:u8][gear1DoorModes:u8][gear2DoorModes:u8]
    //     Each byte: low nibble = doorPreDeploy mode
    //                high nibble = doorPostDeploy mode
    //   Per-gear config flags (3 bytes):
    //     [gear0ConfigFlags:u8][gear1ConfigFlags:u8][gear2ConfigFlags:u8]
    //   Per-gear door state (3 bytes):
    //     [gear0DoorState:u8][gear1DoorState:u8][gear2DoorState:u8]
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
        if (maxLen < 53) return 0;

        for (int i = 0; i < 3; i++) {
            size_t off = i * 11;
            buf[off] = static_cast<uint8_t>(gears[i].state());
            CoreProtocol::putU16LE(&buf[off + 1], gears[i].readMotorCurrent_mA());  // mA
            CoreProtocol::putU16LE(&buf[off + 3], gears[i].doorPosition_us(0));  // door0  // µs
            CoreProtocol::putU16LE(&buf[off + 5], gears[i].doorPosition_us(1));  // door1  // µs
            CoreProtocol::putU16LE(&buf[off + 7], gears[i].calibratedStall_mA()); // mA
            // Shunt voltage in 10µV units for diagnostic (INA226 shuntVoltage_uV / 10)
            int16_t shuntV = ina226Available[i]
                ? (int16_t)(ina226[i].shuntVoltage_uV() / 10.0f)
                : 0;
            CoreProtocol::putU16LE(&buf[off + 9], (uint16_t)shuntV);  // 10µV
        }

        CoreProtocol::putU16LE(&buf[33], (uint16_t)yawServo.position());  // yaw servo  // µs
        buf[35] = buildLedFlags();
        CoreProtocol::putU16LE(&buf[36], batteryEnabled ? batteryMonitor.voltage_mV() : (uint16_t)0);  // mV
        // Battery config flags: bit 0 = auto-deploy enabled, bit 1 = low voltage triggered, bit 2 = battery monitoring enabled
        buf[38] = (autoDeployOnLowVoltage ? 0x01 : 0x00)
                | (lowVoltageTriggered    ? 0x02 : 0x00)
                | (batteryEnabled         ? 0x04 : 0x00);

        // Per-gear error reasons (diagnostic — explains WHY a gear is in ERROR state)
        buf[39] = gearErrorReason[0];
        buf[40] = gearErrorReason[1];
        buf[41] = gearErrorReason[2];

        // Configured shunt resistance in milliohms (e.g., 100 = 100mΩ = 0.1Ω)
        CoreProtocol::putU16LE(&buf[42], (uint16_t)(SHUNT_RESISTANCE_OHMS * 1000.0f));  // mΩ

        // Per-gear packed door modes: low nibble = doorPreDeploy, high nibble = doorPostDeploy
        buf[44] = (gears[0].doorPreDeploy() & 0x0F) | ((gears[0].doorPostDeploy() & 0x0F) << 4);
        buf[45] = (gears[1].doorPreDeploy() & 0x0F) | ((gears[1].doorPostDeploy() & 0x0F) << 4);
        buf[46] = (gears[2].doorPreDeploy() & 0x0F) | ((gears[2].doorPostDeploy() & 0x0F) << 4);

        // Per-gear config flags (GearConfigFlags bitmask + runtime ENABLED bit 7)
        buf[47] = gears[0].gearConfig().flags | (gears[0].isEnabled() ? GearConfigFlags::ENABLED : 0);
        buf[48] = gears[1].gearConfig().flags | (gears[1].isEnabled() ? GearConfigFlags::ENABLED : 0);
        buf[49] = gears[2].gearConfig().flags | (gears[2].isEnabled() ? GearConfigFlags::ENABLED : 0);

        // Per-gear door state (DoorState values from door sequencer)
        buf[50] = gears[0].doorState();
        buf[51] = gears[1].doorState();
        buf[52] = gears[2].doorState();

        return 53;
    });

    // Finalize command router (core + GearControl handlers)
    server.addModuleHandler(&gearControlServer);
}

// ============================================================================
//  LOOP
// ============================================================================

void loop() {
    // Process protocol, connection timeout, indicators
    server.loop();

    // Update INA226 current monitors (refresh cached readings for stall detection)
    for (int i = 0; i < 3; i++) {
        if (ina226Available[i]) ina226[i].update();
    }

    // Update all landing gear modules (sequencing, door servos, status LEDs)
    for (int i = 0; i < 3; i++) {
        gears[i].update();

        // Propagate calibration error reason (e.g., motor disconnected)
        if (gears[i].state() == GearState::ERROR && gears[i].lastCalibErrorReason() != 0) {
            if (gearErrorReason[i] != gears[i].lastCalibErrorReason()) {
                gearErrorReason[i] = gears[i].lastCalibErrorReason();
                SFX_LOG_ERROR("Gear %d ERROR: reason=0x%02X", i, gearErrorReason[i]);
            }
        }
    }

    // Sync coordinator: advance gears past sync barriers when all are ready
    {
        // Barrier 1: doors-open → motor start
        bool anyWaitingDoors = false;
        bool anyStillOpening = false;
        for (int i = 0; i < 3; i++) {
            if (gears[i].isWaitingSyncDoorsOpen()) anyWaitingDoors = true;
            if (gears[i].isSyncOpeningDoors()) anyStillOpening = true;
        }
        if (anyWaitingDoors && !anyStillOpening) {
            for (int i = 0; i < 3; i++) {
                if (gears[i].isWaitingSyncDoorsOpen()) gears[i].advanceSyncPhase();
            }
        }

        // Barrier 2: motor-done → door close
        bool anyWaitingMotor = false;
        bool anyStillRunning = false;
        for (int i = 0; i < 3; i++) {
            if (gears[i].isWaitingSyncMotorDone()) anyWaitingMotor = true;
            if (gears[i].isSyncRunningMotor()) anyStillRunning = true;
        }
        if (anyWaitingMotor && !anyStillRunning) {
            for (int i = 0; i < 3; i++) {
                if (gears[i].isWaitingSyncMotorDone()) gears[i].advanceSyncPhase();
            }
        }
    }

    // Update yaw servo motion profiling
    yawServo.update();

    // Update indicator LED conditions (error/warning)
    bool anyError = false;
    for (int i = 0; i < 3; i++) {
        if (gears[i].isError()) { anyError = true; break; }
    }
    server.indicators().setErrorCondition(anyError);
    server.indicators().setWarningCondition(lowVoltageTriggered);

    // Update battery voltage monitor (only when enabled via BATTERY_CONFIG)
    if (batteryEnabled) {
        batteryMonitor.update();
    }

    busy_wait_ms(1);
}
