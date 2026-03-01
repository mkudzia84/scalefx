/*
 * Serial LightFX Protocol - Implementation
 *
 * Binary COBS protocol client/server for LightFX controller.
 *   - LightFxClient: For HubFX (sends commands via USB)
 *   - LightFxServer: For LightFX Pico (receives commands, implements ICommandHandler)
 */

#include "serial_lightfx.h"

using namespace CoreProtocol;

// ============================================================================
// LightFxClient Implementation
// ============================================================================

bool LightFxClient::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    _usbHostRef = usbHost;
    _serverReady = false;
    _serverName[0] = '\0';
    memset(&_boardInfo, 0, sizeof(_boardInfo));

    // Set up internal packet handler
    SerialBus::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    return true;
}

int LightFxClient::process() {
    return SerialBus::process();
}

int LightFxClient::sendInit(unsigned long keepaliveMs) {
    if (!_usbHostRef) return -1;
    
    char buf[64];
    if (keepaliveMs > 0) {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=%lu", keepaliveMs);
    } else {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=off");
    }
    
    int written = _usbHostRef->cdcPrintln(SerialBus::deviceIndex(), buf);
    if (written > 0) {
        _lastSendMs = millis();
    }
    return written;
}

void LightFxClient::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case CorePacket::INIT_READY:
            _serverReady = true;
            if (len > 0) {
                char buffer[128];
                size_t bufLen = (len < sizeof(buffer) - 1) ? len : sizeof(buffer) - 1;
                memcpy(buffer, payload, bufLen);
                buffer[bufLen] = '\0';
                
                // Parse pipe-delimited: name|version|platform|cpuMHz|ramBytes
                char* name = strtok(buffer, "|");
                char* version = strtok(nullptr, "|");
                char* platform = strtok(nullptr, "|");
                char* cpuStr = strtok(nullptr, "|");
                char* ramStr = strtok(nullptr, "|");
                
                if (name) {
                    strncpy(_serverName, name, sizeof(_serverName) - 1);
                    _serverName[sizeof(_serverName) - 1] = '\0';
                    strncpy(_boardInfo.deviceName, name, sizeof(_boardInfo.deviceName) - 1);
                }
                if (version) strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
                if (platform) strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
                if (cpuStr) _boardInfo.cpuFrequencyMHz = atoi(cpuStr);
                if (ramStr) _boardInfo.freeRamBytes = atoi(ramStr);
            }
            if (_readyCallback) _readyCallback(_serverName);
            break;

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
            break;

        case LightFxPacket::POWER_STATUS_RESP:
            if (len >= 7) {
                LightFxPowerStatus status;
                status.voltage = getU16LE(&payload[0]) / 1000.0f;  // mV to V
                status.current = (float)(int16_t)getU16LE(&payload[2]);  // mA
                status.power = (float)getU16LE(&payload[4]);  // mW
                status.available = payload[6] != 0;
                if (_powerStatusCallback) _powerStatusCallback(status);
            }
            break;

        case CorePacket::ACK:
            _receivedAck = true;
            _pendingAckNack = false;
            _lastAckReceived = true;
            break;

        case CorePacket::NACK:
            _receivedNack = true;
            _pendingAckNack = false;
            _lastAckReceived = false;
            if (len >= 1) {
                _lastNackErrorCode = payload[0];
                if (_errorCallback) {
                    _errorCallback(_lastNackErrorCode, LightFxError::getMessage(_lastNackErrorCode));
                }
            }
            break;

        default:
            break;
    }
}

bool LightFxClient::sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len) {
    if (!isConnected()) return false;
    
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    
    int sent = sendPacket(type, payload, len);
    if (sent < 0) {
        _pendingAckNack = false;
        return false;
    }
    
    if (!_blockingMode) return true;
    return waitForAckNack();
}

bool LightFxClient::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        SerialBus::process();
        
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastAckReceived = false;
            return false;
        }
        delay(1);
    }
    
    return _receivedAck;
}

// ============================================================================
// LightFxClient - LED Direct Control
// ============================================================================

bool LightFxClient::ledSet(uint8_t channel, uint8_t brightness) {
    uint8_t payload[2] = { channel, brightness };
    return sendPacketBlocking(LightFxPacket::LED_SET, payload, sizeof(payload));
}

bool LightFxClient::ledOff(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_OFF, payload, sizeof(payload));
}

// ============================================================================
// LightFxClient - LED Sequence Control
// ============================================================================

bool LightFxClient::ledSeqClear(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_CLEAR, payload, sizeof(payload));
}

bool LightFxClient::ledSeqAddOn(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::ON;
    putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxClient::ledSeqAddOff(uint8_t channel, uint16_t durationMs) {
    uint8_t payload[4];
    payload[0] = channel;
    payload[1] = LightFxEventType::OFF;
    putU16LE(&payload[2], durationMs);
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxClient::ledSeqAddFlash(uint8_t channel, uint16_t intervalMs, uint16_t durationMs,
                                   uint8_t brightness, uint8_t dutyPercent) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FLASH;
    putU16LE(&payload[2], intervalMs);
    putU16LE(&payload[4], durationMs);
    payload[6] = brightness;
    payload[7] = dutyPercent;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxClient::ledSeqAddFadeIn(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_IN;
    putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxClient::ledSeqAddFadeOut(uint8_t channel, uint16_t durationMs, uint8_t brightness) {
    uint8_t payload[5];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADE_OUT;
    putU16LE(&payload[2], durationMs);
    payload[4] = brightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxClient::ledSeqAddFading(uint8_t channel, uint16_t cycleMs, uint16_t durationMs,
                                    uint8_t minBrightness, uint8_t maxBrightness) {
    uint8_t payload[8];
    payload[0] = channel;
    payload[1] = LightFxEventType::FADING;
    putU16LE(&payload[2], cycleMs);
    putU16LE(&payload[4], durationMs);
    payload[6] = minBrightness;
    payload[7] = maxBrightness;
    return sendPacketBlocking(LightFxPacket::LED_SEQ_ADD, payload, sizeof(payload));
}

bool LightFxClient::ledSeqStart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_START, payload, sizeof(payload));
}

bool LightFxClient::ledSeqStop(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_STOP, payload, sizeof(payload));
}

bool LightFxClient::ledSeqRestart(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacketBlocking(LightFxPacket::LED_SEQ_RESTART, payload, sizeof(payload));
}

bool LightFxClient::ledSeqStatus(uint8_t channel) {
    uint8_t payload[1] = { channel };
    return sendPacket(LightFxPacket::LED_SEQ_STATUS, payload, sizeof(payload)) > 0;
}

bool LightFxClient::ledStatus() {
    return sendPacket(LightFxPacket::LED_STATUS, nullptr, 0) > 0;
}

// ============================================================================
// LightFxClient - Servo Control
// ============================================================================

bool LightFxClient::servoSet(uint8_t id, int16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = id;
    putI16LE(&payload[1], pulseUs);
    return sendPacketBlocking(LightFxPacket::SERVO_SET, payload, sizeof(payload));
}

bool LightFxClient::servoSettings(uint8_t id, uint16_t minUs, uint16_t maxUs,
                                  uint16_t speed, uint16_t accel, uint16_t decel) {
    uint8_t payload[11];
    payload[0] = id;
    putU16LE(&payload[1], minUs);
    putU16LE(&payload[3], maxUs);
    putU16LE(&payload[5], speed);
    putU16LE(&payload[7], accel);
    putU16LE(&payload[9], decel);
    return sendPacketBlocking(LightFxPacket::SERVO_SETTINGS, payload, sizeof(payload));
}

// ============================================================================
// LightFxClient - Power Monitor
// ============================================================================

bool LightFxClient::powerStatus() {
    return sendPacket(LightFxPacket::POWER_STATUS, nullptr, 0) > 0;
}

// ============================================================================
// LightFxServer Implementation
// ============================================================================

bool LightFxServer::begin(Stream* serial) {
    if (!serial) return false;
    _serial = serial;
    _initialized = true;
    return true;
}

void LightFxServer::end() {
    _serial = nullptr;
    _initialized = false;
}

CommandHandleResult LightFxServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return CommandHandleResult::NotMyCommand;

    // Check if packet type is in LightFX range (0x40-0x5F)
    if (type < 0x40 || type > 0x5F) return CommandHandleResult::NotMyCommand;

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

        case LightFxPacket::POWER_STATUS:
            if (_powerStatusCallback) {
                LightFxPowerStatus status;
                _powerStatusCallback(status);
                sendPowerStatus(status);
            }
            return CommandHandleResult::Handled;

        case LightFxPacket::POWER_CONFIG: {
            SFX_REQUIRE_LEN(4);
            uint16_t shuntMohm = getU16LE(&payload[0]);
            uint16_t maxCurrentMa = getU16LE(&payload[2]);
            SFX_VALIDATE(shuntMohm >= LightFxSpec::INA226_SHUNT_MOHM_MIN &&
                         shuntMohm <= LightFxSpec::INA226_SHUNT_MOHM_MAX,
                         SerialError::PARAM_OUT_OF_RANGE);
            SFX_VALIDATE(maxCurrentMa != 0, SerialError::INVALID_VALUE);
            SFX_DISPATCH(_powerConfigCallback, shuntMohm, maxCurrentMa);
        }

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// LightFxServer - Response Methods
// ============================================================================

int LightFxServer::sendRawPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_serial) return -1;
    
    uint8_t buffer[COBS_BUFFER_SIZE];
    size_t encodedLen = encodePacket(buffer, type, payload, len);
    
    if (encodedLen == 0) return -1;
    
    size_t written = _serial->write(buffer, encodedLen);
    _serial->write(FRAME_DELIMITER);
    
    return (int)written;
}

int LightFxServer::sendAck() {
    return sendRawPacket(CorePacket::ACK, nullptr, 0);
}

int LightFxServer::sendNack(uint8_t errorCode) {
    uint8_t payload[1] = { errorCode };
    return sendRawPacket(CorePacket::NACK, payload, sizeof(payload));
}

int LightFxServer::sendSeqStatus(const LightFxSeqStatus& status) {
    uint8_t payload[8];
    payload[0] = status.channel;
    payload[1] = status.playing ? 1 : 0;
    payload[2] = status.eventCount;
    payload[3] = status.currentIndex;
    putU32LE(&payload[4], status.loopCount);
    return sendRawPacket(LightFxPacket::LED_SEQ_STATUS_RESP, payload, sizeof(payload));
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
    
    return sendRawPacket(LightFxPacket::LED_STATUS_RESP, payload, len);
}

int LightFxServer::sendPowerStatus(const LightFxPowerStatus& status) {
    uint8_t payload[11];  // 7 bytes + 4 bytes for shunt config
    putU16LE(&payload[0], (uint16_t)(status.voltage * 1000.0f));  // V to mV
    putI16LE(&payload[2], (int16_t)status.current);  // mA
    putU16LE(&payload[4], (uint16_t)status.power);  // mW
    payload[6] = status.available ? 1 : 0;
    putU16LE(&payload[7], status.shuntMohm);  // Shunt resistance in mΩ
    putU16LE(&payload[9], status.maxCurrentMa);  // Max current in mA
    return sendRawPacket(LightFxPacket::POWER_STATUS_RESP, payload, sizeof(payload));
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
    
    return sendRawPacket(LightFxPacket::LED_SEQ_QUEUE_RESP, payload, payloadLen);
}
