/*
 * Serial GearControl Protocol - Binary Protocol Client/Server
 *
 * Binary COBS protocol client/server for GearControl landing gear controller.
 *   - GearControlClient: For HubFX (sends commands via USB)
 *   - GearControlServer: For GearControl Pico (receives commands, implements ICommandHandler)
 *
 * Hardware:
 *   - 3 motors (H-bridge) for landing gear extend/retract
 *   - 3 INA226 shunt monitors (one per motor) for stall detection
 *   - 8 servos (2 door servos per gear × 3 + 1 yaw + 1 spare)
 *   - 6 status LEDs (2 per motor: CW/CCW indicators)
 *   - 2 indicator LEDs (bi-color RED/GREEN)
 *   - Battery voltage sense (ADC with ÷6 divider)
 *
 * Packet Types (0x60-0x7F range):
 *   GEAR_DEPLOY  (0x60)  - [gear_id:u8] Deploy gear (open doors → extend → opt close doors)
 *   GEAR_RETRACT (0x61)  - [gear_id:u8] Retract gear (open doors → retract → close doors)
 *   GEAR_STOP    (0x62)  - [gear_id:u8] Emergency stop motor
 *   GEAR_ALL     (0x63)  - [action:u8] Deploy(1)/Retract(0)/Stop(2) all gears
 *   SERVO_SET    (0x64)  - [id:u8][pulseUs:u16] Set servo position
 *   SRV_SETTINGS (0x65)  - [id:u8][min:u16][max:u16][speed:u16][accel:u16][decel:u16]
 *   GEAR_CONFIG  (0x66)  - [gear_id:u8][flags:u8][stall_mA:u16][timeout_ms:u16]
 *   DOOR_CONFIG  (0x67)  - [gear_id:u8][open0:u16][close0:u16][open1:u16][close1:u16]
 *   YAW_CONFIG   (0x68)  - [gear_id:u8][neutral:u16][min:u16][max:u16]
 *   YAW_INPUT    (0x69)  - [position_us:u16] Raw yaw steering signal
 *   GEAR_CALIBRATE (0x6A) - [gear_id:u8][timeout_s:u8(opt)] Start stall current calibration (default 60s timeout)
 *   GEAR_CALIB_STATUS (0x6B) - [gear_id:u8][phase:u8][current:u16][peak:u16][stall:u16][finished:u8][errorReason:u8] Calibration progress (server→client)
 *   GEAR_CALIB_CANCEL (0x6C) - [gear_id:u8] Cancel calibration in progress
 *   BATTERY_CONFIG  (0x6D)  - [enabled:u8][auto_deploy:u8] Configure battery monitoring (enable/disable + auto-deploy)
 *   DOOR_MODE      (0x6E)  - [gear_id:u8][pre_deploy:u8][post_deploy:u8][delay_ms:u16LE] Configure door modes
 *   GEAR_RESET     (0x6F)  - [gear_id:u8] Clear error state (ERROR → UNKNOWN)
 *   GEAR_SEQ_STATUS(0x70)  - Server→client sequence progress (async)
 *   GEAR_ENABLE    (0x71)  - [gear_id:u8][enabled:u8] Enable/disable gear channel
 *   GEAR_DOOR_STATUS(0x72) - Server→client door state transition (async)
 *   (I2C_SCAN moved to CorePacket — handled by SfxServer)
 */

#ifndef SERIAL_GEARCONTROL_H
#define SERIAL_GEARCONTROL_H

#include <Arduino.h>
#include <functional>
#include "serial_bus_client.h"
#include "serial_bus_server.h"

// ============================================================================
// GearControl Binary Packet Types (0x60-0x7F range)
// ============================================================================

namespace GearControlPacket {
    // Gear control
    constexpr uint8_t GEAR_DEPLOY    = 0x60;  // [gear_id:u8]
    constexpr uint8_t GEAR_RETRACT   = 0x61;  // [gear_id:u8]
    constexpr uint8_t GEAR_STOP      = 0x62;  // [gear_id:u8]
    constexpr uint8_t GEAR_ALL       = 0x63;  // [action:u8] 0=retract, 1=deploy, 2=stop

    // Servo control
    constexpr uint8_t SERVO_SET      = 0x64;  // [servo_id:u8][pulse_us:u16LE]
    constexpr uint8_t SRV_SETTINGS   = 0x65;  // [servo_id:u8][min_us:u16LE][max_us:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]

    // Configuration
    constexpr uint8_t GEAR_CONFIG    = 0x66;  // [gear_id:u8][flags:u8][stall_current_mA:u16][timeout_ms:u16]
    constexpr uint8_t DOOR_CONFIG    = 0x67;  // [gear_id:u8][open0_us:u16][close0_us:u16][open1_us:u16][close1_us:u16]

    // Yaw control
    constexpr uint8_t YAW_CONFIG     = 0x68;  // [gear_id:u8][neutral_us:u16][min_us:u16][max_us:u16]
    constexpr uint8_t YAW_INPUT      = 0x69;  // [position_us:u16LE]

    // Calibration
    constexpr uint8_t GEAR_CALIBRATE      = 0x6A;  // [gear_id:u8][timeout_s:u8(opt)] Start stall current calibration
    constexpr uint8_t GEAR_CALIB_STATUS   = 0x6B;  // [gear_id:u8][phase:u8][current:u16][peak:u16][stall:u16][finished:u8][errorReason:u8] Calibration progress (server→client)
    constexpr uint8_t GEAR_CALIB_CANCEL   = 0x6C;  // [gear_id:u8] Cancel calibration in progress

    // Battery configuration
    constexpr uint8_t BATTERY_CONFIG      = 0x6D;  // [enabled:u8][auto_deploy:u8] Enable monitoring + auto-deploy

    // Door mode configuration
    constexpr uint8_t DOOR_MODE           = 0x6E;  // [gear_id:u8][pre_deploy:u8][post_deploy:u8][delay_ms:u16LE]

    // Error reset
    constexpr uint8_t GEAR_RESET          = 0x6F;  // [gear_id:u8] Clear error state (ERROR → UNKNOWN)

    // Sequence progress (server → client, long-running)
    constexpr uint8_t GEAR_SEQ_STATUS     = 0x70;  // [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]

    // Channel enable/disable
    constexpr uint8_t GEAR_ENABLE         = 0x71;  // [gear_id:u8][enabled:u8] Enable/disable gear channel

    // Door status (server → client, async)
    constexpr uint8_t GEAR_DOOR_STATUS    = 0x72;  // [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE]

    // Diagnostics — I2C scan is handled by SfxServer (CorePacket::I2C_SCAN)
}

// ============================================================================
// GearControl Error Codes (0x60-0x6F range)
// ============================================================================

namespace GearControlError {
    // Import generic error codes for convenience
    using namespace SerialError;

    constexpr uint8_t INVALID_GEAR_ID    = 0x60;  // Gear ID out of range (0-2)
    constexpr uint8_t INVALID_SERVO_ID   = 0x61;  // Servo ID out of range (0-7)
    constexpr uint8_t GEAR_BUSY          = 0x62;  // Gear is mid-sequence
    constexpr uint8_t MOTOR_STALL        = 0x63;  // Motor stall detected
    constexpr uint8_t MOTOR_TIMEOUT      = 0x64;  // Operation timed out
    constexpr uint8_t SERVO_OUT_OF_RANGE = 0x65;  // Servo pulse out of range
    constexpr uint8_t INA226_ERROR       = 0x66;  // Power monitor communication error
    constexpr uint8_t YAW_NOT_AVAILABLE  = 0x67;  // Yaw not configured for this gear
    constexpr uint8_t INVALID_ACTION      = 0x68;  // Invalid gear-all action
    constexpr uint8_t NO_CURRENT_MONITOR  = 0x69;  // No INA226 attached for calibration
    constexpr uint8_t NOT_CALIBRATING      = 0x6A;  // Gear is not currently calibrating
    constexpr uint8_t GEAR_DISABLED         = 0x6B;  // Gear channel is disabled

    /**
     * @brief Get human-readable error message for GearControl errors
     */
    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case INVALID_GEAR_ID:     return "Invalid gear ID (use 0-2)";
            case INVALID_SERVO_ID:    return "Invalid servo ID (use 0-7)";
            case GEAR_BUSY:           return "Gear busy (sequence in progress)";
            case MOTOR_STALL:         return "Motor stall detected";
            case MOTOR_TIMEOUT:       return "Motor operation timed out";
            case SERVO_OUT_OF_RANGE:  return "Servo pulse out of range (500-2500)";
            case INA226_ERROR:        return "INA226 communication error";
            case YAW_NOT_AVAILABLE:   return "Yaw not configured for this gear";
            case INVALID_ACTION:      return "Invalid action (use 0=retract, 1=deploy, 2=stop)";
            case NO_CURRENT_MONITOR:  return "No current monitor attached (INA226 required)";
            case NOT_CALIBRATING:     return "Gear is not currently calibrating";
            case GEAR_DISABLED:      return "Gear channel is disabled";
            default:
                return SerialError::getMessage(code);
        }
    }
}

// ============================================================================
// Gear Error Reason Codes (for STATUS diagnostic)
// ============================================================================

namespace GearErrorReason {
    constexpr uint8_t NONE           = 0x00;  // No error
    constexpr uint8_t MONITOR_FAULT  = 0x01;  // INA226 I2C init failed
    constexpr uint8_t MOTOR_STALL    = 0x02;  // Motor stall detected during operation
    constexpr uint8_t MOTOR_TIMEOUT  = 0x03;  // Motor operation exceeded timeout
    constexpr uint8_t SEQUENCE_ERROR     = 0x04;  // Unexpected state during sequencing
    constexpr uint8_t MOTOR_DISCONNECTED  = 0x05;  // Motor current dropped to 0 (disconnected)
    constexpr uint8_t CALIB_TIMEOUT        = 0x06;  // Calibration exceeded overall time limit
    constexpr uint8_t NO_STALL_DETECTED     = 0x07;  // No stall current detected in either direction

    inline const char* getName(uint8_t reason) {
        switch (reason) {
            case NONE:               return "none";
            case MONITOR_FAULT:      return "INA226 init failed";
            case MOTOR_STALL:        return "motor stall";
            case MOTOR_TIMEOUT:      return "motor timeout";
            case SEQUENCE_ERROR:     return "sequence error";
            case MOTOR_DISCONNECTED: return "motor disconnected";
            case CALIB_TIMEOUT:      return "calibration timeout";
            case NO_STALL_DETECTED:  return "no stall detected";
            default:                 return "unknown";
        }
    }
}

// ============================================================================
// GearControl Hardware Specification Constants
// ============================================================================

namespace GearControlSpec {
    // Gear limits
    constexpr uint8_t GEAR_COUNT      = 3;
    constexpr uint8_t GEAR_ID_MIN     = 0;
    constexpr uint8_t GEAR_ID_MAX     = 2;

    // Servo limits (standard PWM range)
    constexpr uint8_t SERVO_COUNT     = 8;
    constexpr uint8_t SERVO_ID_MIN    = 0;
    constexpr uint8_t SERVO_ID_MAX    = 7;
    constexpr uint16_t SERVO_PULSE_MIN = 500;    // µs
    constexpr uint16_t SERVO_PULSE_MAX = 2500;   // µs

    // LED limits
    constexpr uint8_t LED_COUNT       = 6;       // 2 per motor

    // Motor limits
    constexpr uint16_t STALL_CURRENT_MIN_mA   = 50;    // Minimum detectable stall
    constexpr uint16_t STALL_CURRENT_MAX_mA   = 5000;  // Maximum expected stall
    constexpr uint16_t MOTOR_TIMEOUT_MIN_ms   = 500;
    constexpr uint16_t MOTOR_TIMEOUT_MAX_ms   = 65000;

    // Gear-all action values
    constexpr uint8_t ACTION_RETRACT  = 0;
    constexpr uint8_t ACTION_DEPLOY   = 1;
    constexpr uint8_t ACTION_STOP     = 2;

    // Validation helpers
    inline bool isValidGearId(uint8_t id) {
        return id <= GEAR_ID_MAX;
    }

    inline bool isValidServoId(uint8_t id) {
        return id <= SERVO_ID_MAX;
    }

    inline bool isValidServoPulse(uint16_t pulse) {
        return pulse >= SERVO_PULSE_MIN && pulse <= SERVO_PULSE_MAX;
    }

    inline bool isValidAction(uint8_t action) {
        return action <= ACTION_STOP;
    }

    inline bool isValidDoorMode(uint8_t mode) {
        return mode <= 4;  // DoorMode::DUAL_SEQ (defined below)
    }

    // Door delay limits
    constexpr uint16_t DOOR_DELAY_MIN_ms = 0;
    constexpr uint16_t DOOR_DELAY_MAX_ms = 5000;
}

// ============================================================================
// GearControl Data Types
// ============================================================================

/**
 * @brief Gear state machine states
 */
enum class GearState : uint8_t {
    UNKNOWN     = 0,
    DEPLOYED    = 1,
    RETRACTED   = 2,
    DEPLOYING   = 3,
    RETRACTING  = 4,
    ERROR       = 5,
    CALIBRATING = 6
};

/**
 * @brief Calibration phase (wire format for GEAR_CALIB_STATUS)
 */
enum class CalibPhase : uint8_t {
    IDLE          = 0,
    CLEAR_RUN     = 1,   // Brief retract to clear deploy endpoint
    CLEAR_SETTLE  = 2,   // Settle after clearing endpoint
    DEPLOY_RUN    = 3,   // Running motor in deploy direction
    MID_SETTLE    = 4,   // Settling between directions
    RETRACT_RUN   = 5,   // Running motor in retract direction
    COMPLETE      = 6,   // Calibration finished successfully
    ERROR         = 7,   // Calibration failed
    CANCELLED     = 8,   // Calibration cancelled by client
    OPENING_DOORS = 9,   // Opening doors before motor calibration
    CLOSING_DOORS = 10   // Closing doors after calibration
};

/**
 * @brief Gear sequence phase (wire format for GEAR_SEQ_STATUS)
 *
 * Maps to the internal GearSeqStep enum values.
 */
namespace GearSeqPhase {
    constexpr uint8_t IDLE           = 0;
    constexpr uint8_t OPENING_DOORS  = 1;
    constexpr uint8_t RUNNING_MOTOR  = 2;
    constexpr uint8_t CLOSING_DOORS  = 3;
    constexpr uint8_t SEQ_ERROR      = 4;
    constexpr uint8_t SYNC_WAIT      = 5;  // Waiting for other gears to reach same phase
}

/**
 * @brief Door state (wire format for STATUS and GEAR_DOOR_STATUS)
 */
namespace DoorState {
    constexpr uint8_t UNKNOWN  = 0;  // State not determined (e.g. after reset)
    constexpr uint8_t CLOSED   = 1;  // Doors at close position
    constexpr uint8_t OPEN     = 2;  // Doors at open position
    constexpr uint8_t OPENING  = 3;  // Doors moving to open position
    constexpr uint8_t CLOSING  = 4;  // Doors moving to close position
}

/**
 * @brief Gear sequence status data (sent in GEAR_SEQ_STATUS packets)
 *
 * Wire format (8 bytes):
 *   [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
 */
struct GearControlSeqStatus {
    uint8_t gearId = 0;
    uint8_t phase = GearSeqPhase::IDLE;  // GearSeqPhase value
    bool deploying = false;              // true = deploying, false = retracting
    bool finished = false;               // true when sequence completed (IDLE or ERROR)
    uint32_t elapsed_ms = 0;             // Time since sequence started               // ms
};

/**
 * @brief Door status data (sent in GEAR_DOOR_STATUS packets)
 *
 * Wire format (6 bytes):
 *   [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE]
 */
struct GearControlDoorStatus {
    uint8_t gearId = 0;
    uint8_t state = 0;           // DoorState value
    uint16_t door0Pos_us = 0;    // Door 0 current position  // µs
    uint16_t door1Pos_us = 0;    // Door 1 current position  // µs
};

/**
 * @brief Calibration status data (sent in GEAR_CALIB_STATUS packets)
 *
 * Wire format (10 bytes):
 *   [gear_id:u8][phase:u8][current_mA:u16LE][peak_mA:u16LE][calibratedStall_mA:u16LE][finished:u8][errorReason:u8]
 *   errorReason: GearErrorReason code (only meaningful when phase=ERROR)
 */
struct GearControlCalibStatus {
    uint8_t gearId = 0;
    CalibPhase phase = CalibPhase::IDLE;
    uint16_t current_mA = 0;           // Live motor current reading         // mA
    uint16_t peak_mA = 0;              // Peak current in current phase      // mA
    uint16_t calibratedStall_mA = 0;   // Final calibrated value (COMPLETE)  // mA
    bool finished = false;              // True when calibration is done (COMPLETE/ERROR/CANCELLED)
    uint8_t errorReason = 0;           // GearErrorReason code (only valid when phase=ERROR)
};

/**
 * @brief Gear configuration flags
 */
namespace GearConfigFlags {
    constexpr uint8_t HAS_YAW = 0x01;  // This gear has yaw servo
    // Runtime flag (set in STATUS, not persisted in config)
    constexpr uint8_t ENABLED = 0x80;  // Bit 7: gear channel is enabled
}

/**
 * @brief Door activation modes for landing gear sequencing
 *
 * Two modes per gear control the full deploy/retract cycle:
 *   doorPreDeploy  - Doors opened before deploy motor, closed after retract motor.
 *   doorPostDeploy - Doors closed after deploy motor, opened before retract motor.
 *                    (Retract is the reverse of the deploy operation.)
 *
 * When doorPostDeploy == NONE: doors stay open after deploy, retract skips pre-retract open.
 * For DUAL_DELAY and DUAL_SEQ: doors open 0→1, close 1→0 (mimics real aircraft).
 */
namespace DoorMode {
    constexpr uint8_t NONE       = 0;  // No door servos (motor only)
    constexpr uint8_t SINGLE     = 1;  // One door servo (servo 0 only)
    constexpr uint8_t DUAL_SYNC  = 2;  // Two doors, simultaneous (default)
    constexpr uint8_t DUAL_DELAY = 3;  // Two doors, door 1 starts after delay_ms
    constexpr uint8_t DUAL_SEQ   = 4;  // Two doors, door 1 starts after door 0 completes
}

/**
 * @brief Per-gear configuration
 */
struct GearControlGearConfig {
    uint8_t gearId = 0;
    uint8_t flags = 0;
    uint16_t stallCurrent_mA = 500;    // Current threshold for stall detection
    uint16_t timeout_ms = 60000;       // Maximum motor run time
};

/**
 * @brief Door servo configuration for one gear
 */
struct GearControlDoorConfig {
    uint8_t gearId = 0;
    uint16_t open0_us = 2000;    // Door servo 0 open position
    uint16_t close0_us = 1000;   // Door servo 0 closed position
    uint16_t open1_us = 2000;    // Door servo 1 open position
    uint16_t close1_us = 1000;   // Door servo 1 closed position
};

/**
 * @brief Door mode configuration for one gear
 *
 * Two modes control the full deploy/retract cycle:
 *   doorPreDeploy  - Doors opened before deploy motor, closed after retract motor.
 *   doorPostDeploy - Doors closed after deploy motor, opened before retract motor.
 *                    NONE = skip post-deploy close and pre-retract open.
 *
 * Wire format (5 bytes): [gear_id:u8][pre_deploy:u8][post_deploy:u8][delay_ms:u16LE]
 */
struct GearControlDoorModeConfig {
    uint8_t gearId = 0;
    uint8_t preDeployMode = DoorMode::DUAL_SYNC;   // Pre-deploy: open before deploy, close after retract
    uint8_t postDeployMode = DoorMode::NONE;       // Post-deploy: close after deploy, open before retract (NONE=skip)
    uint16_t delay_ms = 500;                       // Delay between doors (DUAL_DELAY only)
};

/**
 * @brief Yaw servo configuration
 */
struct GearControlYawConfig {
    uint8_t gearId = 0;         // Associated gear (yaw active when this gear deployed)
    uint16_t neutral_us = 1500;  // Neutral/center position
    uint16_t min_us = 1000;      // Minimum yaw position
    uint16_t max_us = 2000;      // Maximum yaw position
};

/**
 * @brief Servo settings configuration
 *
 * Matches GunFX/LightFX SRV_SETTINGS pattern.
 * Wire format: [id:u8][min:u16LE][max:u16LE][speed:u16LE][accel:u16LE][decel:u16LE]
 * Total: 11 bytes
 */
struct GearControlServoConfig {
    uint8_t servoId = 0;
    uint16_t minUs = 500;                  // Minimum pulse width limit          // µs
    uint16_t maxUs = 2500;                 // Maximum pulse width limit          // µs
    uint16_t maxSpeedUsPerSec = 4000;      // Maximum speed                     // µs/s
    uint16_t maxAccelUsPerSec2 = 8000;     // Acceleration                      // µs/s²
    uint16_t maxDecelUsPerSec2 = 8000;     // Deceleration                      // µs/s²
};

/**
 * @brief Per-gear status data
 */
struct GearControlGearStatus {
    GearState state = GearState::UNKNOWN;
    uint16_t motorCurrent_mA = 0;
    uint16_t door0Pos_us = 0;
    uint16_t door1Pos_us = 0;
    uint16_t calibratedStall_mA = 0;
};

/**
 * @brief Complete GearControl status
 */
struct GearControlStatus {
    GearControlGearStatus gear[3];
    uint16_t yawPos_us = 1500;
    uint8_t ledFlags = 0;        // Bits 0-5 status LEDs, 6-7 indicator LEDs
    uint16_t batteryVoltage_mV = 0;
};

/**
 * @brief Board information returned during init (alias for BusClientBoardInfo)
 */
using GearControlBoardInfo = BusClientBoardInfo;

// ============================================================================
// Callback Types
// ============================================================================

// Module callbacks
using GearControlStatusCallback = std::function<void(const GearControlStatus& status)>;

// Server callbacks - return error code (SerialError::OK for success)
using GearControlGearDeployCallback = std::function<uint8_t(uint8_t gearId)>;
using GearControlGearRetractCallback = std::function<uint8_t(uint8_t gearId)>;
using GearControlGearStopCallback = std::function<uint8_t(uint8_t gearId)>;
using GearControlGearAllCallback = std::function<uint8_t(uint8_t action)>;
using GearControlServoSetCallback = std::function<uint8_t(uint8_t servoId, uint16_t pulse_us)>;
using GearControlServoSettingsCallback = std::function<uint8_t(const GearControlServoConfig& config)>;
using GearControlGearConfigCallback = std::function<uint8_t(const GearControlGearConfig& config)>;
using GearControlDoorConfigCallback = std::function<uint8_t(const GearControlDoorConfig& config)>;
using GearControlYawConfigCallback = std::function<uint8_t(const GearControlYawConfig& config)>;
using GearControlYawInputCallback = std::function<uint8_t(uint16_t position_us)>;
using GearControlGearCalibrateCallback = std::function<uint8_t(uint8_t gearId, uint8_t timeout_s)>;
using GearControlCalibCancelCallback = std::function<uint8_t(uint8_t gearId)>;
using GearControlCalibStatusCallback = std::function<void(const GearControlCalibStatus& status)>;
using GearControlBatteryConfigCallback = std::function<uint8_t(bool enabled, bool autoDeployOnLowVoltage)>;
using GearControlDoorModeCallback = std::function<uint8_t(const GearControlDoorModeConfig& config)>;
using GearControlGearResetCallback = std::function<uint8_t(uint8_t gearId)>;
using GearControlGearEnableCallback = std::function<uint8_t(uint8_t gearId, bool enabled)>;
using GearControlSeqStatusCallback = std::function<void(const GearControlSeqStatus& status)>;
using GearControlDoorStatusCallback = std::function<void(const GearControlDoorStatus& status)>;

// ============================================================================
// GearControlClient Class (Binary Protocol)
// ============================================================================

/**
 * @brief Client-side GearControl serial communication (binary COBS protocol)
 *
 * Used by HubFX to send commands to GearControl server boards over USB.
 * Extends BusClient with GearControl-specific commands.
 */
class GearControlClient : public BusClient {
public:
    GearControlClient() = default;
    ~GearControlClient() override = default;

    GearControlClient(const GearControlClient&) = delete;
    GearControlClient& operator=(const GearControlClient&) = delete;

    // ========================================================================
    // Gear Control
    // ========================================================================

    CommandResult gearDeploy(uint8_t gearId);
    CommandResult gearRetract(uint8_t gearId);
    CommandResult gearStop(uint8_t gearId);
    CommandResult gearAll(uint8_t action);
    CommandResult gearCalibrate(uint8_t gearId, uint8_t timeout_s = 0);
    CommandResult gearCalibCancel(uint8_t gearId);
    CommandResult gearReset(uint8_t gearId);
    CommandResult gearEnable(uint8_t gearId, bool enabled);

    // ========================================================================
    // Servo Control
    // ========================================================================

    CommandResult setServoPosition(uint8_t servoId, uint16_t pulse_us);
    CommandResult setServoSettings(const GearControlServoConfig& config);

    // ========================================================================
    // Configuration
    // ========================================================================

    CommandResult setGearConfig(const GearControlGearConfig& config);
    CommandResult setDoorConfig(const GearControlDoorConfig& config);
    CommandResult setYawConfig(const GearControlYawConfig& config);
    CommandResult setYawInput(uint16_t position_us);
    CommandResult setBatteryConfig(bool enabled, bool autoDeployOnLowVoltage = false);
    CommandResult setDoorMode(const GearControlDoorModeConfig& config);
    CommandResult requestI2CScan();

    // ========================================================================
    // Status
    // ========================================================================

    CommandResult requestStatus();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onStatus(GearControlStatusCallback cb) { _statusCallback = cb; }
    void onCalibStatus(GearControlCalibStatusCallback cb) { _calibStatusCallback = cb; }
    void onGearSeqStatus(GearControlSeqStatusCallback cb) { _gearSeqStatusCallback = cb; }
    void onDoorStatus(GearControlDoorStatusCallback cb) { _doorStatusCallback = cb; }
    void onI2CScanResult(std::function<void(const I2CScanResult&)> cb) { _i2cScanResultCallback = cb; }

    // ========================================================================
    // State
    // ========================================================================

    const GearControlStatus& lastStatus() const { return _lastStatus; }

protected:
    void onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) override;
    const char* getModuleErrorMessage(uint8_t code) override { return GearControlError::getMessage(code); }

private:
    GearControlStatus _lastStatus;

    GearControlStatusCallback _statusCallback;
    GearControlCalibStatusCallback _calibStatusCallback;
    GearControlSeqStatusCallback _gearSeqStatusCallback;
    GearControlDoorStatusCallback _doorStatusCallback;
    std::function<void(const I2CScanResult&)> _i2cScanResultCallback;
};

// ============================================================================
// GearControlServer Class (Binary Protocol)
// ============================================================================

/**
 * @brief Server-side GearControl serial communication (binary COBS protocol)
 *
 * Used by GearControl Pico to receive commands from HubFX client.
 * Extends BusServer with GearControl-specific command dispatch.
 */
class GearControlServer : public BusServer {
public:
    GearControlServer() = default;
    ~GearControlServer() override = default;

    GearControlServer(const GearControlServer&) = delete;
    GearControlServer& operator=(const GearControlServer&) = delete;

    // ========================================================================
    // ICommandHandler Interface
    // ========================================================================

    const char* handlerName() const override { return "GearControlServer"; }

    // ========================================================================
    // Response Methods
    // ========================================================================

    int sendCalibStatus(const GearControlCalibStatus& status);
    int sendGearSeqStatus(const GearControlSeqStatus& status);
    int sendDoorStatus(const GearControlDoorStatus& status);

    // ========================================================================
    // Callbacks
    // ========================================================================

    void onGearDeploy(GearControlGearDeployCallback cb) { _gearDeployCallback = cb; }
    void onGearRetract(GearControlGearRetractCallback cb) { _gearRetractCallback = cb; }
    void onGearStop(GearControlGearStopCallback cb) { _gearStopCallback = cb; }
    void onGearAll(GearControlGearAllCallback cb) { _gearAllCallback = cb; }
    void onServoSet(GearControlServoSetCallback cb) { _servoSetCallback = cb; }
    void onServoSettings(GearControlServoSettingsCallback cb) { _servoSettingsCallback = cb; }
    void onGearConfig(GearControlGearConfigCallback cb) { _gearConfigCallback = cb; }
    void onDoorConfig(GearControlDoorConfigCallback cb) { _doorConfigCallback = cb; }
    void onYawConfig(GearControlYawConfigCallback cb) { _yawConfigCallback = cb; }
    void onYawInput(GearControlYawInputCallback cb) { _yawInputCallback = cb; }
    void onGearCalibrate(GearControlGearCalibrateCallback cb) { _gearCalibrateCallback = cb; }
    void onGearCalibCancel(GearControlCalibCancelCallback cb) { _gearCalibCancelCallback = cb; }
    void onBatteryConfig(GearControlBatteryConfigCallback cb) { _batteryConfigCallback = cb; }
    void onDoorMode(GearControlDoorModeCallback cb) { _doorModeCallback = cb; }
    void onGearReset(GearControlGearResetCallback cb) { _gearResetCallback = cb; }
    void onGearEnable(GearControlGearEnableCallback cb) { _gearEnableCallback = cb; }

protected:
    CommandHandleResult handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) override;
    uint8_t moduleRangeLow() const override { return 0x60; }
    uint8_t moduleRangeHigh() const override { return 0x7F; }
    const char* getModuleErrorMessage(uint8_t code) override { return GearControlError::getMessage(code); }

private:
    uint8_t _calibTag = 0;   // Tag from GEAR_CALIBRATE request, echoed in CALIB_STATUS responses
    uint8_t _gearTag[3] = {};  // Per-gear tags from GEAR_DEPLOY/RETRACT, echoed in GEAR_SEQ_STATUS

    GearControlGearDeployCallback _gearDeployCallback;
    GearControlGearRetractCallback _gearRetractCallback;
    GearControlGearStopCallback _gearStopCallback;
    GearControlGearAllCallback _gearAllCallback;
    GearControlServoSetCallback _servoSetCallback;
    GearControlServoSettingsCallback _servoSettingsCallback;
    GearControlGearConfigCallback _gearConfigCallback;
    GearControlDoorConfigCallback _doorConfigCallback;
    GearControlYawConfigCallback _yawConfigCallback;
    GearControlYawInputCallback _yawInputCallback;
    GearControlGearCalibrateCallback _gearCalibrateCallback;
    GearControlCalibCancelCallback _gearCalibCancelCallback;
    GearControlBatteryConfigCallback _batteryConfigCallback;
    GearControlDoorModeCallback _doorModeCallback;
    GearControlGearResetCallback _gearResetCallback;
    GearControlGearEnableCallback _gearEnableCallback;
};

#endif // SERIAL_GEARCONTROL_H
