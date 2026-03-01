/*
 * Serial Core - Implementation
 * 
 * Core command handling and INIT_READY payload encoding/decoding.
 */

#include "serial_core.h"
#include <cstring>

// ============================================================================
// ISerialCore Implementation
// ============================================================================

int ISerialCore::sendInitReady(const CoreBoardInfo& info) {
    uint8_t payload[64];
    size_t len = CorePayload::encodeInitReady(info, payload);
    return sendPacket(CorePacket::INIT_READY, payload, len);
}

// ============================================================================
// CoreCommandServer Implementation
// ============================================================================

void CoreCommandServer::begin(Stream* serial) {
    _serial = serial;
    _initialized = false;
    _lastActivityMs = 0;
}

void CoreCommandServer::setBoardInfo(const char* deviceName, const char* firmwareVersion,
                                       const char* platform, uint32_t cpuMHz, uint32_t freeRam,
                                       uint32_t buildNumber) {
    if (deviceName) {
        strncpy(_boardInfo.deviceName, deviceName, sizeof(_boardInfo.deviceName) - 1);
        _boardInfo.deviceName[sizeof(_boardInfo.deviceName) - 1] = '\0';
    }
    if (firmwareVersion) {
        // Strip "v" or "V" prefix if present
        const char* ver = firmwareVersion;
        if (ver[0] == 'v' || ver[0] == 'V') ver++;
        strncpy(_boardInfo.firmwareVersion, ver, sizeof(_boardInfo.firmwareVersion) - 1);
        _boardInfo.firmwareVersion[sizeof(_boardInfo.firmwareVersion) - 1] = '\0';
    }
    if (platform) {
        strncpy(_boardInfo.platform, platform, sizeof(_boardInfo.platform) - 1);
        _boardInfo.platform[sizeof(_boardInfo.platform) - 1] = '\0';
    }
    _boardInfo.cpuFrequencyMHz = cpuMHz;
    _boardInfo.freeRamBytes = freeRam;
    _boardInfo.buildNumber = buildNumber;
}

bool CoreCommandServer::tryHandle(uint8_t type, const uint8_t* payload, size_t len) {
    _lastActivityMs = millis();
    // Increment with overflow protection (wrap to 1, not 0, to distinguish from reset)
    if (_commandCounter == UINT32_MAX) {
        _commandCounter = 1;
    } else {
        _commandCounter++;
    }
    
    switch (type) {
        case CorePacket::INIT:
            handleInit(payload, len);
            return true;
            
        case CorePacket::SHUTDOWN:
            sendAck();
            if (_shutdownCallback) _shutdownCallback();
            return true;
            
        case CorePacket::REBOOT:
            // No ACK - device reboots immediately
            if (_rebootCallback) _rebootCallback();
            return true;
            
        case CorePacket::BOOTSEL:
            // No ACK - device enters bootloader immediately
            if (_bootselCallback) _bootselCallback();
            return true;
            
        case CorePacket::KEEPALIVE:
            // Keepalive updates activity timer (already done above) and sends ACK
            // Increment with overflow protection (wrap to 1, not 0, to distinguish from reset)
            if (_keepaliveCounter == UINT32_MAX) {
                _keepaliveCounter = 1;
            } else {
                _keepaliveCounter++;
            }
            sendAck();
            if (_keepaliveCallback) _keepaliveCallback();
            return true;
            
        case CorePacket::STATUS_REQ:
            sendStatus();
            return true;
            
        default:
            _commandCounter--;  // Don't count unhandled commands
            return false;
    }
}

bool CoreCommandServer::checkTimeout(unsigned long timeoutMs) {
    if (timeoutMs == 0 || _lastActivityMs == 0) return false;
    
    if (millis() - _lastActivityMs > timeoutMs) {
        if (_initialized) {
            reset();
        }
        return true;
    }
    return false;
}

void CoreCommandServer::reset() {
    _initialized = false;
}

void CoreCommandServer::handleInit(const uint8_t* payload, size_t len) {
    (void)payload;
    (void)len;
    
    // If already initialized, this is a reconnection
    if (_initialized) {
        reset();
    }
    
    // Send INIT_READY response
    sendInitReady();
    
    _initialized = true;
    
    if (_initCallback) {
        _initCallback();
    }
}

void CoreCommandServer::sendInitReady() {
    if (!_serial) return;
    
    uint8_t payload[64];
    size_t len = CorePayload::encodeInitReady(_boardInfo, payload);
    
    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encLen = CoreProtocol::encodePacket(encoded, CorePacket::INIT_READY, payload, len);
    
    if (encLen > 0) {
        _serial->write(encoded, encLen);
    }
}

void CoreCommandServer::sendAck() {
    if (!_serial) return;
    
    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encLen = CoreProtocol::encodePacket(encoded, CorePacket::ACK, nullptr, 0);
    
    if (encLen > 0) {
        _serial->write(encoded, encLen);
    }
}

void CoreCommandServer::sendNack(uint8_t errorCode) {
    if (!_serial) return;
    
    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encLen = CoreProtocol::encodePacket(encoded, CorePacket::NACK, &errorCode, 1);
    
    if (encLen > 0) {
        _serial->write(encoded, encLen);
    }
}

void CoreCommandServer::sendStatus() {
    if (!_serial) return;
    
    // STATUS payload: [counter:u32LE][uptime:u32LE][freeRam:u32LE][moduleData:0-52]
    uint8_t payload[CoreProtocol::MAX_PAYLOAD_SIZE];
    size_t idx = 0;
    
    // Core header (12 bytes)
    CoreProtocol::putU32LE(&payload[idx], _commandCounter);
    idx += 4;
    CoreProtocol::putU32LE(&payload[idx], millis());
    idx += 4;
    CoreProtocol::putU32LE(&payload[idx], _boardInfo.freeRamBytes);
    idx += 4;
    
    // Module-specific data (appended by callback)
    if (_statusDataCallback) {
        size_t moduleLen = _statusDataCallback(&payload[idx], sizeof(payload) - idx);
        idx += moduleLen;
    }
    
    uint8_t encoded[CoreProtocol::COBS_BUFFER_SIZE];
    size_t encLen = CoreProtocol::encodePacket(encoded, CorePacket::STATUS, payload, idx);
    
    if (encLen > 0) {
        _serial->write(encoded, encLen);
    }
}

// ============================================================================
// INIT_READY Payload Encoding/Decoding
// ============================================================================

namespace CorePayload {

/*
 * INIT_READY payload format:
 *   [nameLen:u8][name:N bytes][verLen:u8][version:N bytes]
 *   [platLen:u8][platform:N bytes][cpuMHz:u32LE][ramBytes:u32LE][buildNum:u32LE]
 */

size_t encodeInitReady(const CoreBoardInfo& info, uint8_t* payload) {
    size_t idx = 0;
    
    // Device name (length-prefixed)
    size_t nameLen = strlen(info.deviceName);
    if (nameLen > 31) nameLen = 31;
    payload[idx++] = (uint8_t)nameLen;
    memcpy(&payload[idx], info.deviceName, nameLen);
    idx += nameLen;
    
    // Firmware version (length-prefixed)
    size_t verLen = strlen(info.firmwareVersion);
    if (verLen > 15) verLen = 15;
    payload[idx++] = (uint8_t)verLen;
    memcpy(&payload[idx], info.firmwareVersion, verLen);
    idx += verLen;
    
    // Platform (length-prefixed)
    size_t platLen = strlen(info.platform);
    if (platLen > 31) platLen = 31;
    payload[idx++] = (uint8_t)platLen;
    memcpy(&payload[idx], info.platform, platLen);
    idx += platLen;
    
    // CPU frequency (u32 LE)
    CoreProtocol::putU32LE(&payload[idx], info.cpuFrequencyMHz);
    idx += 4;
    
    // Free RAM (u32 LE)
    CoreProtocol::putU32LE(&payload[idx], info.freeRamBytes);
    idx += 4;
    
    // Build number (u32 LE)
    CoreProtocol::putU32LE(&payload[idx], info.buildNumber);
    idx += 4;
    
    return idx;
}

bool decodeInitReady(const uint8_t* payload, size_t len, CoreBoardInfo& info) {
    if (len < 3) return false;  // Minimum: 3 length bytes
    
    size_t idx = 0;
    
    // Device name
    uint8_t nameLen = payload[idx++];
    if (idx + nameLen > len) return false;
    if (nameLen >= sizeof(info.deviceName)) nameLen = sizeof(info.deviceName) - 1;
    memcpy(info.deviceName, &payload[idx], nameLen);
    info.deviceName[nameLen] = '\0';
    idx += nameLen;
    
    // Firmware version
    if (idx >= len) return false;
    uint8_t verLen = payload[idx++];
    if (idx + verLen > len) return false;
    if (verLen >= sizeof(info.firmwareVersion)) verLen = sizeof(info.firmwareVersion) - 1;
    memcpy(info.firmwareVersion, &payload[idx], verLen);
    info.firmwareVersion[verLen] = '\0';
    idx += verLen;
    
    // Platform
    if (idx >= len) return false;
    uint8_t platLen = payload[idx++];
    if (idx + platLen > len) return false;
    if (platLen >= sizeof(info.platform)) platLen = sizeof(info.platform) - 1;
    memcpy(info.platform, &payload[idx], platLen);
    info.platform[platLen] = '\0';
    idx += platLen;
    
    // CPU frequency, RAM, build number (optional for backwards compatibility)
    if (idx + 4 <= len) {
        info.cpuFrequencyMHz = CoreProtocol::getU32LE(&payload[idx]);
        idx += 4;
    }
    if (idx + 4 <= len) {
        info.freeRamBytes = CoreProtocol::getU32LE(&payload[idx]);
        idx += 4;
    }
    if (idx + 4 <= len) {
        info.buildNumber = CoreProtocol::getU32LE(&payload[idx]);
        idx += 4;
    }
    
    return true;
}

} // namespace CorePayload
