/*
 * LightFX Server — Protocol Implementation
 *
 * LightFxServer servo/landing light command dispatch and response methods.
 * LED commands are handled by the base class LedProtocolServer.
 */

#include "lightfx_server.h"

using namespace CoreProtocol;

// ============================================================================
// LightFxServer - Module Packet Handler
// ============================================================================

CommandHandleResult LightFxServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {

    // Delegate LED commands (0x40-0x4C) to base class first
    auto result = LedProtocolServer::handleModulePacket(type, payload, len);
    if (result != CommandHandleResult::NotMyCommand) return result;

    // Handle LightFX-specific commands (servos + landing lights)
    switch (type) {
        case LightFxPacket::SERVO_SET: {
            SFX_REQUIRE_LEN(3);
            uint8_t id = payload[0];
            int16_t pulseUs = getI16LE(&payload[1]);
            SFX_VALIDATE(LightFxSpec::isValidServoId(id), LightFxError::INVALID_SERVO);
            SFX_VALIDATE(LightFxSpec::isValidServoPulse(pulseUs), SerialError::PARAM_OUT_OF_RANGE);
            SFX_DISPATCH(_servoSetCallback, id, pulseUs);
        }

        case LightFxPacket::SERVO_SETTINGS: {
            SFX_REQUIRE_LEN(11);
            LightFxServoConfig config;
            config.servoId = payload[0];
            config.minUs = getU16LE(&payload[1]);
            config.maxUs = getU16LE(&payload[3]);
            config.maxSpeedUsPerSec = getU16LE(&payload[5]);
            config.maxAccelUsPerSec2 = getU16LE(&payload[7]);
            config.maxDecelUsPerSec2 = getU16LE(&payload[9]);
            config.reversed = (len >= 12) ? (payload[11] != 0) : false;  // optional byte 12
            SFX_VALIDATE(LightFxSpec::isValidServoId(config.servoId), LightFxError::INVALID_SERVO);
            SFX_VALIDATE(config.minUs >= LightFxSpec::SERVO_PULSE_MIN && config.minUs <= LightFxSpec::SERVO_PULSE_MAX &&
                         config.maxUs >= LightFxSpec::SERVO_PULSE_MIN && config.maxUs <= LightFxSpec::SERVO_PULSE_MAX,
                         SerialError::PARAM_OUT_OF_RANGE);
            SFX_VALIDATE(config.minUs < config.maxUs, LightFxError::INVALID_PARAM);
            SFX_DISPATCH(_servoSettingsCallback, config);
        }

        // Landing light control
        case LightFxPacket::LANDING_LIGHT_BIND: {
            SFX_REQUIRE_LEN(4);
            uint8_t slot = payload[0];
            uint8_t servoId = payload[1];       // 0=no servo, 1-3=servo id
            uint8_t channelMask = payload[2];   // bitmask: bit0=ch1 .. bit7=ch8
            uint8_t brightness = payload[3];
            SFX_VALIDATE(LightFxSpec::isValidLandingLightSlot(slot), LightFxError::INVALID_SLOT);
            SFX_VALIDATE(LightFxSpec::isValidLandingServoId(servoId), LightFxError::INVALID_SERVO);
            SFX_VALIDATE(LightFxSpec::isValidChannelMask(channelMask), LightFxError::INVALID_CHANNEL);
            SFX_DISPATCH(_landingLightBindCallback, slot, servoId, channelMask, brightness);
        }

        case LightFxPacket::LANDING_LIGHT_UNBIND: {
            SFX_REQUIRE_LEN(1);
            uint8_t slot = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidLandingLightSlotOrAll(slot), LightFxError::INVALID_SLOT);
            SFX_DISPATCH(_landingLightUnbindCallback, slot);
        }

        case LightFxPacket::LANDING_LIGHT_DEPLOY: {
            SFX_REQUIRE_LEN(1);
            uint8_t slot = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidLandingLightSlotOrAll(slot), LightFxError::INVALID_SLOT);
            // Store tag for landing light progress responses
            if (slot == 0) {
                for (int i = 0; i < 3; i++) _landingLightTag[i] = _currentTag;
            } else if (slot <= 3) {
                _landingLightTag[slot - 1] = _currentTag;
            }
            SFX_DISPATCH(_landingLightDeployCallback, slot);
        }

        case LightFxPacket::LANDING_LIGHT_RETRACT: {
            SFX_REQUIRE_LEN(1);
            uint8_t slot = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidLandingLightSlotOrAll(slot), LightFxError::INVALID_SLOT);
            // Store tag for landing light progress responses
            if (slot == 0) {
                for (int i = 0; i < 3; i++) _landingLightTag[i] = _currentTag;
            } else if (slot <= 3) {
                _landingLightTag[slot - 1] = _currentTag;
            }
            SFX_DISPATCH(_landingLightRetractCallback, slot);
        }

        case LightFxPacket::BATTERY_AUTO_CUTOFF: {
            SFX_REQUIRE_LEN(1);
            bool enabled = payload[0] != 0;
            SFX_DISPATCH(_batteryAutoCutoffCallback, enabled);
        }

        case LightFxPacket::LIGHT_PROGRAM_SELECT: {
            SFX_REQUIRE_LEN(1);
            uint8_t programIndex = payload[0];
            SFX_DISPATCH(_lightProgramSelectCallback, programIndex);
        }

        case LightFxPacket::LIGHT_PROGRAM_RESET: {
            // No payload — best-effort callback dispatches handled below
            SFX_DISPATCH(_lightProgramResetCallback);
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// LightFxServer - Response Methods
// ============================================================================

int LightFxServer::sendLandingLightStatus(const LightFxLandingLightStatus& status) {
    uint8_t payload[3];
    payload[0] = status.slot;
    payload[1] = status.phase;
    payload[2] = status.finished ? 1 : 0;
    uint8_t tag = (status.slot >= 1 && status.slot <= 3) ? _landingLightTag[status.slot - 1] : 0;
    return sendRawPacket(LightFxPacket::LANDING_LIGHT_STATUS, tag, payload, sizeof(payload));
}
