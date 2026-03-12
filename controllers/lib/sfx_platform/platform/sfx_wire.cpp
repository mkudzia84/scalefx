/*
 * SfxWire — Binary Wire Encoding Implementation
 *
 * Stateless protocol utilities: CRC-8, COBS, packet build/encode/parse.
 * See sfx_wire.h for documentation.
 */

#include "sfx_wire.h"
#include <cstring>

namespace SfxWire {

// ============================================================================
// CRC-8 (polynomial 0x07)
// ============================================================================

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

// ============================================================================
// COBS Encode
// ============================================================================

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

// ============================================================================
// COBS Decode
// ============================================================================

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

// ============================================================================
// Build Raw Packet
// ============================================================================

size_t buildPacket(uint8_t* output, uint8_t type, uint8_t tag,
                   const uint8_t* payload, size_t payloadLen) {
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

// ============================================================================
// Build + COBS Encode Packet
// ============================================================================

size_t encodePacket(uint8_t* output, uint8_t type, uint8_t tag,
                    const uint8_t* payload, size_t payloadLen) {
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

// ============================================================================
// Parse + Verify Decoded Packet
// ============================================================================

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

    uint8_t crcVal = crc8(input, HEADER_SIZE + pktLen);
    if (crcVal != input[HEADER_SIZE + pktLen]) {
        return false;
    }

    *type = pktType;
    *tag = pktTag;
    *payload = (pktLen > 0) ? &input[HEADER_SIZE] : nullptr;
    *payloadLen = pktLen;

    return true;
}

} // namespace SfxWire
