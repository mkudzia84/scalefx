/*
 * LightFX Client — Protocol Implementation
 *
 * LightFxClient command methods and response parsing.
 */

#include "lightfx_client.h"

using namespace CoreProtocol;

// ============================================================================
// LightFxClient - Module Packet Handler
// ============================================================================

void LightFxClient::onModulePacket(uint8_t type, uint8_t tag, const uint8_t* payload, size_t len) {
    switch (type) {
        case LightFxPacket::LED_SEQ_STATUS_RESP:
            if (len >= 8) {
                LightFxSeqStatus status;
                status.channel = payload[0];
                status.playing = payload[1] != 0;
                status.eventCount = payload[2];
                status.currentIndex = payload[3];
                status.loopCount = getU32LE(&payload[4]);
                if (_seqStatusCallback) _seqStatusCallback(status);
            }
            // Treat response data as implicit ACK for tag correlation
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            break;

        case LightFxPacket::LED_STATUS_RESP:
            // Parse channel status: [ch:u8][brightness:u8][seq_playing:u8][events:u8] per channel
            for (size_t i = 0; i + 4 <= len; i += 4) {
                LightFxChannelStatus status;
                status.channel = payload[i];
                status.brightness = payload[i + 1];
                status.seqPlaying = payload[i + 2] != 0;
                status.seqEventCount = payload[i + 3];
                if (_channelStatusCallback) _channelStatusCallback(status);
            }
            // Treat response data as implicit ACK for tag correlation
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            break;

        case LightFxPacket::LED_SEQ_QUEUE_RESP:
            if (len >= 4) {
                LightFxSeqQueue queue;
                queue.channel = payload[0];
                queue.count = payload[1];
                queue.currentIndex = payload[2];
                queue.playing = payload[3] != 0;
                // Parse events: [type:u8][duration:u16LE][param1:u8] = 4 bytes each
                uint8_t eventCount = (queue.count <= 24) ? queue.count : 24;
                for (uint8_t i = 0; i < eventCount && (4 + i * 4 + 4) <= len; i++) {
                    size_t offset = 4 + (i * 4);
                    queue.events[i].type = payload[offset];
                    queue.events[i].duration = getU16LE(&payload[offset + 1]);
                    queue.events[i].param1 = payload[offset + 3];
                }
                if (_seqQueueCallback) _seqQueueCallback(queue);
            }
            // Treat response data as implicit ACK for tag correlation
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            break;

        case CorePacket::STATUS:
            // Core STATUS response — resolve tag for blocking calls
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            break;

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
// LightFxClient - LED Direct Control
// ============================================================================

CommandResult LightFxClient::ledSet(uint8_t channel, uint8_t brightness) {
    uint8_t payload[2] = { channel, brightness };
    return sendCommand(LightFxPacket::LED_SET, payload, sizeof(payload));
}

CommandResult LightFxClient::ledOff(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_OFF, payload, sizeof(payload));
}

// ============================================================================
// LightFxClient - LED Sequence Control
// ============================================================================

CommandResult LightFxClient::ledSeqClear(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_CLEAR, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqAddOn(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::ON;
    putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqAddOff(uint8_t channel, uint16_t durationMs) {
    uint8_t payload[4];
    payload[0] = channel;
    payload[1] = LightFxEventType::OFF;
    putU16LE(&payload[2], durationMs);
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint16_t durationMs,
                                   uint8_t brightness, uint8_t dutyPercent) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FLASH;
    putU16LE(&payload[2], intervalMs);
    putU16LE(&payload[4], durationMs);
    payload[6] = brightness;
    payload[7] = dutyPercent;
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_IN;
    putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_OUT;
    putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqAddFading(uint8_t channel, uint16_t cycleMs, uint16_t durationMs,
                                    uint8_t minBrightness, uint8_t maxBrightness) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADING;
    putU16LE(&payload[2], cycleMs);
    putU16LE(&payload[4], durationMs);
    payload[6] = minBrightness;
    payload[7] = maxBrightness;
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqStart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_START, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqStop(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_STOP, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqRestart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_RESTART, payload, sizeof(payload));
}

CommandResult LightFxClient::ledSeqStatus(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_STATUS, payload, sizeof(payload));
}

CommandResult LightFxClient::ledStatus() {
    return sendCommand(LightFxPacket::LED_STATUS, nullptr, 0);
}

CommandResult LightFxClient::ledMasterBrightness(uint8_t pct) {
    uint8_t payload[1] = { pct };
    return sendCommand(LightFxPacket::LED_MASTER_BRIGHTNESS, payload, sizeof(payload));
}

// ============================================================================
// LightFxClient - Channel Management
// ============================================================================

CommandResult LightFxClient::ledReset(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_RESET, payload, sizeof(payload));
}

CommandResult LightFxClient::ledEnable(uint8_t channel, bool enabled) {
    uint8_t payload[2] = { channel, (uint8_t)(enabled ? 1 : 0) };
    return sendCommand(LightFxPacket::LED_ENABLE, payload, sizeof(payload));
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

// ============================================================================
// LightFxClient - Status
// ============================================================================

CommandResult LightFxClient::requestStatus() {
    return sendCommand(CorePacket::STATUS_REQ, nullptr, 0);
}
