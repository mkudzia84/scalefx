/*
 * Serial GunFX - Master/Slave Communication Implementation
 */

#include "serial_gunfx.h"

// ============================================================================
// GunFxSerialMaster Implementation
// ============================================================================

bool GunFxSerialMaster::begin(UsbHost* usbHost, int deviceIndex) {
    if (!SerialBus::begin(usbHost, deviceIndex)) {
        return false;
    }

    // Set up internal packet handler
    SerialBus::onPacketReceived([this](uint8_t type, const uint8_t* payload, size_t len) {
        handlePacket(type, payload, len);
    });

    _slaveReady = false;
    _slaveName[0] = '\0';

    return true;
}

// ----------------------------------------------------------------------------
// Trigger Control
// ----------------------------------------------------------------------------

int GunFxSerialMaster::triggerOn(uint16_t rpm) {
    uint8_t payload[2];
    SerialProtocol::putU16LE(payload, rpm);
    return sendPacket(SerialProtocol::GUNFX_PKT_TRIGGER_ON, payload, sizeof(payload));
}

int GunFxSerialMaster::triggerOff(uint16_t fanDelayMs) {
    uint8_t payload[2];
    SerialProtocol::putU16LE(payload, fanDelayMs);
    return sendPacket(SerialProtocol::GUNFX_PKT_TRIGGER_OFF, payload, sizeof(payload));
}

// ----------------------------------------------------------------------------
// Servo Control
// ----------------------------------------------------------------------------

int GunFxSerialMaster::setServoPosition(uint8_t servoId, uint16_t pulseUs) {
    uint8_t payload[3];
    payload[0] = servoId;
    SerialProtocol::putU16LE(&payload[1], pulseUs);
    return sendPacket(SerialProtocol::GUNFX_PKT_SRV_SET, payload, sizeof(payload));
}

int GunFxSerialMaster::setServoConfig(const GunFxServoConfig& config) {
    uint8_t payload[11];
    payload[0] = config.servoId;
    SerialProtocol::putU16LE(&payload[1], config.minUs);
    SerialProtocol::putU16LE(&payload[3], config.maxUs);
    SerialProtocol::putU16LE(&payload[5], config.maxSpeedUsPerSec);
    SerialProtocol::putU16LE(&payload[7], config.maxAccelUsPerSec2);
    SerialProtocol::putU16LE(&payload[9], config.maxDecelUsPerSec2);
    return sendPacket(SerialProtocol::GUNFX_PKT_SRV_SETTINGS, payload, sizeof(payload));
}

int GunFxSerialMaster::setRecoilJerk(uint8_t servoId, uint16_t jerkUs, uint16_t varianceUs) {
    uint8_t payload[5];
    payload[0] = servoId;
    SerialProtocol::putU16LE(&payload[1], jerkUs);
    SerialProtocol::putU16LE(&payload[3], varianceUs);
    return sendPacket(SerialProtocol::GUNFX_PKT_SRV_RECOIL_JERK, payload, sizeof(payload));
}

// ----------------------------------------------------------------------------
// Smoke Control
// ----------------------------------------------------------------------------

int GunFxSerialMaster::setSmokeHeater(bool on) {
    uint8_t payload[1] = { on ? (uint8_t)1 : (uint8_t)0 };
    return sendPacket(SerialProtocol::GUNFX_PKT_SMOKE_HEAT, payload, sizeof(payload));
}

// ----------------------------------------------------------------------------
// Packet Handler
// ----------------------------------------------------------------------------

void GunFxSerialMaster::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case SerialProtocol::SFX_PKT_INIT_READY:
            _slaveReady = true;
            if (len > 0) {
                size_t nameLen = (len < sizeof(_slaveName) - 1) ? len : sizeof(_slaveName) - 1;
                memcpy(_slaveName, payload, nameLen);
                _slaveName[nameLen] = '\0';
            }
            if (_readyCallback) {
                _readyCallback(_slaveName);
            }
            break;

        case SerialProtocol::SFX_PKT_STATUS:
            if (len >= 11) {
                uint8_t flags = payload[0];
                _lastStatus.firing = (flags & 0x01) != 0;
                _lastStatus.flashActive = (flags & 0x02) != 0;
                _lastStatus.flashFading = (flags & 0x04) != 0;
                _lastStatus.heaterOn = (flags & 0x08) != 0;
                _lastStatus.fanOn = (flags & 0x10) != 0;
                _lastStatus.fanSpindown = (flags & 0x20) != 0;
                
                _lastStatus.fanOffRemainingMs = SerialProtocol::getU16LE(&payload[1]);
                _lastStatus.servoUs[0] = SerialProtocol::getU16LE(&payload[3]);
                _lastStatus.servoUs[1] = SerialProtocol::getU16LE(&payload[5]);
                _lastStatus.servoUs[2] = SerialProtocol::getU16LE(&payload[7]);
                _lastStatus.rateOfFireRpm = SerialProtocol::getU16LE(&payload[9]);
                
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

    // Handle packet
    handlePacket(type, payload, payloadLen);
}

void GunFxSerialSlave::handlePacket(uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case SerialProtocol::SFX_PKT_INIT:
            if (_initCallback) {
                _initCallback();
            }
            // Auto-respond with INIT_READY
            sendInitReady();
            break;

        case SerialProtocol::SFX_PKT_SHUTDOWN:
            if (_shutdownCallback) {
                _shutdownCallback();
            }
            break;

        case SerialProtocol::SFX_PKT_KEEPALIVE:
            // Keepalive received - connection status already updated
            break;

        case SerialProtocol::GUNFX_PKT_TRIGGER_ON:
            if (_triggerOnCallback && len >= 2) {
                uint16_t rpm = SerialProtocol::getU16LE(payload);
                _triggerOnCallback(rpm);
            }
            break;

        case SerialProtocol::GUNFX_PKT_TRIGGER_OFF:
            if (_triggerOffCallback && len >= 2) {
                uint16_t fanDelayMs = SerialProtocol::getU16LE(payload);
                _triggerOffCallback(fanDelayMs);
            }
            break;

        case SerialProtocol::GUNFX_PKT_SRV_SET:
            if (_servoSetCallback && len >= 3) {
                uint8_t servoId = payload[0];
                uint16_t pulseUs = SerialProtocol::getU16LE(&payload[1]);
                _servoSetCallback(servoId, pulseUs);
            }
            break;

        case SerialProtocol::GUNFX_PKT_SRV_SETTINGS:
            if (_servoSettingsCallback && len >= 11) {
                GunFxServoConfig config;
                config.servoId = payload[0];
                config.minUs = SerialProtocol::getU16LE(&payload[1]);
                config.maxUs = SerialProtocol::getU16LE(&payload[3]);
                config.maxSpeedUsPerSec = SerialProtocol::getU16LE(&payload[5]);
                config.maxAccelUsPerSec2 = SerialProtocol::getU16LE(&payload[7]);
                config.maxDecelUsPerSec2 = SerialProtocol::getU16LE(&payload[9]);
                _servoSettingsCallback(config);
            }
            break;

        case SerialProtocol::GUNFX_PKT_SRV_RECOIL_JERK:
            if (_servoSettingsCallback && len >= 5) {
                GunFxServoConfig config;
                config.servoId = payload[0];
                config.recoilJerkUs = SerialProtocol::getU16LE(&payload[1]);
                config.recoilJerkVarianceUs = SerialProtocol::getU16LE(&payload[3]);
                _servoSettingsCallback(config);
            }
            break;

        case SerialProtocol::GUNFX_PKT_SMOKE_HEAT:
            if (_smokeHeatCallback && len >= 1) {
                bool on = payload[0] != 0;
                _smokeHeatCallback(on);
            }
            break;

        default:
            // Unknown packet type - ignore
            break;
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

int GunFxSerialSlave::sendInitReady() {
    size_t nameLen = strlen(_moduleName);
    return sendRawPacket(SerialProtocol::SFX_PKT_INIT_READY, 
                         (const uint8_t*)_moduleName, nameLen);
}

int GunFxSerialSlave::sendStatus(const GunFxStatus& status) {
    uint8_t payload[11];
    
    // Pack flags
    uint8_t flags = 0;
    if (status.firing) flags |= 0x01;
    if (status.flashActive) flags |= 0x02;
    if (status.flashFading) flags |= 0x04;
    if (status.heaterOn) flags |= 0x08;
    if (status.fanOn) flags |= 0x10;
    if (status.fanSpindown) flags |= 0x20;
    
    payload[0] = flags;
    SerialProtocol::putU16LE(&payload[1], status.fanOffRemainingMs);
    SerialProtocol::putU16LE(&payload[3], status.servoUs[0]);
    SerialProtocol::putU16LE(&payload[5], status.servoUs[1]);
    SerialProtocol::putU16LE(&payload[7], status.servoUs[2]);
    SerialProtocol::putU16LE(&payload[9], status.rateOfFireRpm);
    
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

int GunFxSerialSlave::sendNack(const char* reason) {
    if (reason) {
        size_t len = strlen(reason);
        if (len > SerialProtocol::MAX_PAYLOAD_SIZE) {
            len = SerialProtocol::MAX_PAYLOAD_SIZE;
        }
        return sendRawPacket(SerialProtocol::SFX_PKT_NACK, (const uint8_t*)reason, len);
    }
    return sendRawPacket(SerialProtocol::SFX_PKT_NACK, nullptr, 0);
}
