/*
 * Serial GearControl Protocol - Implementation
 *
 * GearControl-specific client/server protocol implementation.
 *   - GearControlClient: Module-specific commands and response parsing
 *   - GearControlServer: Module-specific command dispatch and response methods
 */

#include "serial_gearcontrol.h"

using namespace CoreProtocol;

// ============================================================================
// GearControlClient - Module Packet Handler
// ============================================================================

void GearControlClient::onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::STATUS:
            // Format: [counter:u32][uptime:u32][freeRam:u32][moduleData:32]
            // Module data per gear (3 × 9 = 27 bytes):
            //   [state:u8][motorCurrent_mA:u16][door0:u16][door1:u16][calibratedStall_mA:u16]
            // Trailing (5 bytes): [yawPos:u16][ledFlags:u8][batteryVoltage_mV:u16]
            if (len >= 44) {  // 12 core + 32 module
                const uint8_t* mod = &payload[12];
                for (int i = 0; i < 3; i++) {
                    size_t off = i * 9;
                    _lastStatus.gear[i].state = static_cast<GearState>(mod[off]);
                    _lastStatus.gear[i].motorCurrent_mA = getU16LE(&mod[off + 1]);
                    _lastStatus.gear[i].door0Pos_us = getU16LE(&mod[off + 3]);
                    _lastStatus.gear[i].door1Pos_us = getU16LE(&mod[off + 5]);
                    _lastStatus.gear[i].calibratedStall_mA = getU16LE(&mod[off + 7]);
                }
                _lastStatus.yawPos_us = getU16LE(&mod[27]);
                _lastStatus.ledFlags = mod[29];
                _lastStatus.batteryVoltage_mV = getU16LE(&mod[30]);

                if (tag != CoreProtocol::TAG_ASYNC) {
                    _lastCommandResult = CommandResult::Ack();
                    _resultQueue.resolve(tag, _lastCommandResult);
                }
                if (_statusCallback) _statusCallback(_lastStatus);
            } else if (len >= 12) {
                if (tag != CoreProtocol::TAG_ASYNC) {
                    _lastCommandResult = CommandResult::Ack();
                    _resultQueue.resolve(tag, _lastCommandResult);
                }
                if (_statusCallback) _statusCallback(_lastStatus);
            }
            break;

        case GearControlPacket::GEAR_CALIB_STATUS:
            // Calibration progress: [gear_id:u8][phase:u8][current:u16LE][peak:u16LE][stall:u16LE][finished:u8]
            if (len >= 9) {
                GearControlCalibStatus cs;
                cs.gearId = payload[0];
                cs.phase = static_cast<CalibPhase>(payload[1]);
                cs.current_mA = getU16LE(&payload[2]);
                cs.peak_mA = getU16LE(&payload[4]);
                cs.calibratedStall_mA = getU16LE(&payload[6]);
                cs.finished = (payload[8] != 0);
                if (_calibStatusCallback) _calibStatusCallback(cs);

                // When calibration finishes, resolve the original tag so any
                // blocking caller waiting on gearCalibrate() gets unblocked
                if (cs.finished && tag != CoreProtocol::TAG_ASYNC) {
                    bool isError = (cs.phase == CalibPhase::ERROR);
                    if (isError) {
                        _lastCommandResult = CommandResult::Nack(GearControlError::MOTOR_STALL, "Calibration failed");
                    } else {
                        _lastCommandResult = CommandResult::Ack();
                    }
                    _resultQueue.resolve(tag, _lastCommandResult);
                }
            }
            break;

        case GearControlPacket::GEAR_SEQ_STATUS:
            // Gear sequence progress: [gear_id:u8][phase:u8][deploying:u8][finished:u8][elapsed_ms:u32LE]
            if (len >= 8) {
                GearControlSeqStatus ss;
                ss.gearId = payload[0];
                ss.phase = payload[1];
                ss.deploying = (payload[2] != 0);
                ss.finished = (payload[3] != 0);
                ss.elapsed_ms = getU32LE(&payload[4]);                       // ms
                if (_gearSeqStatusCallback) _gearSeqStatusCallback(ss);

                // When sequence finishes, resolve the original tag
                if (ss.finished && tag != CoreProtocol::TAG_ASYNC) {
                    bool isError = (ss.phase == GearSeqPhase::SEQ_ERROR);
                    if (isError) {
                        _lastCommandResult = CommandResult::Nack(GearControlError::MOTOR_STALL, "Gear sequence error");
                    } else {
                        _lastCommandResult = CommandResult::Ack();
                    }
                    _resultQueue.resolve(tag, _lastCommandResult);
                }
            }
            break;

        case CorePacket::I2C_SCAN_RESULT:
            // I2C scan response: [numExp:u8][N×(addr,found,id)][numExtra:u8][M×addr]
            if (len >= 2 && _i2cScanResultCallback) {
                I2CScanResult result;
                size_t idx = 0;
                result.numExpected = payload[idx++];
                if (result.numExpected > I2CScanResult::MAX_EXPECTED) {
                    result.numExpected = I2CScanResult::MAX_EXPECTED;
                }
                for (uint8_t i = 0; i < result.numExpected && idx + 2 < len; i++) {
                    result.expected[i].address = payload[idx++];
                    result.expected[i].found = payload[idx++] != 0;
                    result.expected[i].identified = payload[idx++] != 0;
                }
                if (idx < len) {
                    result.numExtra = payload[idx++];
                    if (result.numExtra > I2CScanResult::MAX_EXTRA) {
                        result.numExtra = I2CScanResult::MAX_EXTRA;
                    }
                    for (uint8_t i = 0; i < result.numExtra && idx < len; i++) {
                        result.extraAddresses[i] = payload[idx++];
                    }
                }
                _i2cScanResultCallback(result);
            }
            // Treat as implicit ACK for blocking calls
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// GearControlClient - Gear Control
// ============================================================================

CommandResult GearControlClient::gearDeploy(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_DEPLOY, payload, sizeof(payload));
}

CommandResult GearControlClient::gearRetract(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_RETRACT, payload, sizeof(payload));
}

CommandResult GearControlClient::gearStop(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_STOP, payload, sizeof(payload));
}

CommandResult GearControlClient::gearAll(uint8_t action) {
    uint8_t payload[1] = { action };
    return sendCommand(GearControlPacket::GEAR_ALL, payload, sizeof(payload));
}

CommandResult GearControlClient::gearCalibrate(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_CALIBRATE, payload, sizeof(payload));
}

CommandResult GearControlClient::gearCalibCancel(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_CALIB_CANCEL, payload, sizeof(payload));
}

// ============================================================================
// GearControlClient - Servo Control
// ============================================================================

CommandResult GearControlClient::setServoPosition(uint8_t servoId, uint16_t pulse_us) {
    uint8_t payload[3];
    payload[0] = servoId;
    putU16LE(&payload[1], pulse_us);
    return sendCommand(GearControlPacket::SERVO_SET, payload, sizeof(payload));
}

CommandResult GearControlClient::setServoSettings(const GearControlServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    putU16LE(&payload[1], config.minUs);                // min_us
    putU16LE(&payload[3], config.maxUs);                // max_us
    putU16LE(&payload[5], config.maxSpeedUsPerSec);     // speed  // µs/s
    putU16LE(&payload[7], config.maxAccelUsPerSec2);    // accel  // µs/s²
    putU16LE(&payload[9], config.maxDecelUsPerSec2);    // decel  // µs/s²
    return sendCommand(GearControlPacket::SRV_SETTINGS, payload, sizeof(payload));
}

// ============================================================================
// GearControlClient - Configuration
// ============================================================================

CommandResult GearControlClient::setGearConfig(const GearControlGearConfig& config) {
    uint8_t payload[6];
    payload[0] = config.gearId;
    payload[1] = config.flags;
    putU16LE(&payload[2], config.stallCurrent_mA);
    putU16LE(&payload[4], config.timeout_ms);
    return sendCommand(GearControlPacket::GEAR_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setDoorConfig(const GearControlDoorConfig& config) {
    uint8_t payload[9];
    payload[0] = config.gearId;
    putU16LE(&payload[1], config.open0_us);
    putU16LE(&payload[3], config.close0_us);
    putU16LE(&payload[5], config.open1_us);
    putU16LE(&payload[7], config.close1_us);
    return sendCommand(GearControlPacket::DOOR_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setYawConfig(const GearControlYawConfig& config) {
    uint8_t payload[7];
    payload[0] = config.gearId;
    putU16LE(&payload[1], config.neutral_us);
    putU16LE(&payload[3], config.min_us);
    putU16LE(&payload[5], config.max_us);
    return sendCommand(GearControlPacket::YAW_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setYawInput(uint16_t position_us) {
    uint8_t payload[2];
    putU16LE(payload, position_us);
    return sendCommand(GearControlPacket::YAW_INPUT, payload, sizeof(payload));
}

CommandResult GearControlClient::setBatteryConfig(bool enabled, bool autoDeployOnLowVoltage) {
    uint8_t payload[2];
    payload[0] = enabled ? 1 : 0;
    payload[1] = autoDeployOnLowVoltage ? 1 : 0;
    return sendCommand(GearControlPacket::BATTERY_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setDoorMode(const GearControlDoorModeConfig& config) {
    uint8_t payload[4];
    payload[0] = config.gearId;
    payload[1] = config.mode;
    putU16LE(&payload[2], config.delay_ms);
    return sendCommand(GearControlPacket::DOOR_MODE, payload, sizeof(payload));
}

CommandResult GearControlClient::requestI2CScan() {
    return sendCommand(CorePacket::I2C_SCAN, nullptr, 0);
}

// ============================================================================
// GearControlClient - Status
// ============================================================================

CommandResult GearControlClient::requestStatus() {
    return sendCommand(CorePacket::STATUS_REQ, nullptr, 0);
}

// ============================================================================
// GearControlServer - Module Packet Handler
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
            config.minUs = getU16LE(&payload[1]);              // min_us             // µs
            config.maxUs = getU16LE(&payload[3]);              // max_us             // µs
            config.maxSpeedUsPerSec = getU16LE(&payload[5]);   // speed              // µs/s
            config.maxAccelUsPerSec2 = getU16LE(&payload[7]);  // accel              // µs/s²
            config.maxDecelUsPerSec2 = getU16LE(&payload[9]);  // decel              // µs/s²
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
            SFX_VALIDATE(GearControlSpec::isValidGearId(gearId), GearControlError::INVALID_GEAR_ID);
            _calibTag = _currentTag;  // Store tag for calibration status responses
            SFX_DISPATCH(_gearCalibrateCallback, gearId);
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
            SFX_REQUIRE_LEN(4);
            GearControlDoorModeConfig config;
            config.gearId = payload[0];
            config.mode = payload[1];
            config.delay_ms = getU16LE(&payload[2]);
            SFX_VALIDATE(GearControlSpec::isValidGearId(config.gearId), GearControlError::INVALID_GEAR_ID);
            SFX_VALIDATE(GearControlSpec::isValidDoorMode(config.mode), GearControlError::INVALID_ACTION);
            SFX_DISPATCH(_doorModeCallback, config);
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// GearControlServer - Response Methods
// ============================================================================

int GearControlServer::sendCalibStatus(const GearControlCalibStatus& status) {
    uint8_t payload[9];
    payload[0] = status.gearId;
    payload[1] = static_cast<uint8_t>(status.phase);
    putU16LE(&payload[2], status.current_mA);          // mA
    putU16LE(&payload[4], status.peak_mA);              // mA
    putU16LE(&payload[6], status.calibratedStall_mA);   // mA
    payload[8] = status.finished ? 1 : 0;
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
