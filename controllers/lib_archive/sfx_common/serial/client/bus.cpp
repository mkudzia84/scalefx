/*
 * Serial Bus - Client-Side SerialBus Implementation
 * 
 * SerialBus class for HubFX (USB Host CDC communication).
 * CoreProtocol functions are implemented in serial_core.cpp.
 */

#include "bus.h"
#include "serial/core/diag_log.h"

#ifndef SCALEFX_SERVER
// ============================================================================
// SerialBus Implementation
// ============================================================================

bool SerialBus::begin(int deviceIndex) {
    if (_initialized) return true;
    
    _deviceIndex = deviceIndex;
    _rxIndex = 0;
    _stats = {};
    _initialized = true;
    
    SFX_LOG_DEBUG("[SerialBus] Initialized for device %d", deviceIndex);
    return true;
}

void SerialBus::end() {
    if (!_initialized) return;
    
    _initialized = false;
    SFX_LOG_DEBUG("[SerialBus] Deinitialized");
}

void SerialBus::setDevice(int deviceIndex) {
    _deviceIndex = deviceIndex;
    _rxIndex = 0;
    SFX_LOG_DEBUG("[SerialBus] Device changed to %d", deviceIndex);
}

int SerialBus::sendPacket(uint8_t type, const uint8_t* payload, size_t len, uint8_t tag) {
    if (!_initialized) return -1;
    if (len > CoreProtocol::MAX_PAYLOAD_SIZE) return -1;
    
    UsbHost& usb = UsbHost::instance();
    
    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encLen = CoreProtocol::encodePacket(encoded, type, tag, payload, len);
    if (encLen == 0) {
        SFX_LOG_WARN("[SerialBus] Encode failed for type 0x%02X", type);
        return -1;
    }
    
    int written = usb.cdcWrite(_deviceIndex, encoded, encLen);
    if (written < 0) {
        SFX_LOG_WARN("[SerialBus] Write failed for type 0x%02X", type);
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
    if (!_initialized) return 0;
    
    UsbHost& usb = UsbHost::instance();
    int packetsProcessed = 0;
    
    uint8_t readBuf[64];
    int n = usb.cdcRead(_deviceIndex, readBuf, sizeof(readBuf));
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
    if (frameLen < 5) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t decoded[CoreProtocol::MAX_PACKET_SIZE];
    size_t decLen = CoreProtocol::cobsDecode(frame, frameLen, decoded, sizeof(decoded));
    if (decLen < 5) {
        _stats.framing_errors++;
        return;
    }
    
    uint8_t type;
    uint8_t tag;
    const uint8_t* payload;
    size_t payloadLen;
    
    if (!CoreProtocol::parsePacket(decoded, decLen, &type, &tag, &payload, &payloadLen)) {
        _stats.crc_errors++;
        SFX_LOG_WARN("[SerialBus] Packet parse failed, len=%zu", decLen);
        return;
    }
    
    _stats.packets_received++;
    
    if (_rxCallback) {
        _rxCallback(type, tag, payload, payloadLen);
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
    if (!_initialized) return false;
    
    const CdcDeviceInfo* device = UsbHost::instance().getCdcDevice(_deviceIndex);
    return (device != nullptr && device->connected);
}

void SerialBus::resetStats() {
    _stats = {};
}

#endif // !SCALEFX_SERVER
