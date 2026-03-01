/*
 * Serial Bus - COBS-Framed Protocol Implementation
 * 
 * Object-oriented serial communication library for ScaleFX controllers.
 */

#include "serial_bus.h"

// ============================================================================
// Protocol Implementation
// ============================================================================

namespace CoreProtocol {

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output) {
    if (length == 0 || length > MAX_PACKET_SIZE) {
        return 0;
    }
    
    size_t readIndex = 0;
    size_t writeIndex = 1;
    size_t codeIndex = 0;
    uint8_t code = 1;
    
    while (readIndex < length) {
        if (input[readIndex] == 0) {
            output[codeIndex] = code;
            code = 1;
            codeIndex = writeIndex++;
            readIndex++;
        } else {
            output[writeIndex++] = input[readIndex++];
            code++;
            if (code == 0xFF) {
                output[codeIndex] = code;
                code = 1;
                codeIndex = writeIndex++;
            }
        }
    }
    output[codeIndex] = code;
    
    return writeIndex;
}

size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t maxOutput) {
    if (length == 0) {
        return 0;
    }
    
    size_t readIndex = 0;
    size_t writeIndex = 0;
    
    while (readIndex < length) {
        uint8_t code = input[readIndex++];
        if (code == 0) {
            return 0;  // Unexpected zero
        }
        
        for (int i = 1; i < code; i++) {
            if (readIndex >= length || writeIndex >= maxOutput) {
                return 0;
            }
            output[writeIndex++] = input[readIndex++];
        }
        
        if (code < 0xFF && readIndex < length) {
            if (writeIndex >= maxOutput) {
                return 0;
            }
            output[writeIndex++] = 0;
        }
    }
    
    return writeIndex;
}

size_t buildPacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen) {
    if (payloadLen > MAX_PAYLOAD_SIZE) {
        return 0;
    }
    
    output[0] = type;
    output[1] = (uint8_t)payloadLen;
    
    if (payloadLen > 0 && payload != nullptr) {
        memcpy(&output[2], payload, payloadLen);
    }
    
    size_t packetLen = 2 + payloadLen;
    output[packetLen] = crc8(output, packetLen);
    
    return packetLen + 1;
}

size_t encodePacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen) {
    uint8_t raw[MAX_PACKET_SIZE];
    
    size_t rawLen = buildPacket(raw, type, payload, payloadLen);
    if (rawLen == 0) {
        return 0;
    }
    
    size_t encodedLen = cobsEncode(raw, rawLen, output);
    if (encodedLen == 0) {
        return 0;
    }
    
    output[encodedLen] = FRAME_DELIMITER;
    return encodedLen + 1;
}

bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, 
                 const uint8_t** payload, size_t* payloadLen) {
    if (length < 3) {
        return false;
    }
    
    uint8_t pktType = input[0];
    uint8_t pktLen = input[1];
    
    if (length != (size_t)(2 + pktLen + 1)) {
        return false;
    }
    
    uint8_t crc = crc8(input, 2 + pktLen);
    if (crc != input[2 + pktLen]) {
        return false;
    }
    
    *type = pktType;
    *payload = (pktLen > 0) ? &input[2] : nullptr;
    *payloadLen = pktLen;
    
    return true;
}

// Packet type name lookup (for debugging)
const char* packetTypeToText(uint8_t type) {
    switch (type) {
        case CorePacket::INIT:       return "INIT";
        case CorePacket::SHUTDOWN:   return "SHUTDOWN";
        case CorePacket::KEEPALIVE:  return "KEEPALIVE";
        case CorePacket::INIT_READY: return "INIT_READY";
        case CorePacket::STATUS:     return "STATUS";
        case CorePacket::ERROR:      return "ERROR";
        case CorePacket::ACK:        return "ACK";
        case CorePacket::NACK:       return "NACK";
        case CorePacket::REBOOT:     return "REBOOT";
        case CorePacket::BOOTSEL:    return "BOOTSEL";
        case CorePacket::STATUS_REQ: return "STATUS_REQ";
        default:                     return "UNKNOWN";
    }
}

} // namespace CoreProtocol

#ifndef SCALEFX_SERVER
// ============================================================================
// SerialBus Implementation
// ============================================================================

bool SerialBus::begin(UsbHost* usbHost, int deviceIndex) {
    if (_initialized) return true;
    
    _usbHost = usbHost;
    _deviceIndex = deviceIndex;
    _rxIndex = 0;
    _stats = {};
    _initialized = true;
    
    Serial.printf("[SerialBus] Initialized for device %d\n", deviceIndex);
    return true;
}

void SerialBus::end() {
    if (!_initialized) return;
    
    _initialized = false;
    _usbHost = nullptr;
    Serial.println("[SerialBus] Deinitialized");
}

void SerialBus::setDevice(int deviceIndex) {
    _deviceIndex = deviceIndex;
    _rxIndex = 0;
    Serial.printf("[SerialBus] Device changed to %d\n", deviceIndex);
}

int SerialBus::sendPacket(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_initialized || !_usbHost) return -1;
    if (len > CoreProtocol::MAX_PAYLOAD_SIZE) return -1;
    
    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encLen = CoreProtocol::encodePacket(encoded, type, payload, len);
    if (encLen == 0) {
        Serial.printf("[SerialBus] Encode failed for type 0x%02X\n", type);
        return -1;
    }
    
    int written = _usbHost->cdcWrite(_deviceIndex, encoded, encLen);
    if (written < 0) {
        Serial.printf("[SerialBus] Write failed for type 0x%02X\n", type);
        return -1;
    }
    
    _stats.packets_sent++;
    _lastSendMs = millis();  // Track time of successful send
    return 0;
}

int SerialBus::sendKeepalive() {
    return sendPacket(CorePacket::KEEPALIVE);
}

int SerialBus::process() {
    if (!_initialized || !_usbHost) return 0;
    
    int packetsProcessed = 0;
    
    uint8_t readBuf[64];
    int n = _usbHost->cdcRead(_deviceIndex, readBuf, sizeof(readBuf));
    if (n <= 0) return 0;
    
    for (int i = 0; i < n; i++) {
        uint8_t byte = readBuf[i];
        
        if (byte == CoreProtocol::FRAME_DELIMITER) {
            if (_rxIndex > 0) {
                processFrame(_rxBuffer, _rxIndex);
                packetsProcessed++;
            }
            _rxIndex = 0;
        } else {
            if (_rxIndex < SERIAL_BUS_RX_BUFFER_SIZE) {
                _rxBuffer[_rxIndex++] = byte;
            } else {
                _rxIndex = 0;
                _stats.framing_errors++;
            }
        }
    }
    
    return packetsProcessed;
}

void SerialBus::processFrame(const uint8_t* frame, size_t frameLen) {
    if (frameLen < 4) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t decoded[CoreProtocol::MAX_PACKET_SIZE];
    size_t decLen = CoreProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    if (decLen < 3) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t type;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!CoreProtocol::parsePacket(decoded, decLen, &type, &payload, &payloadLen)) {
        _stats.crc_errors++;
        Serial.printf("[SerialBus] Packet parse failed, len=%zu\n", decLen);
        return;
    }
    
    _stats.packets_received++;
    
    if (_rxCallback) {
        _rxCallback(type, payload, payloadLen);
    }
}

void SerialBus::setKeepaliveInterval(unsigned long intervalMs) {
    _keepaliveIntervalMs = intervalMs;
    _lastSendMs = millis();  // Reset send timer
}

bool SerialBus::processKeepalive() {
    if (!_initialized || _keepaliveIntervalMs == 0) return false;
    
    // Only send keepalive if no other message was sent within the interval
    // This ensures at least one message (of any type) per interval
    unsigned long now = millis();
    if (now - _lastSendMs >= _keepaliveIntervalMs) {
        sendKeepalive();
        return true;
    }
    return false;
}

bool SerialBus::isConnected() const {
    if (!_initialized || !_usbHost) return false;
    
    const CdcDeviceInfo* device = _usbHost->getCdcDevice(_deviceIndex);
    return (device != nullptr && device->connected);
}

void SerialBus::resetStats() {
    _stats = {};
}

#endif // !SCALEFX_SERVER
