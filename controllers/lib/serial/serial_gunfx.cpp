/*
 * Serial GunFX - Master/Slave Communication Implementation
 */

#include "serial_gunfx.h"
#include "serial_init.h"  // For SerialInitSender

// ============================================================================
// GunFxSerialMaster Implementation
// ============================================================================

bool GunFxSerialMaster::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    // Store USB host reference for text INIT
    _usbHostRef = usbHost;

    // Set up internal packet handler
    SerialBus::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    _slaveReady = false;
    _slaveName[0] = '\0';

    return true;
}

void GunFxSerialMaster::setCompatibleVersions(const char** versions, size_t count) {
    _compatibleVersions = versions;
    _compatibleVersionCount = count;
}

bool GunFxSerialMaster::checkVersionCompatibility(const char* version) {
    // If no compatibility list set, accept any version
    if (_compatibleVersions == nullptr || _compatibleVersionCount == 0) {
        return true;
    }
    
    // Check if version matches any in the compatibility list
    for (size_t i = 0; i < _compatibleVersionCount; i++) {
        if (strcmp(version, _compatibleVersions[i]) == 0) {
            return true;
        }
    }
    
    return false;
}

int GunFxSerialMaster::process() {
    int result = SerialBus::process();
    
    // Check connection timeout (inherited from SerialBus)
    return result;
}

int GunFxSerialMaster::sendInit(unsigned long keepaliveMs) {
    if (!_usbHostRef) return -1;
    
    // Build text INIT command: "INIT protocol=binary keepalive=<ms> or keepalive=off"
    char buf[64];
    if (keepaliveMs > 0) {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=%lu", keepaliveMs);
    } else {
        snprintf(buf, sizeof(buf), "INIT protocol=binary keepalive=off");
    }
    
    // Send as text line (the slave initHandler expects text INIT)
    int written = _usbHostRef->cdcPrintln(SerialBus::deviceIndex(), buf);
    if (written > 0) {
        _lastSendMs = millis();  // Update send time for keepalive tracking
    }
    return written;
}

// ----------------------------------------------------------------------------
// Blocking Command Support
// ----------------------------------------------------------------------------

CommandResult GunFxSerialMaster::sendPacketBlocking(uint8_t type, const uint8_t* payload, size_t len) {
    if (!isConnected()) {
        _lastCommandResult = CommandResult::NotConnected();
        return _lastCommandResult;
    }
    
    // Clear ACK/NACK state
    _pendingAckNack = true;
    _receivedAck = false;
    _receivedNack = false;
    _lastNackErrorCode = 0;
    _lastNackReason[0] = '\0';
    
    // Send packet
    int sent = sendPacket(type, payload, len);
    if (sent < 0) {
        _pendingAckNack = false;
        _lastCommandResult = CommandResult::SendFailed();
        return _lastCommandResult;
    }
    
    // If not blocking, return immediately
    if (!_blockingMode) {
        _lastCommandResult = CommandResult::Ack();
        return _lastCommandResult;
    }
    
    // Wait for ACK/NACK
    return waitForAckNack();
}

CommandResult GunFxSerialMaster::waitForAckNack() {
    unsigned long startMs = millis();
    
    while (_pendingAckNack) {
        // Process incoming data
        SerialBus::process();
        
        // Check timeout
        if (millis() - startMs > _commandTimeoutMs) {
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Timeout();
            return _lastCommandResult;
        }
        
        // Small delay to avoid busy-waiting
        delay(1);
    }
    
    return _lastCommandResult;
}

// ----------------------------------------------------------------------------
// Trigger Control
// ----------------------------------------------------------------------------

CommandResult GunFxSerialMaster::triggerOn(uint16_t rpm) {
    uint8_t payload[2];
    SerialProtocol::putU16LE(payload, rpm);
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_TRIGGER_ON, payload, sizeof(payload));
}

CommandResult GunFxSerialMaster::triggerOff(uint16_t fanDelayMs) {
    uint8_t payload[2];
    SerialProtocol::putU16LE(payload, fanDelayMs);
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_TRIGGER_OFF, payload, sizeof(payload));
}

// ----------------------------------------------------------------------------
// Servo Control
// ----------------------------------------------------------------------------

CommandResult GunFxSerialMaster::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = servoId;
    SerialProtocol::putU16LE(&payload[1], pulseUs);
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_SRV_SET, payload, sizeof(payload));
}

CommandResult GunFxSerialMaster::setServoConfig(const GunFxServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    SerialProtocol::putU16LE(&payload[1], config.minUs);
    SerialProtocol::putU16LE(&payload[3], config.maxUs);
    SerialProtocol::putU16LE(&payload[5], config.maxSpeedUsPerSec);
    SerialProtocol::putU16LE(&payload[7], config.maxAccelUsPerSec2);
    SerialProtocol::putU16LE(&payload[9], config.maxDecelUsPerSec2);
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_SRV_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxSerialMaster::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    uint8_t payload[5];
    payload[0] = servoId;
    SerialProtocol::putU16LE(&payload[1], jerkUs);
    SerialProtocol::putU16LE(&payload[3], varianceUs);
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_SRV_RECOIL_JERK, payload, sizeof(payload));
}

// ----------------------------------------------------------------------------
// Smoke Control
// ----------------------------------------------------------------------------

CommandResult GunFxSerialMaster::setSmokeHeater(bool on) {
    uint8_t payload[1] = { on ? (uint8_t)1 : (uint8_t)0 };
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_SMOKE_HEAT, payload, sizeof(payload));
}

CommandResult GunFxSerialMaster::setSmokeSettings(const GunFxSmokeConfig& config) {
    uint8_t payload[8];
    payload[0] = config.fanPulsing ? 1 : 0;
    payload[1] = config.fanSpeed;
    payload[2] = config.fanPulseHigh;
    payload[3] = config.fanPulseLow;
    SerialProtocol::putU16LE(&payload[4], config.fanPulseMs);
    SerialProtocol::putU16LE(&payload[6], config.fanSpindownMs);
    return sendPacketBlocking(SerialProtocol::GUNFX_PKT_SMOKE_SETTINGS, payload, sizeof(payload));
}

CommandResult GunFxSerialMaster::requestStatus() {
    // Send STATUS_REQ packet - slave responds with STATUS packet
    // Unlike other commands, slave responds with STATUS (not ACK/NACK)
    return sendPacketBlocking(SerialProtocol::SFX_PKT_STATUS_REQ, nullptr, 0);
}

// ----------------------------------------------------------------------------
// Packet Handler
// ----------------------------------------------------------------------------

void GunFxSerialMaster::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case SerialProtocol::SFX_PKT_INIT_READY:
            _slaveReady = true;
            
            // Parse board info: name|version|platform|cpuMHz|ramBytes
            if (len > 0) {
                // Convert to null-terminated string for parsing
                char buffer[128];
                size_t bufLen = (len < sizeof(buffer) - 1) ? len : sizeof(buffer) - 1;
                memcpy(buffer, payload, bufLen);
                buffer[bufLen] = '\0';
                
                // Parse pipe-delimited format
                char* name = strtok(buffer, "|");
                char* version = strtok(nullptr, "|");
                char* platform = strtok(nullptr, "|");
                char* cpuStr = strtok(nullptr, "|");
                char* ramStr = strtok(nullptr, "|");
                
                if (name) {
                    strncpy(_slaveName, name, sizeof(_slaveName) - 1);
                    _slaveName[sizeof(_slaveName) - 1] = '\0';
                    strncpy(_boardInfo.deviceName, name, sizeof(_boardInfo.deviceName) - 1);
                    _boardInfo.deviceName[sizeof(_boardInfo.deviceName) - 1] = '\0';
                }
                if (version) {
                    strncpy(_boardInfo.firmwareVersion, version, sizeof(_boardInfo.firmwareVersion) - 1);
                    _boardInfo.firmwareVersion[sizeof(_boardInfo.firmwareVersion) - 1] = '\0';
                }
                if (platform) {
                    strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
                    _boardInfo.platform[sizeof(_boardInfo.platform) - 1] = '\0';
                }
                if (cpuStr) {
                    _boardInfo.cpuFrequencyMHz = atoi(cpuStr);
                }
                if (ramStr) {
                    _boardInfo.freeRamBytes = atoi(ramStr);
                }
                
                // Check version compatibility
                _boardInfo.versionCompatible = checkVersionCompatibility(_boardInfo.firmwareVersion);
            }
            
            if (_readyCallback) {
                _readyCallback(_slaveName);
            }
            break;

        case SerialProtocol::SFX_PKT_STATUS:
            // Parse new 28-byte status format
            if (len >= 28) {
                uint8_t flags = payload[0];
                _lastStatus.firing = (flags & 0x01) != 0;
                _lastStatus.flashActive = (flags & 0x02) != 0;
                _lastStatus.flashFading = (flags & 0x04) != 0;
                _lastStatus.heaterOn = (flags & 0x08) != 0;
                _lastStatus.fanOn = (flags & 0x10) != 0;
                _lastStatus.fanSpindown = (flags & 0x20) != 0;
                
                _lastStatus.fanSpeed = payload[1];
                _lastStatus.fanOffRemainingMs = SerialProtocol::getU16LE(&payload[2]);
                _lastStatus.servoUs[0] = SerialProtocol::getU16LE(&payload[4]);
                _lastStatus.servoUs[1] = SerialProtocol::getU16LE(&payload[6]);
                _lastStatus.servoUs[2] = SerialProtocol::getU16LE(&payload[8]);
                _lastStatus.rateOfFireRpm = SerialProtocol::getU16LE(&payload[10]);
                _lastStatus.shotsFired = SerialProtocol::getU32LE(&payload[12]);
                _lastStatus.heaterOnTimeMs = SerialProtocol::getU32LE(&payload[16]);
                _lastStatus.uptimeMs = SerialProtocol::getU32LE(&payload[20]);
                _lastStatus.freeRam = SerialProtocol::getU32LE(&payload[24]);
                
                // STATUS response completes a pending STATUS_REQ command
                if (_pendingAckNack) {
                    _receivedAck = true;
                    _pendingAckNack = false;
                    _lastCommandResult = CommandResult::Ack();
                }
                
                if (_statusCallback) {
                    _statusCallback(_lastStatus);
                }
            }
            break;

        case SerialProtocol::SFX_PKT_ERROR:
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

        case SerialProtocol::SFX_PKT_ACK:
            _receivedAck = true;
            _pendingAckNack = false;
            _lastCommandResult = CommandResult::Ack();
            break;

        case SerialProtocol::SFX_PKT_NACK:
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
                // Use GunFxError::getMessage for domain-specific messages, falls back to SerialError
                strncpy(_lastNackReason, reason[0] ? reason : GunFxError::getMessage(errorCode), sizeof(_lastNackReason) - 1);
                _lastCommandResult = CommandResult::Nack(errorCode, _lastNackReason);
            }
            break;

        default:
            // Unknown packet type - ignore
            break;
    }
}

// ============================================================================
// GunFxSerialSlave Implementation
// ============================================================================

bool GunFxSerialSlave::begin(Stream* serial, const char* moduleName) {
    if (!serial) return false;

    _serial = serial;
    _rxIndex = 0;
    _masterConnected = false;
    _lastRxTimeMs = 0;

    // Copy module name
    strncpy(_moduleName, moduleName, sizeof(_moduleName) - 1);
    _moduleName[sizeof(_moduleName) - 1] = '\0';

    _initialized = true;
    return true;
}

void GunFxSerialSlave::end() {
    _initialized = false;
    _serial = nullptr;
    _masterConnected = false;
}

int GunFxSerialSlave::process() {
    if (!_initialized || !_serial) return 0;

    int packetsProcessed = 0;

    // Read available bytes
    while (_serial->available()) {
        uint8_t byte = _serial->read();
        
        if (byte == SerialProtocol::FRAME_DELIMITER) {
            // End of frame - process if we have data
            if (_rxIndex > 0) {
                processFrame(_rxBuffer, _rxIndex);
                packetsProcessed++;
                _rxIndex = 0;
            }
        } else {
            // Add to buffer
            if (_rxIndex < sizeof(_rxBuffer)) {
                _rxBuffer[_rxIndex++] = byte;
            } else {
                // Buffer overflow - reset
                _rxIndex = 0;
            }
        }
    }

    // Check connection timeout
    if (_connectionTimeoutMs > 0 && _masterConnected) {
        unsigned long now = millis();
        if (now - _lastRxTimeMs > _connectionTimeoutMs) {
            _masterConnected = false;
        }
    }

    return packetsProcessed;
}

void GunFxSerialSlave::processFrame(const uint8_t* frame, size_t frameLen) {
    // Decode COBS
    uint8_t decoded[SerialProtocol::MAX_PACKET_SIZE];
    size_t decodedLen = SerialProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    
    if (decodedLen == 0) return;

    // Parse packet
    uint8_t type;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!SerialProtocol::parsePacket(decoded, decodedLen, &type, &payload, &payloadLen)) {
        return;  // Invalid packet
    }

    // Update connection status
    _lastRxTimeMs = millis();
    _masterConnected = true;

    // Handle packet (ignore result in standalone mode)
    handlePacket(type, payload, payloadLen);
}

// ----------------------------------------------------------------------------
// IBinaryCommandHandler Implementation
// ----------------------------------------------------------------------------

CommandHandleResult GunFxSerialSlave::tryProcessPacket(uint8_t type, const uint8_t* payload, size_t len) {
    // Update connection status when receiving any packet
    _lastRxTimeMs = millis();
    _masterConnected = true;
    
    return handlePacket(type, payload, len);
}

CommandHandleResult GunFxSerialSlave::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case SerialProtocol::SFX_PKT_KEEPALIVE:
            // Keepalive received - connection status already updated
            return CommandHandleResult::Handled;

        case SerialProtocol::SFX_PKT_STATUS_REQ:
            // Master requests status - invoke callback and send response
            if (_statusRequestCallback) {
                GunFxStatus status = _statusRequestCallback();
                sendStatus(status);
            } else {
                // Send empty status if no callback registered
                sendStatus(GunFxStatus{});
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_TRIGGER_ON:
            if (len >= 2) {
                uint16_t rpm = SerialProtocol::getU16LE(payload);
                if (_triggerOnCallback) {
                    uint8_t result = _triggerOnCallback(rpm);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_TRIGGER_OFF:
            if (len >= 2) {
                uint16_t fanDelayMs = SerialProtocol::getU16LE(payload);
                if (_triggerOffCallback) {
                    uint8_t result = _triggerOffCallback(fanDelayMs);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_SRV_SET:
            if (len >= 3) {
                uint8_t servoId = payload[0];
                uint16_t pulseUs = SerialProtocol::getU16LE(&payload[1]);
                if (_servoSetCallback) {
                    uint8_t result = _servoSetCallback(servoId, pulseUs);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_SRV_SETTINGS:
            if (len >= 11) {
                GunFxServoConfig config;
                config.servoId = payload[0];
                config.minUs = SerialProtocol::getU16LE(&payload[1]);
                config.maxUs = SerialProtocol::getU16LE(&payload[3]);
                config.maxSpeedUsPerSec = SerialProtocol::getU16LE(&payload[5]);
                config.maxAccelUsPerSec2 = SerialProtocol::getU16LE(&payload[7]);
                config.maxDecelUsPerSec2 = SerialProtocol::getU16LE(&payload[9]);
                if (_servoSettingsCallback) {
                    uint8_t result = _servoSettingsCallback(config);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_SRV_RECOIL_JERK:
            if (len >= 5) {
                GunFxServoConfig config;
                config.servoId = payload[0];
                config.recoilJerkUs = SerialProtocol::getU16LE(&payload[1]);
                config.recoilJerkVarianceUs = SerialProtocol::getU16LE(&payload[3]);
                if (_servoSettingsCallback) {
                    uint8_t result = _servoSettingsCallback(config);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_SMOKE_HEAT:
            if (len >= 1) {
                bool on = payload[0] != 0;
                if (_smokeHeatCallback) {
                    uint8_t result = _smokeHeatCallback(on);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        case SerialProtocol::GUNFX_PKT_SMOKE_SETTINGS:
            if (len >= 8) {
                GunFxSmokeConfig config;
                config.fanPulsing = payload[0] != 0;
                config.fanSpeed = payload[1];
                config.fanPulseHigh = payload[2];
                config.fanPulseLow = payload[3];
                config.fanPulseMs = SerialProtocol::getU16LE(&payload[4]);
                config.fanSpindownMs = SerialProtocol::getU16LE(&payload[6]);
                if (_smokeSettingsCallback) {
                    uint8_t result = _smokeSettingsCallback(config);
                    if (result == SerialError::OK) {
                        sendAck();
                    } else {
                        sendNack(result);
                    }
                } else {
                    sendAck();
                }
            } else {
                sendNack(SerialError::MISSING_PARAMETER);
            }
            return CommandHandleResult::Handled;

        default:
            // Unknown packet type - let next handler try
            return CommandHandleResult::NotMyCommand;
    }
}

// ----------------------------------------------------------------------------
// Status Transmission
// ----------------------------------------------------------------------------

int GunFxSerialSlave::sendRawPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_serial) return -1;

    // Build and encode packet
    uint8_t encoded[SerialProtocol::COBS_BUFFER_SIZE];
    size_t encodedLen = SerialProtocol::encodePacket(encoded, type, payload, len);
    
    if (encodedLen == 0) return -1;

    // Send encoded packet + delimiter
    size_t written = _serial->write(encoded, encodedLen);
    _serial->write(SerialProtocol::FRAME_DELIMITER);
    
    return (int)written;
}

int GunFxSerialSlave::sendStatus(const GunFxStatus& status) {
    // Binary status payload: 28 bytes
    // [0]     flags (6 bits used)
    // [1]     fanSpeed (uint8)
    // [2-3]   fanOffRemainingMs (uint16)
    // [4-5]   servo0 (uint16)
    // [6-7]   servo1 (uint16)
    // [8-9]   servo2 (uint16)
    // [10-11] rateOfFireRpm (uint16)
    // [12-15] shotsFired (uint32)
    // [16-19] heaterOnTimeMs (uint32)
    // [20-23] uptimeMs (uint32)
    // [24-27] freeRam (uint32)
    
    uint8_t payload[28];
    
    // Pack flags
    uint8_t flags = 0;
    if (status.firing) flags |= 0x01;
    if (status.flashActive) flags |= 0x02;
    if (status.flashFading) flags |= 0x04;
    if (status.heaterOn) flags |= 0x08;
    if (status.fanOn) flags |= 0x10;
    if (status.fanSpindown) flags |= 0x20;
    
    payload[0] = flags;
    payload[1] = status.fanSpeed;
    SerialProtocol::putU16LE(&payload[2], status.fanOffRemainingMs);
    SerialProtocol::putU16LE(&payload[4], status.servoUs[0]);
    SerialProtocol::putU16LE(&payload[6], status.servoUs[1]);
    SerialProtocol::putU16LE(&payload[8], status.servoUs[2]);
    SerialProtocol::putU16LE(&payload[10], status.rateOfFireRpm);
    SerialProtocol::putU32LE(&payload[12], status.shotsFired);
    SerialProtocol::putU32LE(&payload[16], status.heaterOnTimeMs);
    SerialProtocol::putU32LE(&payload[20], status.uptimeMs);
    SerialProtocol::putU32LE(&payload[24], status.freeRam);
    
    return sendRawPacket(SerialProtocol::SFX_PKT_STATUS, payload, sizeof(payload));
}

int GunFxSerialSlave::sendError(uint8_t errorCode, const char* message) {
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
    
    return sendRawPacket(SerialProtocol::SFX_PKT_ERROR, payload, len);
}

int GunFxSerialSlave::sendAck() {
    return sendRawPacket(SerialProtocol::SFX_PKT_ACK, nullptr, 0);
}

int GunFxSerialSlave::sendNack(uint8_t errorCode, const char* reason) {
    uint8_t payload[64];
    payload[0] = errorCode;
    
    const char* msg = (reason && reason[0]) ? reason : GunFxError::getMessage(errorCode);
    size_t msgLen = strlen(msg);
    if (msgLen > sizeof(payload) - 1) {
        msgLen = sizeof(payload) - 1;
    }
    memcpy(&payload[1], msg, msgLen);
    
    return sendRawPacket(SerialProtocol::SFX_PKT_NACK, payload, 1 + msgLen);
}
