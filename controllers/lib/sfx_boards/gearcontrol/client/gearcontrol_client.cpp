/*
 * GearControl Client — Protocol Implementation
 *
 * GearControlClient command methods and response parsing.
 */

#include "gearcontrol_client.h"

using namespace CoreProtocol;

// ============================================================================
// GearControlClient - Module Packet Handler
// ============================================================================

void GearControlClient::onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::STATUS:
            // Format: 22-byte core header + 32-byte module data
            // Module data per gear (3 × 9 = 27 bytes):
            //   [state:u8][motorCurrent_mA:u16][door0:u16][door1:u16][calibratedStall_mA:u16]
            // Trailing (5 bytes): [yawPos:u16][ledFlags:u8][batteryVoltage_mV:u16]
            if (len >= CoreProtocol::STATUS_CORE_HEADER_SIZE + 32) {
                const uint8_t* mod = &payload[CoreProtocol::STATUS_CORE_HEADER_SIZE];
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
            } else if (len >= CoreProtocol::STATUS_CORE_HEADER_SIZE) {
                if (tag != CoreProtocol::TAG_ASYNC) {
                    _lastCommandResult = CommandResult::Ack();
                    _resultQueue.resolve(tag, _lastCommandResult);
                }
                if (_statusCallback) _statusCallback(_lastStatus);
            }
            break;

        case GearControlPacket::GEAR_CALIB_STATUS:
            // Calibration progress: [gear_id:u8][phase:u8][current:u16LE][peak:u16LE][stall:u16LE][finished:u8][errorReason:u8]
            if (len >= 9) {
                GearControlCalibStatus cs;
                cs.gearId = payload[0];
                cs.phase = static_cast<CalibPhase>(payload[1]);
                cs.current_mA = getU16LE(&payload[2]);
                cs.peak_mA = getU16LE(&payload[4]);
                cs.calibratedStall_mA = getU16LE(&payload[6]);
                cs.finished = (payload[8] != 0);
                cs.errorReason = (len >= 10) ? payload[9] : 0;  // Optional: backward-compatible
                if (_calibStatusCallback) _calibStatusCallback(cs);

                // When calibration finishes, resolve the original tag so any
                // blocking caller waiting on gearCalibrate() gets unblocked
                if (cs.finished && tag != CoreProtocol::TAG_ASYNC) {
                    bool isError = (cs.phase == CalibPhase::ERROR);
                    if (isError) {
                        // Map error reason to appropriate NACK code
                        uint8_t errCode = GearControlError::MOTOR_STALL;  // default
                        const char* errMsg = "Calibration failed";
                        if (cs.errorReason == GearErrorReason::CALIB_TIMEOUT) {
                            errCode = GearControlError::MOTOR_TIMEOUT;
                            errMsg = "Calibration timed out";
                        } else if (cs.errorReason == GearErrorReason::MOTOR_DISCONNECTED) {
                            errCode = GearControlError::INA226_ERROR;
                            errMsg = "Motor disconnected during calibration";
                        } else if (cs.errorReason == GearErrorReason::NO_STALL_DETECTED) {
                            errCode = GearControlError::MOTOR_STALL;
                            errMsg = "No stall current detected";
                        }
                        _lastCommandResult = CommandResult::Nack(errCode, errMsg);
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

        case GearControlPacket::GEAR_DOOR_STATUS:
            // Door state transition: [gear_id:u8][state:u8][door0_pos_us:u16LE][door1_pos_us:u16LE]
            if (len >= 5) {
                GearControlDoorStatus ds;
                ds.gearId = payload[0];
                ds.state = payload[1];
                ds.door0Pos_us = (len >= 5) ? getU16LE(&payload[2]) : 0;  // µs
                ds.door1Pos_us = (len >= 7) ? getU16LE(&payload[4]) : 0;  // µs
                if (_doorStatusCallback) _doorStatusCallback(ds);
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

CommandResult GearControlClient::gearCalibrate(uint8_t gearId, uint8_t timeout_s) {
    if (timeout_s > 0) {
        uint8_t payload[2] = { gearId, timeout_s };
        return sendCommand(GearControlPacket::GEAR_CALIBRATE, payload, sizeof(payload));
    }
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_CALIBRATE, payload, sizeof(payload));
}

CommandResult GearControlClient::gearCalibCancel(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_CALIB_CANCEL, payload, sizeof(payload));
}

CommandResult GearControlClient::gearReset(uint8_t gearId) {
    uint8_t payload[1] = { gearId };
    return sendCommand(GearControlPacket::GEAR_RESET, payload, sizeof(payload));
}

CommandResult GearControlClient::gearEnable(uint8_t gearId, bool enabled) {
    uint8_t payload[2] = { gearId, (uint8_t)(enabled ? 1 : 0) };
    return sendCommand(GearControlPacket::GEAR_ENABLE, payload, sizeof(payload));
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
    uint8_t payload[12];
    payload[0] = config.servoId;
    putU16LE(&payload[1], config.minUs);                // min_us
    putU16LE(&payload[3], config.maxUs);                // max_us
    putU16LE(&payload[5], config.maxSpeedUsPerSec);     // speed  // µs/s
    putU16LE(&payload[7], config.maxAccelUsPerSec2);    // accel  // µs/s²
    putU16LE(&payload[9], config.maxDecelUsPerSec2);    // decel  // µs/s²
    if (config.reversed) {
        payload[11] = 1;
        return sendCommand(GearControlPacket::SRV_SETTINGS, payload, 12);
    }
    return sendCommand(GearControlPacket::SRV_SETTINGS, payload, 11);
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

CommandResult GearControlClient::setBatteryConfig(bool autoDeployOnLowVoltage,
                                                  uint8_t chemistry, uint8_t cellCount) {
    uint8_t payload[3];
    payload[0] = autoDeployOnLowVoltage ? 1 : 0;
    payload[1] = chemistry;
    payload[2] = cellCount;  // 0 = auto-detect
    return sendCommand(GearControlPacket::BATTERY_CONFIG, payload, sizeof(payload));
}

CommandResult GearControlClient::setDoorMode(const GearControlDoorModeConfig& config) {
    uint8_t payload[5];
    payload[0] = config.gearId;
    payload[1] = config.preDeployMode;
    payload[2] = config.postDeployMode;
    putU16LE(&payload[3], config.delay_ms);
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
