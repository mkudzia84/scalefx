/*
 * GunFX Server — Protocol Implementation
 *
 * GunFxServer command dispatch and response methods.
 */

#include "gunfx_server.h"

using namespace CoreProtocol;

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
