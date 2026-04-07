/*
 * Serial Core - Implementation
 * 
 * packetTypeToText() and INIT_READY payload encoding/decoding.
 * Wire encoding utilities (CRC-8, COBS, packet build/encode/parse) are
 * now in sfx_platform/platform/sfx_wire.cpp (SfxWire namespace).
 * CoreCommandServer is implemented in serial_bus_server.cpp.
 */

#include "core.h"
#include <cstring>

// ============================================================================
// CoreProtocol — Packet Type Name Lookup
// ============================================================================

namespace CoreProtocol {

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
        case CorePacket::IDENTIFY:   return "IDENTIFY";
        case 0xA4:                   return "STREAM_BEGIN";
        case 0xA5:                   return "STREAM_DATA";
        case 0xA6:                   return "STREAM_END";
        default:                     return "UNKNOWN";
    }
}

} // namespace CoreProtocol

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
