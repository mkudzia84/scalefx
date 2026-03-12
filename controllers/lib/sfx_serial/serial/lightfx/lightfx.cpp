/*
 * Serial LightFX Protocol - Implementation
 *
 * LightFX-specific client/server protocol implementation.
 *   - LightFxClient: Module-specific commands and response parsing
 *   - LightFxServer: Module-specific command dispatch and response methods
 */

#include "lightfx.h"

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

// ============================================================================
// LightFxServer - Module Packet Handler
// ============================================================================

CommandHandleResult LightFxServer::handleModulePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case LightFxPacket::LED_SET: {
            SFX_REQUIRE_LEN(2);
            uint8_t channel = payload[0];
            uint8_t brightness = payload[1];
            SFX_VALIDATE(LightFxSpec::isValidLedChannel(channel), LightFxError::INVALID_CHANNEL);
            SFX_DISPATCH(_ledSetCallback, channel, brightness);
        }

        case LightFxPacket::LED_OFF: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidLedChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            SFX_DISPATCH(_ledOffCallback, channel);
        }

        case LightFxPacket::LED_SEQ_CLEAR:
            SFX_HANDLE_CHANNEL_CMD(LightFxSpec::isValidLedChannel, LightFxError::INVALID_CHANNEL, _ledSeqClearCallback);

        case LightFxPacket::LED_SEQ_ADD: {
            SFX_REQUIRE_LEN(4);
            uint8_t channel = payload[0];
            uint8_t eventType = payload[1];
            uint16_t param1 = getU16LE(&payload[2]);
            uint16_t param2 = (len >= 6) ? getU16LE(&payload[4]) : 0;
            uint8_t param3 = (len >= 7) ? payload[6] : 255;
            uint8_t param4 = (len >= 8) ? payload[7] : 50;
            SFX_VALIDATE(LightFxSpec::isValidLedChannel(channel), LightFxError::INVALID_CHANNEL);
            SFX_VALIDATE(LightFxSpec::isValidEventType(eventType), LightFxError::INVALID_EVENT);
            SFX_DISPATCH(_ledSeqAddCallback, channel, eventType, param1, param2, param3, param4);
        }

        case LightFxPacket::LED_SEQ_START: {
            SFX_REQUIRE_LEN(1);
            SFX_DISPATCH(_ledSeqStartCallback, payload[0]);
        }

        case LightFxPacket::LED_SEQ_STOP: {
            SFX_REQUIRE_LEN(1);
            SFX_DISPATCH(_ledSeqStopCallback, payload[0]);
        }

        case LightFxPacket::LED_SEQ_RESTART: {
            SFX_REQUIRE_LEN(1);
            SFX_DISPATCH(_ledSeqRestartCallback, payload[0]);
        }

        case LightFxPacket::LED_SEQ_STATUS:
            if (len >= 1 && _ledSeqStatusCallback) {
                uint8_t channel = payload[0];
                LightFxSeqStatus status;
                status.channel = channel;
                _ledSeqStatusCallback(channel, status);
                sendSeqStatus(status);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_SEQ_QUEUE:
            if (len >= 1 && _ledSeqQueueCallback) {
                uint8_t channel = payload[0];
                LightFxSeqQueue queue;
                queue.channel = channel;
                _ledSeqQueueCallback(channel, queue);
                sendSeqQueue(queue);
            } else if (len < 1) {
                sendNack(SerialError::MISSING_PARAMETER);
            } else {
                sendNack(SerialError::NOT_SUPPORTED);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_STATUS:
            if (_ledStatusCallback) {
                LightFxChannelStatus channels[8];
                for (uint8_t i = 0; i < 8; i++) {
                    channels[i].channel = i + 1;
                    _ledStatusCallback(i + 1, channels[i]);
                }
                sendChannelStatus(channels, 8);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::LED_MASTER_BRIGHTNESS: {
            SFX_REQUIRE_LEN(1);
            uint8_t pct = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidMasterBrightness(pct), SerialError::PARAM_OUT_OF_RANGE);
            SFX_DISPATCH(_ledMasterBrightnessCallback, pct);
        }

        case LightFxPacket::LED_RESET: {
            SFX_REQUIRE_LEN(1);
            uint8_t channel = payload[0];
            SFX_VALIDATE(LightFxSpec::isValidLedChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            SFX_DISPATCH(_ledResetCallback, channel);
        }

        case LightFxPacket::LED_ENABLE: {
            SFX_REQUIRE_LEN(2);
            uint8_t channel = payload[0];
            uint8_t enabled = payload[1];
            SFX_VALIDATE(LightFxSpec::isValidLedChannelOrAll(channel), LightFxError::INVALID_CHANNEL);
            SFX_DISPATCH(_ledEnableCallback, channel, enabled);
        }

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
            uint8_t id = payload[0];
            int minUs = getU16LE(&payload[1]);
            int maxUs = getU16LE(&payload[3]);
            int speed = getU16LE(&payload[5]);
            int accel = getU16LE(&payload[7]);
            int decel = getU16LE(&payload[9]);
            SFX_VALIDATE(LightFxSpec::isValidServoId(id), LightFxError::INVALID_SERVO);
            SFX_VALIDATE(minUs >= LightFxSpec::SERVO_PULSE_MIN && minUs <= LightFxSpec::SERVO_PULSE_MAX &&
                         maxUs >= LightFxSpec::SERVO_PULSE_MIN && maxUs <= LightFxSpec::SERVO_PULSE_MAX,
                         SerialError::PARAM_OUT_OF_RANGE);
            SFX_VALIDATE(minUs < maxUs, LightFxError::INVALID_PARAM);
            SFX_DISPATCH(_servoSettingsCallback, id, minUs, maxUs, speed, accel, decel);
        }

        // Landing light control
        case LightFxPacket::LANDING_LIGHT_BIND: {
            SFX_REQUIRE_LEN(8);
            uint8_t slot = payload[0];
            uint8_t servoId = payload[1];
            uint8_t ledChannel = payload[2];
            uint16_t deployUs = getU16LE(&payload[3]);
            uint16_t retractUs = getU16LE(&payload[5]);
            uint8_t brightness = payload[7];
            SFX_VALIDATE(LightFxSpec::isValidLandingLightSlot(slot), LightFxError::INVALID_SLOT);
            SFX_VALIDATE(LightFxSpec::isValidServoId(servoId), LightFxError::INVALID_SERVO);
            SFX_VALIDATE(LightFxSpec::isValidLedChannel(ledChannel), LightFxError::INVALID_CHANNEL);
            SFX_DISPATCH(_landingLightBindCallback, slot, servoId, ledChannel, deployUs, retractUs, brightness);
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

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// LightFxServer - Response Methods
// ============================================================================

int LightFxServer::sendSeqStatus(const LightFxSeqStatus& status) {
    uint8_t payload[8];
    payload[0] = status.channel;
    payload[1] = status.playing ? 1 : 0;
    payload[2] = status.eventCount;
    payload[3] = status.currentIndex;
    putU32LE(&payload[4], status.loopCount);
    return sendRawPacket(LightFxPacket::LED_SEQ_STATUS_RESP, _currentTag, payload, sizeof(payload));
}

int LightFxServer::sendChannelStatus(const LightFxChannelStatus* channels, uint8_t count) {
    uint8_t payload[32];  // 4 bytes per channel, max 8 channels
    size_t len = 0;
    
    for (uint8_t i = 0; i < count && len + 4 <= sizeof(payload); i++) {
        payload[len++] = channels[i].channel;
        payload[len++] = channels[i].brightness;
        payload[len++] = channels[i].seqPlaying ? 1 : 0;
        payload[len++] = channels[i].seqEventCount;
    }
    
    return sendRawPacket(LightFxPacket::LED_STATUS_RESP, _currentTag, payload, len);
}

int LightFxServer::sendSeqQueue(const LightFxSeqQueue& queue) {
    // Response format: [channel:u8][count:u8][currentIndex:u8][playing:u8][events...]
    // Each event: [type:u8][duration:u16LE][param1:u8] = 4 bytes
    // Max 24 events = 96 bytes + 4 header = 100 bytes (needs chunking for large queues)
    // For now, limit to 15 events per packet (4 + 15*4 = 64 bytes)
    constexpr uint8_t MAX_EVENTS_PER_PACKET = 15;
    
    uint8_t eventCount = (queue.count <= MAX_EVENTS_PER_PACKET) ? queue.count : MAX_EVENTS_PER_PACKET;
    size_t payloadLen = 4 + (eventCount * 4);
    
    uint8_t payload[64];
    payload[0] = queue.channel;
    payload[1] = queue.count;           // Total events (may be more than sent)
    payload[2] = queue.currentIndex;
    payload[3] = queue.playing ? 1 : 0;
    
    for (uint8_t i = 0; i < eventCount; i++) {
        size_t offset = 4 + (i * 4);
        payload[offset + 0] = queue.events[i].type;
        putU16LE(&payload[offset + 1], queue.events[i].duration);
        payload[offset + 3] = queue.events[i].param1;
    }
    
    return sendRawPacket(LightFxPacket::LED_SEQ_QUEUE_RESP, _currentTag, payload, payloadLen);
}

int LightFxServer::sendLandingLightStatus(const LightFxLandingLightStatus& status) {
    uint8_t payload[3];
    payload[0] = status.slot;
    payload[1] = status.phase;
    payload[2] = status.finished ? 1 : 0;
    uint8_t tag = (status.slot >= 1 && status.slot <= 3) ? _landingLightTag[status.slot - 1] : 0;
    return sendRawPacket(LightFxPacket::LANDING_LIGHT_STATUS, tag, payload, sizeof(payload));
}
