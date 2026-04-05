/*
 * LightFX Client — Protocol Implementation
 *
 * LightFxClient servo/landing light command methods and response parsing.
 * LED commands and responses are handled by the base class LedProtocolClient.
 */

#include "lightfx_client.h"

using namespace CoreProtocol;

// ============================================================================
// LightFxClient - Module Packet Handler
// ============================================================================

void LightFxClient::onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    // Delegate LED responses to base class first
    if (handleLedResponse(type, tag, payload, len)) return;

    switch (type) {
        case LightFxPacket::LANDING_LIGHT_STATUS:
            // Landing light progress: [slot:u8][phase:u8][finished:u8]
            if (len >= 3) {
                LightFxLandingLightStatus lls;
                lls.slot = payload[0];
                lls.phase = payload[1];
                lls.finished = (payload[2] != 0);
                if (_landingLightStatusCallback) _landingLightStatusCallback(lls);

                // When operation finishes, resolve the original tag
                if (lls.finished && tag != CoreProtocol::TAG_ASYNC) {
                    _lastCommandResult = CommandResult::Ack();
                    _resultQueue.resolve(tag, _lastCommandResult);
                }
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// LightFxClient - Servo Control
// ============================================================================

CommandResult LightFxClient::servoSet(uint8_t id, int16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = id;
    putI16LE(&payload[1], pulseUs);
    return sendCommand(LightFxPacket::SERVO_SET, payload, sizeof(payload));
}

CommandResult LightFxClient::servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                                  uint16_t speed, uint16_t accel, uint16_t decel) {
    uint8_t payload[11];
    payload[0] = id;
    putU16LE(&payload[1], minUs);
    putU16LE(&payload[3], maxUs);
    putU16LE(&payload[5], speed);
    putU16LE(&payload[7], accel);
    putU16LE(&payload[9], decel);
    return sendCommand(LightFxPacket::SERVO_SETTINGS, payload, sizeof(payload));
}

// ============================================================================
// LightFxClient - Landing Light Control
// ============================================================================

CommandResult LightFxClient::landingLightBind(uint8_t slot, uint8_t servoId, uint8_t ledChannel,
                                     uint16_t deployUs, uint16_t retractUs, uint8_t brightness) {
    uint8_t payload[8];
    payload[0] = slot;
    payload[1] = servoId;
    payload[2] = ledChannel;
    putU16LE(&payload[3], deployUs);
    putU16LE(&payload[5], retractUs);
    payload[7] = brightness;
    return sendCommand(LightFxPacket::LANDING_LIGHT_BIND, payload, sizeof(payload));
}

CommandResult LightFxClient::landingLightUnbind(uint8_t slot) {
    uint8_t payload[1] = { slot };
    return sendCommand(LightFxPacket::LANDING_LIGHT_UNBIND, payload, sizeof(payload));
}

CommandResult LightFxClient::landingLightDeploy(uint8_t slot) {
    uint8_t payload[1] = { slot };
    return sendCommand(LightFxPacket::LANDING_LIGHT_DEPLOY, payload, sizeof(payload));
}

CommandResult LightFxClient::landingLightRetract(uint8_t slot) {
    uint8_t payload[1] = { slot };
    return sendCommand(LightFxPacket::LANDING_LIGHT_RETRACT, payload, sizeof(payload));
}
