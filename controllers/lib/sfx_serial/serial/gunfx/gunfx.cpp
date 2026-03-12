/*
 * Serial GunFX Protocol - Implementation
 *
 * GunFX-specific client/server protocol implementation.
 *   - GunFxClient: Module-specific commands and STATUS parsing
 *   - GunFxServer: Module-specific command dispatch and response methods
 */

#include "gunfx.h"

using namespace CoreProtocol;

// ============================================================================
// GunFxClient - Module Packet Handler
// ============================================================================

void GunFxClient::onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::STATUS:
            // New format: [counter:u32][uptime:u32][freeRam:u32][moduleData:20]
            // Module data: [flags:u8][fanSpeed:u8][fanOffMs:u16][servo0-2:u16x3]
            //              [rpm:u16][shots:u32][heaterMs:u32]
            if (len >= 32) {
                // Skip core header (12 bytes), parse module data at offset 12
                const uint8_t* mod = &payload[12];
                uint8_t flags = mod[0];
                _lastStatus.firing = (flags & 0x01) != 0;
                _lastStatus.flashActive = (flags & 0x02) != 0;
                _lastStatus.flashFading = (flags & 0x04) != 0;
                _lastStatus.heaterOn = (flags & 0x08) != 0;
                _lastStatus.fanOn = (flags & 0x10) != 0;
                _lastStatus.fanSpindown = (flags & 0x20) != 0;
                
                _lastStatus.fanSpeed = mod[1];
                _lastStatus.fanOffRemainingMs = getU16LE(&mod[2]);
                _lastStatus.servoUs[0] = getU16LE(&mod[4]);
                _lastStatus.servoUs[1] = getU16LE(&mod[6]);
                _lastStatus.servoUs[2] = getU16LE(&mod[8]);
                _lastStatus.rateOfFireRpm = getU16LE(&mod[10]);
                _lastStatus.shotsFired = getU32LE(&mod[12]);
                _lastStatus.heaterOnTimeMs = getU32LE(&mod[16]);
                
                // Core header fields
                _lastStatus.uptimeMs = getU32LE(&payload[4]);
                _lastStatus.freeRam = getU32LE(&payload[8]);
            } else if (len >= 12) {
                // Core-only status (no module data)
                _lastStatus.uptimeMs = getU32LE(&payload[4]);
                _lastStatus.freeRam = getU32LE(&payload[8]);
            }
            
            if (tag != TAG_ASYNC) {
                _resultQueue.resolve(tag, CommandResult::Ack());
            }
            if (_statusCallback) _statusCallback(_lastStatus);
            break;

        default:
            break;
    }
}

// ============================================================================
// GunFxClient - Trigger Control
// ============================================================================

CommandResult GunFxClient::triggerOn(uint16_t rpm) {
    uint8_t payload[2];
    putU16LE(payload, rpm);
    return sendCommand(GunFxPacket::TRIGGER_ON, payload, sizeof(payload));
}

CommandResult GunFxClient::triggerOff(uint16_t fanDelayMs) {
    uint8_t payload[2];
    putU16LE(payload, fanDelayMs);
    return sendCommand(GunFxPacket::TRIGGER_OFF, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Servo Control
// ============================================================================

CommandResult GunFxClient::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = servoId;
    putU16LE(&payload[1], pulseUs);
    return sendCommand(GunFxPacket::SRV_SET, payload, sizeof(payload));
}

CommandResult GunFxClient::setServoConfig(const GunFxServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    putU16LE(&payload[1], config.minUs);
    putU16LE(&payload[3], config.maxUs);
    putU16LE(&payload[5], config.maxSpeedUsPerSec);
    putU16LE(&payload[7], config.maxAccelUsPerSec2);
    putU16LE(&payload[9], config.maxDecelUsPerSec2);
    return sendCommand(GunFxPacket::SRV_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxClient::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    uint8_t payload[5];
    payload[0] = servoId;
    putU16LE(&payload[1], jerkUs);
    putU16LE(&payload[3], varianceUs);
    return sendCommand(GunFxPacket::SRV_RECOIL_JERK, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Smoke Control
// ============================================================================

CommandResult GunFxClient::setSmokeHeater(bool on) {
    uint8_t payload[1] = { on ? (uint8_t)1 : (uint8_t)0 };
    return sendCommand(GunFxPacket::SMOKE_HEAT, payload, sizeof(payload));
}

CommandResult GunFxClient::setSmokeSettings(const GunFxSmokeConfig& config) {
    uint8_t payload[8];
    payload[0] = config.fanPulsing ? 1 : 0;
    payload[1] = config.fanSpeed;
    payload[2] = config.fanPulseHigh;
    payload[3] = config.fanPulseLow;
    putU16LE(&payload[4], config.fanPulseMs);
    putU16LE(&payload[6], config.fanSpindownMs);
    return sendCommand(GunFxPacket::SMOKE_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxClient::smokeReset() {
    return sendCommand(GunFxPacket::SMOKE_RESET, nullptr, 0);
}

CommandResult GunFxClient::smokeCurrentLimit(uint8_t channel, uint16_t limit_mA) {
    uint8_t payload[3];
    payload[0] = channel;
    putU16LE(&payload[1], limit_mA);
    return sendCommand(GunFxPacket::SMOKE_CURRENT_LIMIT, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Status
// ============================================================================

CommandResult GunFxClient::requestStatus() {
    return sendCommand(CorePacket::STATUS_REQ, nullptr, 0);
}

// ============================================================================
// GunFxServer - Module Packet Handler
// ============================================================================

CommandHandleResult GunFxServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case GunFxPacket::TRIGGER_ON: {
            SFX_REQUIRE_LEN(2);
            uint16_t rpm = getU16LE(payload);
            SFX_VALIDATE(GunFxSpec::isValidRpm(rpm), GunFxError::INVALID_RPM);
            SFX_DISPATCH(_triggerOnCallback, rpm);
        }

        case GunFxPacket::TRIGGER_OFF: {
            SFX_REQUIRE_LEN(2);
            uint16_t fanDelayMs = getU16LE(payload);
            SFX_DISPATCH(_triggerOffCallback, fanDelayMs);
        }

        case GunFxPacket::SRV_SET: {
            SFX_REQUIRE_LEN(3);
            uint8_t servoId = payload[0];
            uint16_t pulseUs = getU16LE(&payload[1]);
            SFX_VALIDATE(GunFxSpec::isValidServoId(servoId), GunFxError::SERVO_INVALID_ID);
            SFX_VALIDATE(GunFxSpec::isValidServoPulse(pulseUs), GunFxError::SERVO_PULSE_RANGE);
            SFX_DISPATCH(_servoSetCallback, servoId, pulseUs);
        }

        case GunFxPacket::SRV_SETTINGS: {
            SFX_REQUIRE_LEN(11);
            GunFxServoConfig config;
            config.servoId = payload[0];
            config.minUs = getU16LE(&payload[1]);
            config.maxUs = getU16LE(&payload[3]);
            config.maxSpeedUsPerSec = getU16LE(&payload[5]);
            config.maxAccelUsPerSec2 = getU16LE(&payload[7]);
            config.maxDecelUsPerSec2 = getU16LE(&payload[9]);
            SFX_VALIDATE(GunFxSpec::isValidServoId(config.servoId), GunFxError::SERVO_INVALID_ID);
            SFX_VALIDATE(GunFxSpec::isValidServoPulse(config.minUs) &&
                         GunFxSpec::isValidServoPulse(config.maxUs), GunFxError::SERVO_PULSE_RANGE);
            SFX_VALIDATE(config.minUs < config.maxUs, GunFxError::SERVO_MIN_MAX);
            SFX_DISPATCH(_servoSettingsCallback, config);
        }

        case GunFxPacket::SRV_RECOIL_JERK: {
            SFX_REQUIRE_LEN(5);
            uint8_t servoId = payload[0];
            uint16_t jerkUs = getU16LE(&payload[1]);
            uint16_t varianceUs = getU16LE(&payload[3]);
            SFX_VALIDATE(GunFxSpec::isValidServoId(servoId), GunFxError::SERVO_INVALID_ID);
            SFX_DISPATCH(_recoilJerkCallback, servoId, jerkUs, varianceUs);
        }

        case GunFxPacket::SMOKE_HEAT: {
            SFX_REQUIRE_LEN(1);
            bool on = payload[0] != 0;
            SFX_DISPATCH(_smokeHeatCallback, on);
        }

        case GunFxPacket::SMOKE_SETTINGS: {
            SFX_REQUIRE_LEN(8);
            GunFxSmokeConfig config;
            config.fanPulsing = payload[0] != 0;
            config.fanSpeed = payload[1];
            config.fanPulseHigh = payload[2];
            config.fanPulseLow = payload[3];
            config.fanPulseMs = getU16LE(&payload[4]);
            config.fanSpindownMs = getU16LE(&payload[6]);
            SFX_DISPATCH(_smokeSettingsCallback, config);
        }

        case GunFxPacket::SMOKE_RESET: {
            SFX_DISPATCH(_smokeResetCallback);
        }

        case GunFxPacket::SMOKE_CURRENT_LIMIT: {
            SFX_REQUIRE_LEN(3);
            uint8_t channel = payload[0];
            uint16_t limit_mA = getU16LE(&payload[1]);
            SFX_VALIDATE(channel <= 1, SerialError::INVALID_PARAM);
            SFX_DISPATCH(_smokeCurrentLimitCallback, channel, limit_mA);
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// GunFxServer - Response Methods
// ============================================================================

int GunFxServer::sendStatus(const GunFxStatus& status) {
    uint8_t payload[28];
    
    uint8_t flags = 0;
    if (status.firing) flags |= 0x01;
    if (status.flashActive) flags |= 0x02;
    if (status.flashFading) flags |= 0x04;
    if (status.heaterOn) flags |= 0x08;
    if (status.fanOn) flags |= 0x10;
    if (status.fanSpindown) flags |= 0x20;
    
    payload[0] = flags;
    payload[1] = status.fanSpeed;
    putU16LE(&payload[2], status.fanOffRemainingMs);
    putU16LE(&payload[4], status.servoUs[0]);
    putU16LE(&payload[6], status.servoUs[1]);
    putU16LE(&payload[8], status.servoUs[2]);
    putU16LE(&payload[10], status.rateOfFireRpm);
    putU32LE(&payload[12], status.shotsFired);
    putU32LE(&payload[16], status.heaterOnTimeMs);
    putU32LE(&payload[20], status.uptimeMs);
    putU32LE(&payload[24], status.freeRam);
    
    return sendRawPacket(CorePacket::STATUS, _currentTag, payload, sizeof(payload));
}
