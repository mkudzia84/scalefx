/*
 * GearControl Server — Binary Protocol Command Handler (Implementation)
 *
 * Handles GearControl-specific binary COBS protocol packets:
 * gear deploy/retract/stop/all, servo, config, calibration, door, yaw, battery.
 */

#include "gearcontrol_server.h"

using namespace CoreProtocol;

// ============================================================================
// GearControlServer - Command Dispatch
// ============================================================================

CommandHandleResult GearControlServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case GearControlPacket::GEAR_DEPLOY: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            _gearTag[gearId] = _currentTag;  // Store tag for sequence progress responses
            SFX_DISPATCH(_gearDeployCallback, gearId);
        }

        case GearControlPacket::GEAR_RETRACT: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            _gearTag[gearId] = _currentTag;  // Store tag for sequence progress responses
            SFX_DISPATCH(_gearRetractCallback, gearId);
        }

        case GearControlPacket::GEAR_STOP: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearStopCallback, gearId);
        }

        case GearControlPacket::GEAR_ALL: {
            SFX_REQUIRE_LEN(1);
            uint8_t action = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidAction(action), GearControlError::INVALID_ACTION);
            // Store tag for all gears when deploying/retracting
            if (action == GearControlSpec::ACTION_DEPLOY || action == GearControlSpec::ACTION_RETRACT) {
                for (int i = 0; i < 3; i++) _gearTag[i] = _currentTag;
            }
            SFX_DISPATCH(_gearAllCallback, action);
        }

        case GearControlPacket::SERVO_SET: {
            SFX_REQUIRE_LEN(3);
            uint8_t servoId = payload[0];
            uint16_t pulse_us = getU16LE(&payload[1]);
            SFX_VALIDATE(GearControlSpec::isValidServoId(servoId), GearControlError::INVALID_SERVO_ID);
            SFX_VALIDATE(GearControlSpec::isValidServoPulse(pulse_us), GearControlError::SERVO_OUT_OF_RANGE);
            SFX_DISPATCH(_servoSetCallback, servoId, pulse_us);
        }

        case GearControlPacket::SRV_SETTINGS: {
            SFX_REQUIRE_LEN(11);
            GearControlServoConfig config;
            config.servoId = payload[0];
            config.minUs = getU16LE(&payload[1]);              // µs
            config.maxUs = getU16LE(&payload[3]);              // µs
            config.maxSpeedUsPerSec = getU16LE(&payload[5]);   // µs/s
            config.maxAccelUsPerSec2 = getU16LE(&payload[7]);  // µs/s²
            config.maxDecelUsPerSec2 = getU16LE(&payload[9]);  // µs/s²
            SFX_VALIDATE(GearControlSpec::isValidServoId(config.servoId), GearControlError::INVALID_SERVO_ID);
            SFX_VALIDATE(GearControlSpec::isValidServoPulse(config.minUs) &&
                         GearControlSpec::isValidServoPulse(config.maxUs), GearControlError::SERVO_OUT_OF_RANGE);
            SFX_VALIDATE(config.minUs < config.maxUs, GearControlError::SERVO_OUT_OF_RANGE);
            SFX_DISPATCH(_servoSettingsCallback, config);
        }

        case GearControlPacket::GEAR_CONFIG: {
            SFX_REQUIRE_LEN(6);
            GearControlGearConfig config;
            config.gearId = payload[0];
            config.flags = payload[1];
            config.stallCurrent_mA = getU16LE(&payload[2]);
            config.timeout_ms = getU16LE(&payload[4]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearConfigCallback, config);
        }

        case GearControlPacket::DOOR_CONFIG: {
            SFX_REQUIRE_LEN(9);
            GearControlDoorConfig config;
            config.gearId = payload[0];
            config.open0_us = getU16LE(&payload[1]);
            config.close0_us = getU16LE(&payload[3]);
            config.open1_us = getU16LE(&payload[5]);
            config.close1_us = getU16LE(&payload[7]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_doorConfigCallback, config);
        }

        case GearControlPacket::YAW_CONFIG: {
            SFX_REQUIRE_LEN(7);
            GearControlYawConfig config;
            config.gearId = payload[0];
            config.neutral_us = getU16LE(&payload[1]);
            config.min_us = getU16LE(&payload[3]);
            config.max_us = getU16LE(&payload[5]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_yawConfigCallback, config);
        }

        case GearControlPacket::YAW_INPUT: {
            SFX_REQUIRE_LEN(2);
            uint16_t position_us = getU16LE(payload);
            SFX_DISPATCH(_yawInputCallback, position_us);
        }

        case GearControlPacket::GEAR_CALIBRATE: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            uint8_t timeout_s = (len >= 2) ? payload[1] : 0;  // Optional overall timeout in seconds
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            _calibTag = _currentTag;  // Store tag for calibration status responses
            SFX_DISPATCH(_gearCalibrateCallback, gearId, timeout_s);
        }

        case GearControlPacket::GEAR_CALIB_CANCEL: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearCalibCancelCallback, gearId);
        }

        case GearControlPacket::BATTERY_CONFIG: {
            SFX_REQUIRE_LEN(2);
            bool enabled = payload[0] != 0;
            bool autoDeploy = payload[1] != 0;
            SFX_DISPATCH(_batteryConfigCallback, enabled, autoDeploy);
        }

        case GearControlPacket::DOOR_MODE: {
            SFX_REQUIRE_LEN(5);
            GearControlDoorModeConfig config;
            config.gearId = payload[0];
            config.preDeployMode = payload[1];
            config.postDeployMode = payload[2];
            config.delay_ms = getU16LE(&payload[3]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_VALIDATE(GearControlSpec::isValidDoorMode(config.preDeployMode), GearControlError::INVALID_ACTION);
            SFX_VALIDATE(GearControlSpec::isValidDoorMode(config.postDeployMode), GearControlError::INVALID_ACTION);
            SFX_DISPATCH(_doorModeCallback, config);
        }

        case GearControlPacket::GEAR_RESET: {
            SFX_REQUIRE_LEN(1);
            uint8_t gearId = payload[0];
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearResetCallback, gearId);
        }

        case GearControlPacket::GEAR_ENABLE: {
            SFX_REQUIRE_LEN(2);
            uint8_t gearId = payload[0];
            bool enabled = payload[1] != 0;
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            SFX_DISPATCH(_gearEnableCallback, gearId, enabled);
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// GearControlServer - Response Methods
// ============================================================================

int GearControlServer::sendCalibStatus(const GearControlCalibStatus& status) {
    uint8_t payload[10];
    payload[0] = status.gearId;
    payload[1] = static_cast<uint8_t>(status.phase);
    putU16LE(&payload[2], status.current_mA);          // mA
    putU16LE(&payload[4], status.peak_mA);              // mA
    putU16LE(&payload[6], status.calibratedStall_mA);   // mA
    payload[8] = status.finished ? 1 : 0;
    payload[9] = status.errorReason;                    // GearErrorReason code
    return sendRawPacket(GearControlPacket::GEAR_CALIB_STATUS, _calibTag, payload, sizeof(payload));
}

int GearControlServer::sendGearSeqStatus(const GearControlSeqStatus& status) {
    uint8_t payload[8];
    payload[0] = status.gearId;
    payload[1] = status.phase;
    payload[2] = status.deploying ? 1 : 0;
    payload[3] = status.finished ? 1 : 0;
    putU32LE(&payload[4], status.elapsed_ms);                    // ms
    uint8_t tag = (status.gearId < 3) ? _gearTag[status.gearId] : 0;
    return sendRawPacket(GearControlPacket::GEAR_SEQ_STATUS, tag, payload, sizeof(payload));
}

int GearControlServer::sendDoorStatus(const GearControlDoorStatus& status) {
    uint8_t payload[6];
    payload[0] = status.gearId;
    payload[1] = status.state;
    putU16LE(&payload[2], status.door0Pos_us);  // µs
    putU16LE(&payload[4], status.door1Pos_us);  // µs
    // Door status is always async (unsolicited)
    return sendRawPacket(GearControlPacket::GEAR_DOOR_STATUS, CoreProtocol::TAG_ASYNC, payload, sizeof(payload));
}
