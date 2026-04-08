/*
 * LED Protocol Server — Implementation
 *
 * LedProtocolServer command dispatch and response methods.
 */

#include "led_server.h"
#include "../led_manager.h"

using namespace CoreProtocol;

// ============================================================================
// LedProtocolServer - Module Packet Handler
// ============================================================================

CommandHandleResult LedProtocolServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {

    // All commands require a manager
    if (!_ledManager) {
        sendNack(SerialError::NOT_SUPPORTED);
        return CommandHandleResult::Handled;
    }

    switch (type) {
        case LightFxPacket::LED_SET: {
            SFX_REQUIRE_LEN(2);
            uint8_t channel = payload[0];
            uint8_t brightness = payload[1];
            SFX_VALIDATE(_ledManager->isValidChannel(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->ledSet(channel, brightness);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_OFF: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->ledOff(channel);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_CLEAR: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->seqClear(channel);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_ADD: {
            SFX_REQUIRE_LEN(4);
            uint8_t channel   = payload[0];
            uint8_t eventType = payload[1];
            uint16_t param1   = getU16LE(&payload[2]);
            uint16_t param2   = (len >= 6) ? getU16LE(&payload[4]) : 0;
            uint8_t  param3   = (len >= 7) ? payload[6] : 100;
            uint8_t  param4   = (len >= 8) ? payload[7] : 50;
            uint8_t  param5   = (len >= 9) ? payload[8] : 0;   // optional 5th param (e.g., BEACON minBrightness)
            SFX_VALIDATE(_ledManager->isValidChannel(channel), LightFxError::INVALID_CHANNEL);
            SFX_VALIDATE(LightFxSpec::isValidEventType(eventType), LightFxError::INVALID_EVENT);
            uint8_t err = _ledManager->seqAdd(channel, eventType, param1, param2, param3, param4, param5);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_START: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->seqStart(channel);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_STOP: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->seqStop(channel);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_RESTART: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->seqRestart(channel);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_STATUS: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannel(channel), LightFxError::INVALID_CHANNEL);
            LightFxSeqStatus status;
            _ledManager->getSeqStatus(channel, status);
            sendSeqStatus(status);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_STATUS: {
            uint8_t count = _ledManager->channelCount();
            if (count > 16) count = 16;  // Safety cap
            LightFxChannelStatus channels[16];
            for (uint8_t i = 0; i < count; i++) {
                channels[i].channel = i + 1;
                _ledManager->getChannelStatus(i + 1, channels[i]);
            }
            sendChannelStatus(channels, count);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_SEQ_QUEUE: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannel(channel), LightFxError::INVALID_CHANNEL);
            LightFxSeqQueue queue;
            _ledManager->getSeqQueue(channel, queue);
            sendSeqQueue(queue);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_MASTER_BRIGHTNESS: {
            SFX_REQUIRE_LEN(1);
            uint8_t pct = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidMasterBrightness(pct), SerialError::PARAM_OUT_OF_RANGE);
            uint8_t err = _ledManager->setMasterBrightness(pct);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_RESET: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->resetChannel(channel);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        case LightFxPacket::LED_ENABLE: {
            SFX_REQUIRE_LEN(2);
            uint8_t channel = payload[0];
            uint8_t enabled = payload[1];
            SFX_VALIDATE(_ledManager->isValidChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            uint8_t err = _ledManager->enableChannel(channel, enabled != 0);
            if (err == 0) sendAck(); else sendNack(err);
            return CommandHandleResult::Handled;
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// LedProtocolServer - Response Methods
// ============================================================================

int LedProtocolServer::sendSeqStatus(const LightFxSeqStatus& status) {
    uint8_t payload[9];
    payload[0] = status.channel;
    payload[1] = status.playing ? 1 : 0;
    payload[2] = status.eventCount;
    payload[3] = status.currentIndex;
    putU32LE(&payload[4], status.loopCount);
    payload[8] = status.brightness;
    return sendRawPacket(LightFxPacket::LED_SEQ_STATUS_RESP, _currentTag, payload, sizeof(payload));
}

int LedProtocolServer::sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count) {
    uint8_t payload[64];  // 4 bytes per channel, max 16 channels
    size_t len = 0;

    for (uint8_t i = 0; i < count && len + 4 <= sizeof(payload); i++) {
        payload[len++] = channels[i].channel;
        payload[len++] = channels[i].brightness;
        payload[len++] = channels[i].seqPlaying ? 1 : 0;
        payload[len++] = channels[i].seqEventCount;
    }

    return sendRawPacket(LightFxPacket::LED_STATUS_RESP, _currentTag, payload, len);
}

int LedProtocolServer::sendSeqQueue(const LightFxSeqQueue& queue) {
    // Response: [channel:u8][count:u8][currentIndex:u8][playing:u8][brightness:u8][events...]
    // Each event: [type:u8][duration:u16LE][param1:u8] = 4 bytes
    constexpr uint8_t MAX_EVENTS_PER_PACKET = 15;

    uint8_t eventCount = (queue.count <= MAX_EVENTS_PER_PACKET) ? queue.count : MAX_EVENTS_PER_PACKET;
    size_t payloadLen = 5 + (eventCount * 4);

    uint8_t payload[68];
    payload[0] = queue.channel;
    payload[1] = queue.count;           // Total events (may be more than sent)
    payload[2] = queue.currentIndex;
    payload[3] = queue.playing ? 1 : 0;
    payload[4] = queue.brightness;

    for (uint8_t i = 0; i < eventCount; i++) {
        size_t offset = 5 + (i * 4);
        payload[offset + 0] = queue.events[i].type;
        putU16LE(&payload[offset + 1], queue.events[i].duration);
        payload[offset + 3] = queue.events[i].param1;
    }

    return sendRawPacket(LightFxPacket::LED_SEQ_QUEUE_RESP, _currentTag, payload, payloadLen);
}
