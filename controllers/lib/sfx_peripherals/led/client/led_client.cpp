/*
 * LED Protocol Client — Implementation
 *
 * LedProtocolClient command methods and response parsing.
 */

#include "led_client.h"

using namespace CoreProtocol;

// ============================================================================
// LedProtocolClient - Module Packet Handler
// ============================================================================

void LedProtocolClient::onModulePacket(uint8_t type, uint8_t tag,
                                        const uint8_t* payload, size_t len) {
    // Default implementation — handle LED responses only
    handleLedResponse(type, tag, payload, len);
}

bool LedProtocolClient::handleLedResponse(uint8_t type, uint8_t tag,
                                           const uint8_t* payload, size_t len) {
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
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            return true;

        case LightFxPacket::LED_STATUS_RESP:
            for (size_t i = 0; i + 4 <= len; i += 4) {
                LightFxChannelStatus status;
                status.channel = payload[i];
                status.brightness = payload[i + 1];
                status.seqPlaying = payload[i + 2] != 0;
                status.seqEventCount = payload[i + 3];
                if (_channelStatusCallback) _channelStatusCallback(status);
            }
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            return true;

        case LightFxPacket::LED_SEQ_QUEUE_RESP:
            if (len >= 4) {
                LightFxSeqQueue queue;
                queue.channel = payload[0];
                queue.count = payload[1];
                queue.currentIndex = payload[2];
                queue.playing = payload[3] != 0;
                uint8_t eventCount = (queue.count <= 24) ? queue.count : 24;
                for (uint8_t i = 0; i < eventCount && (4 + i * 4 + 4) <= len; i++) {
                    size_t offset = 4 + (i * 4);
                    queue.events[i].type = payload[offset];
                    queue.events[i].duration = getU16LE(&payload[offset + 1]);
                    queue.events[i].param1 = payload[offset + 3];
                }
                if (_seqQueueCallback) _seqQueueCallback(queue);
            }
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            return true;

        case CorePacket::STATUS:
            if (tag != CoreProtocol::TAG_ASYNC) {
                _lastCommandResult = CommandResult::Ack();
                _resultQueue.resolve(tag, _lastCommandResult);
            }
            return true;

        default:
            return false;
    }
}

// ============================================================================
// LedProtocolClient - LED Direct Control
// ============================================================================

CommandResult LedProtocolClient::ledSet(uint8_t channel, uint8_t brightness) {
    uint8_t payload[2] = { channel, brightness };
    return sendCommand(LightFxPacket::LED_SET, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledOff(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_OFF, payload, sizeof(payload));
}

// ============================================================================
// LedProtocolClient - LED Sequence Control
// ============================================================================

CommandResult LedProtocolClient::ledSeqClear(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_CLEAR, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqAddOn(uint8_t channel, uint16_t durationMs,
                                              uint8_t brightness, uint8_t pwmDuty) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::ON;
    putU16LE(&payload[2], durationMs);          // p1 = duration
    putU16LE(&payload[4], (uint16_t)pwmDuty);   // p2 = pwmDuty (0=no power save, 1-100=duty%)
    payload[6] = brightness;                     // p3 = brightness
    payload[7] = 0;                              // p4 = reserved
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqAddOff(uint8_t channel, uint16_t durationMs) {
    uint8_t payload[4];
    payload[0] = channel;
    payload[1] = LightFxEventType::OFF;
    putU16LE(&payload[2], durationMs);
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqAddFlash(uint8_t channel, uint16_t intervalMs,
                                                 uint16_t durationMs,
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

CommandResult LedProtocolClient::ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs,
                                                  uint8_t brightness) {
    uint8_t payload[7];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_IN;
    putU16LE(&payload[2], durationMs);          // p1 = duration
    putU16LE(&payload[4], 0);                   // p2 = unused
    payload[6] = brightness;                     // p3 = brightness
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs,
                                                   uint8_t brightness) {
    uint8_t payload[7];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_OUT;
    putU16LE(&payload[2], durationMs);          // p1 = duration
    putU16LE(&payload[4], 0);                   // p2 = unused
    payload[6] = brightness;                     // p3 = brightness
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqAddFading(uint8_t channel, uint16_t cycleMs,
                                                  uint16_t durationMs,
                                                  uint8_t minBrightness,
                                                  uint8_t maxBrightness) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADING;
    putU16LE(&payload[2], cycleMs);             // p1 = cycle
    putU16LE(&payload[4], durationMs);          // p2 = duration
    payload[6] = minBrightness;                  // p3 = min
    payload[7] = maxBrightness;                  // p4 = max
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqAddBeacon(uint8_t channel, uint16_t cycleMs,
                                                  uint16_t durationMs,
                                                  uint8_t flashPercent,
                                                  uint8_t maxBrightness,
                                                  uint8_t minBrightness) {
    uint8_t payload[9];
    payload[0] = channel;
    payload[1] = LightFxEventType::BEACON;
    putU16LE(&payload[2], cycleMs);             // p1 = cycle
    putU16LE(&payload[4], durationMs);          // p2 = duration
    payload[6] = flashPercent;                   // p3 = flash percent
    payload[7] = maxBrightness;                  // p4 = max brightness
    payload[8] = minBrightness;                  // p5 = min brightness
    return sendCommand(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqStart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_START, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqStop(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_STOP, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqRestart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_RESTART, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledSeqStatus(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_SEQ_STATUS, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledStatus() {
    return sendCommand(LightFxPacket::LED_STATUS, nullptr, 0);
}

CommandResult LedProtocolClient::ledMasterBrightness(uint8_t pct) {
    uint8_t payload[1] = { pct };
    return sendCommand(LightFxPacket::LED_MASTER_BRIGHTNESS, payload, sizeof(payload));
}

// ============================================================================
// LedProtocolClient - Channel Management
// ============================================================================

CommandResult LedProtocolClient::ledReset(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendCommand(LightFxPacket::LED_RESET, payload, sizeof(payload));
}

CommandResult LedProtocolClient::ledEnable(uint8_t channel, bool enabled) {
    uint8_t payload[2] = { channel, (uint8_t)(enabled ? 1 : 0) };
    return sendCommand(LightFxPacket::LED_ENABLE, payload, sizeof(payload));
}

// ============================================================================
// LedProtocolClient - Status
// ============================================================================

CommandResult LedProtocolClient::requestStatus() {
    return sendCommand(CorePacket::STATUS_REQ, nullptr, 0);
}
