/*
 * Serial Core - Implementation
 * 
 * CoreProtocol functions (CRC-8, COBS, packet build/parse) and
 * INIT_READY payload encoding/decoding.
 * CoreCommandServer is implemented in serial_bus_server.cpp.
 */

#include "serial_core.h"
#include <cstring>

// ============================================================================
// CoreProtocol — Binary Protocol Functions
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

size_t buildPacket(uint8_t* output, uint8_t type, uint8_t tag, const uint8_t* payload, size_t payloadLen) {
    if (payloadLen > MAX_PAYLOAD_SIZE) {
        return 0;
    }
    
    output[0] = type;
    output[1] = tag;
    output[2] = (uint8_t)(payloadLen & 0xFF);         // len low byte
    output[3] = (uint8_t)((payloadLen >> 8) & 0xFF);  // len high byte
    
    if (payloadLen > 0 && payload != nullptr) {
        memcpy(&output[HEADER_SIZE], payload, payloadLen);
    }
    
    size_t packetLen = HEADER_SIZE + payloadLen;
    output[packetLen] = crc8(output, packetLen);
    
    return packetLen + 1;
}

size_t encodePacket(uint8_t* output, uint8_t type, uint8_t tag, const uint8_t* payload, size_t payloadLen) {
    uint8_t raw[MAX_PACKET_SIZE];
    
    size_t rawLen = buildPacket(raw, type, tag, payload, payloadLen);
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

bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, uint8_t* tag,
                 const uint8_t** payload, size_t* payloadLen) {
    if (length < 5) {
        return false;  // Minimum: type + tag + len(2) + crc
    }
    
    uint8_t pktType = input[0];
    uint8_t pktTag = input[1];
    uint16_t pktLen = (uint16_t)input[2] | ((uint16_t)input[3] << 8);  // u16LE
    
    if (length != (size_t)(HEADER_SIZE + pktLen + 1)) {
        return false;
    }
    
    uint8_t crc = crc8(input, HEADER_SIZE + pktLen);
    if (crc != input[HEADER_SIZE + pktLen]) {
        return false;
    }
    
    *type = pktType;
    *tag = pktTag;
    *payload = (pktLen > 0) ? &input[HEADER_SIZE] : nullptr;
    *payloadLen = pktLen;
    
    return true;
}

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
