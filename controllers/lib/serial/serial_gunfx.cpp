/*
 * Serial GunFX Protocol - Implementation
 *
 * Binary COBS protocol client/server for GunFX muzzle flash controller.
 *   - GunFxClient: For HubFX (sends commands via USB)
 *   - GunFxServer: For GunFX Pico (receives commands, implements ICommandHandler)
 */

#include "serial_gunfx.h"

using namespace CoreProtocol;

// ============================================================================
// GunFxClient Implementation
// ============================================================================

bool GunFxClient::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    _usbHostRef = usbHost;
    _serverReady = false;
    _serverName[0] = '\0';
    memset(&_boardInfo, 0, sizeof(_boardInfo));
    memset(&_lastStatus, 0, sizeof(_lastStatus));

    // Set up internal packet handler
    SerialBus::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    return true;
}

int GunFxClient::process() {
    return SerialBus::process();
}

int GunFxClient::sendInit(unsigned long keepaliveMs) {
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

void GunFxClient::setCompatibleVersions(const char** versions, size_t count) {
    _compatibleVersions = versions;
    _compatibleVersionCount = count;
}

bool GunFxClient::checkVersionCompatibility(const char* version) {
    if (_compatibleVersions == nullptr || _compatibleVersionCount == 0) {
        return true;
    }
    
    for (size_t i = 0; i < _compatibleVersionCount; i++) {
        if (strcmp(version, _compatibleVersions[i]) == 0) {
            return true;
        }
    }
    
    return false;
}

void GunFxClient::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
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
                if (version) {
                    strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
                    _boardInfo.versionCompatible = checkVersionCompatibility(version);
                }
                if (platform) strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
                if (cpuStr) _boardInfo.cpuFrequencyMHz = atoi(cpuStr);
                if (ramStr) _boardInfo.freeRamBytes = atoi(ramStr);
            }
            if (_readyCallback) _readyCallback(_serverName);
            break;

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
                
                if (_pendingAckNack) {
                    _receivedAck = true;
                    _pendingAckNack = false;
                    _lastCommandResult = CommandResult::Ack();
                }
                
                if (_statusCallback) _statusCallback(_lastStatus);
            } else if (len >= 12) {
                // Core-only status (no module data)
                _lastStatus.uptimeMs = getU32LE(&payload[4]);
                _lastStatus.freeRam = getU32LE(&payload[8]);
                
                if (_pendingAckNack) {
                    _receivedAck = true;
                    _pendingAckNack = false;
                    _lastCommandResult = CommandResult::Ack();
                }
                
                if (_statusCallback) _statusCallback(_lastStatus);
            }
            break;

        case CorePacket::ERROR:
            if (_errorCallback && len >= 1) {
                uint8_t errorCode = payload[0];
                char message[64] = "";
                if (len > 1) {
                    size_t msgLen = (len - 1 < sizeof(message) - 1) ? len - 1 : sizeof(message) - 1;
                    memcpy(message, &payload[1], msgLen);
                    message[msgLen] = '\0';
                }
                _errorCallback(errorCode, message);
            }
            break;

        case CorePacket::ACK:
            _receivedAck = true;
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Ack();
            break;

        case CorePacket::NACK:
            _receivedNack = true;
            _pendingAckNack = false;
            {
                uint8_t errorCode = (len >= 1) ? payload[0] : SerialError::UNKNOWN;
                char reason[64] = "";
                if (len > 1) {
                    size_t msgLen = (len - 1 < sizeof(reason) - 1) ? len - 1 : sizeof(reason) - 1;
                    memcpy(reason, &payload[1], msgLen);
                    reason[msgLen] = '\0';
                }
                _lastNackErrorCode = errorCode;
                strncpy(_lastNackReason, reason[0] ? reason : GunFxError::getMessage(errorCode), 
                        sizeof(_lastNackReason) - 1);
                _lastCommandResult = CommandResult::Nack(errorCode, _lastNackReason);
            }
            break;

        default:
            break;
    }
}

CommandResult GunFxClient::sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len) {
    if (!isConnected()) {
        _lastCommandResult = CommandResult::NotConnected();
        return _lastCommandResult;
    }
    
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    _lastNackErrorCode = 0;
    _lastNackReason[0] = '\0';
    
    int sent = sendPacket(type, payload, len);
    if (sent < 0) {
        _pendingAckNack = false;
        _lastCommandResult = CommandResult::SendFailed();
        return _lastCommandResult;
    }
    
    if (!_blockingMode) {
        _lastCommandResult = CommandResult::Ack();
        return _lastCommandResult;
    }
    
    return waitForAckNack();
}

CommandResult GunFxClient::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        SerialBus::process();
        
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Timeout();
            return _lastCommandResult;
        }
        
        delay(1);
    }
    
    return _lastCommandResult;
}

// ============================================================================
// GunFxClient - Trigger Control
// ============================================================================

CommandResult GunFxClient::triggerOn(uint16_t rpm) {
    uint8_t payload[2];
    putU16LE(payload, rpm);
    return sendPacketBlocking(GunFxPacket::TRIGGER_ON, payload, sizeof(payload));
}

CommandResult GunFxClient::triggerOff(uint16_t fanDelayMs) {
    uint8_t payload[2];
    putU16LE(payload, fanDelayMs);
    return sendPacketBlocking(GunFxPacket::TRIGGER_OFF, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Servo Control
// ============================================================================

CommandResult GunFxClient::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = servoId;
    putU16LE(&payload[1], pulseUs);
    return sendPacketBlocking(GunFxPacket::SRV_SET, payload, sizeof(payload));
}

CommandResult GunFxClient::setServoConfig(const GunFxServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    putU16LE(&payload[1], config.minUs);
    putU16LE(&payload[3], config.maxUs);
    putU16LE(&payload[5], config.maxSpeedUsPerSec);
    putU16LE(&payload[7], config.maxAccelUsPerSec2);
    putU16LE(&payload[9], config.maxDecelUsPerSec2);
    return sendPacketBlocking(GunFxPacket::SRV_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxClient::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    uint8_t payload[5];
    payload[0] = servoId;
    putU16LE(&payload[1], jerkUs);
    putU16LE(&payload[3], varianceUs);
    return sendPacketBlocking(GunFxPacket::SRV_RECOIL_JERK, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Smoke Control
// ============================================================================

CommandResult GunFxClient::setSmokeHeater(bool on) {
    uint8_t payload[1] = { on ? (uint8_t)1 : (uint8_t)0 };
    return sendPacketBlocking(GunFxPacket::SMOKE_HEAT, payload, sizeof(payload));
}

CommandResult GunFxClient::setSmokeSettings(const GunFxSmokeConfig& config) {
    uint8_t payload[8];
    payload[0] = config.fanPulsing ? 1 : 0;
    payload[1] = config.fanSpeed;
    payload[2] = config.fanPulseHigh;
    payload[3] = config.fanPulseLow;
    putU16LE(&payload[4], config.fanPulseMs);
    putU16LE(&payload[6], config.fanSpindownMs);
    return sendPacketBlocking(GunFxPacket::SMOKE_SETTINGS, payload, sizeof(payload));
}

// ============================================================================
// GunFxClient - Status
// ============================================================================

CommandResult GunFxClient::requestStatus() {
    return sendPacketBlocking(CorePacket::STATUS_REQ, nullptr, 0);
}

// ============================================================================
// GunFxServer Implementation
// ============================================================================

bool GunFxServer::begin(Stream* serial, const char* moduleName) {
    if (!serial) return false;

    _serial = serial;
    _rxIndex = 0;
    _clientConnected = false;
    _lastRxTimeMs = 0;

    strncpy(_moduleName, moduleName, sizeof(_moduleName) - 1);
    _moduleName[sizeof(_moduleName) - 1] = '\0';

    _initialized = true;
    return true;
}

void GunFxServer::end() {
    _initialized = false;
    _serial = nullptr;
    _clientConnected = false;
}

int GunFxServer::process() {
    if (!_initialized || !_serial) return 0;

    int packetsProcessed = 0;

    while (_serial->available()) {
        uint8_t byte = _serial->read();
        
        if (byte == FRAME_DELIMITER) {
            if (_rxIndex > 0) {
                processFrame(_rxBuffer, _rxIndex);
                packetsProcessed++;
                _rxIndex = 0;
            }
        } else {
            if (_rxIndex < sizeof(_rxBuffer)) {
                _rxBuffer[_rxIndex++] = byte;
            } else {
                // Buffer overflow, reset
                _rxIndex = 0;
            }
        }
    }

    // Check connection timeout
    if (_connectionTimeoutMs > 0 && _clientConnected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _clientConnected = false;
        }
    }

    return packetsProcessed;
}

void GunFxServer::processFrame(const uint8_t* frame, size_t frameLen) {
    uint8_t decoded[MAX_PACKET_SIZE];
    size_t decodedLen = cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    
    if (decodedLen == 0) return;

    uint8_t type;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!parsePacket(decoded, decodedLen, &type, &payload, &payloadLen)) {
        return;
    }

    _lastRxTimeMs = millis();
    _clientConnected = true;

    handlePacket(type, payload, payloadLen);
}

CommandHandleResult GunFxServer::tryProcess(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return CommandHandleResult::NotMyCommand;

    // Check if packet type is in GunFX range (0x01-0x2F)
    if (type >= 0x01 && type <= 0x2F) {
        _lastRxTimeMs = millis();
        _clientConnected = true;
        return handlePacket(type, payload, len);
    }

    return CommandHandleResult::NotMyCommand;
}

CommandHandleResult GunFxServer::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
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
            GunFxServoConfig config;
            config.servoId = payload[0];
            config.recoilJerkUs = getU16LE(&payload[1]);
            config.recoilJerkVarianceUs = getU16LE(&payload[3]);
            SFX_VALIDATE(GunFxSpec::isValidServoId(config.servoId), GunFxError::SERVO_INVALID_ID);
            SFX_DISPATCH(_servoSettingsCallback, config);
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

        default:
            return CommandHandleResult::NotMyCommand;
    }
}

// ============================================================================
// GunFxServer - Response Methods
// ============================================================================

int GunFxServer::sendRawPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_serial) return -1;
    
    uint8_t buffer[COBS_BUFFER_SIZE];
    size_t encodedLen = encodePacket(buffer, type, payload, len);
    
    if (encodedLen == 0) return -1;
    
    size_t written = _serial->write(buffer, encodedLen);
    _serial->write(FRAME_DELIMITER);
    
    return (int)written;
}

int GunFxServer::sendAck() {
    return sendRawPacket(CorePacket::ACK, nullptr, 0);
}

int GunFxServer::sendNack(uint8_t errorCode, const char* reason) {
    uint8_t payload[64];
    payload[0] = errorCode;
    
    const char* msg = (reason && reason[0]) ? reason : GunFxError::getMessage(errorCode);
    size_t msgLen = strlen(msg);
    if (msgLen > sizeof(payload) - 1) {
        msgLen = sizeof(payload) - 1;
    }
    memcpy(&payload[1], msg, msgLen);
    
    return sendRawPacket(CorePacket::NACK, payload, 1 + msgLen);
}

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
    
    return sendRawPacket(CorePacket::STATUS, payload, sizeof(payload));
}

int GunFxServer::sendError(uint8_t errorCode, const char* message) {
    uint8_t payload[64];
    payload[0] = errorCode;
    
    size_t len = 1;
    if (message) {
        size_t msgLen = strlen(message);
        if (msgLen > sizeof(payload) - 1) {
            msgLen = sizeof(payload) - 1;
        }
        memcpy(&payload[1], message, msgLen);
        len += msgLen;
    }
    
    return sendRawPacket(CorePacket::ERROR, payload, len);
}
