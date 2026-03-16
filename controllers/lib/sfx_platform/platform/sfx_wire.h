/*
 * SfxWire — Binary Wire Encoding Utilities
 *
 * Stateless algorithms for the ScaleFX binary protocol:
 *   - CRC-8 (polynomial 0x07)
 *   - COBS encode/decode
 *   - Packet build/encode/parse
 *   - Little-endian payload helpers
 *
 * These utilities have NO protocol-semantic knowledge — they operate on raw
 * bytes and don't know about packet types, error codes, or command routing.
 * This makes them suitable for the platform layer where DiagLog and other
 * infrastructure components need wire encoding without depending on the
 * full protocol library (sfx_serial).
 *
 * Protocol:
 *   Packet Format: [type:u8][tag:u8][len:u16LE][payload:0-N bytes][crc8:u8]\n *     N = MAX_PAYLOAD_SIZE (platform-specific: 512 Pico, 2048 ESP32)
 *   Framing: COBS encoded, followed by 0x00 frame delimiter
 *   CRC: CRC-8 polynomial 0x07 over type+tag+len(2 bytes)+payload
 *   Tag: 0x00 = async/unsolicited, 0x01-0xFF = request correlation ID
 *
 * Backward Compatibility:
 *   core.h re-exports everything in this header via `using` declarations
 *   in the CoreProtocol namespace. Existing code using CoreProtocol::crc8(),
 *   CoreProtocol::encodePacket(), etc. continues to compile unchanged.
 */

#ifndef SFX_WIRE_H
#define SFX_WIRE_H

#include <stdint.h>
#include <stddef.h>

namespace SfxWire {

// ============================================================================
// Buffer Size Constants
// ============================================================================

// NOTE: len field is u16LE as of protocol v0.4.0 (was u8, max 64 in v0.3.0).
// Derived buffers (COBS, CommandRouter, processFrame stack) scale with
// MAX_PAYLOAD_SIZE. On ESP32-S3, loopTask stack MUST be >= 16KB
// (set via -DARDUINO_LOOP_STACK_SIZE=16384) when using StreamWriter +
// sendRawPacket, as the combined stack-allocated COBS buffers exceed 4KB.
constexpr size_t HEADER_SIZE = 4;          // type(1) + tag(1) + len(2)

// Platform-specific maximum payload size.
// Determines LOCAL buffer sizes (receive buffers, COBS encode/decode).
//   RP2040/RP2350: 512  — Pico servers handle small commands only
//   ESP32-S3:     2048  — HubFX handles file upload/download chunks
//
// When a client (ESP32) talks to a Pico server, it must limit outgoing
// payloads to the peer's capacity — see PICO_MAX_PAYLOAD below and
// SerialBus::setPeerMaxPayload().
constexpr size_t PICO_MAX_PAYLOAD  = 512;   // RP2040/RP2350 capacity
constexpr size_t ESP32_MAX_PAYLOAD = 2048;  // ESP32-S3 capacity

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
constexpr size_t MAX_PAYLOAD_SIZE = ESP32_MAX_PAYLOAD;
#else
constexpr size_t MAX_PAYLOAD_SIZE = PICO_MAX_PAYLOAD;
#endif

constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + MAX_PAYLOAD_SIZE + 1;  // header + payload + crc
constexpr size_t COBS_BUFFER_SIZE = MAX_PACKET_SIZE + MAX_PACKET_SIZE / 254 + 2;
constexpr uint8_t FRAME_DELIMITER = 0x00;

// Tag constants
constexpr uint8_t TAG_ASYNC = 0x00;  // Unsolicited/async message (no request to match)

// ============================================================================
// CRC-8
// ============================================================================

/**
 * @brief Calculate CRC-8 checksum (polynomial 0x07)
 * @param data Input data
 * @param len Data length
 * @return CRC-8 value
 */
uint8_t crc8(const uint8_t* data, size_t len);

// ============================================================================
// COBS Encode/Decode
// ============================================================================

/**
 * @brief COBS encode data
 * @param input Input data
 * @param length Input length
 * @param output Output buffer (must be at least length + length/254 + 1)
 * @return Encoded length, 0 on error
 */
size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output);

/**
 * @brief COBS decode data
 * @param input Encoded data
 * @param length Encoded length
 * @param output Output buffer
 * @param maxOutput Maximum output size
 * @return Decoded length, 0 on error
 */
size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t maxOutput);

// ============================================================================
// Packet Build / Encode / Parse
// ============================================================================

/**
 * @brief Build raw packet (type + tag + len + payload + crc)
 * @param output Output buffer (must be at least MAX_PACKET_SIZE)
 * @param type Packet type
 * @param tag Request correlation tag (0 = async/unsolicited)
 * @param payload Payload data (may be nullptr if payloadLen == 0)
 * @param payloadLen Payload length
 * @return Packet length including CRC, 0 on error
 */
size_t buildPacket(uint8_t* output, uint8_t type, uint8_t tag,
                   const uint8_t* payload, size_t payloadLen);

/**
 * @brief Build and COBS-encode packet with frame delimiter
 * @param output Output buffer (must be at least COBS_BUFFER_SIZE)
 * @param type Packet type
 * @param tag Request correlation tag (0 = async/unsolicited)
 * @param payload Payload data (may be nullptr if payloadLen == 0)
 * @param payloadLen Payload length
 * @return Total encoded length including delimiter, 0 on error
 */
size_t encodePacket(uint8_t* output, uint8_t type, uint8_t tag,
                    const uint8_t* payload, size_t payloadLen);

/**
 * @brief Parse and verify a decoded packet
 * @param input Decoded packet data
 * @param length Packet length
 * @param type Output packet type
 * @param tag Output request correlation tag
 * @param payload Output pointer to payload (within input buffer)
 * @param payloadLen Output payload length
 * @return true if valid, false on CRC error or malformed
 */
bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, uint8_t* tag,
                 const uint8_t** payload, size_t* payloadLen);

// ============================================================================
// Payload Encoding Helpers (Little-Endian)
// ============================================================================

inline void putU16LE(uint8_t* buf, uint16_t value) {
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
}

inline uint16_t getU16LE(const uint8_t* buf) {
    return buf[0] | ((uint16_t)buf[1] << 8);
}

inline void putI16LE(uint8_t* buf, int16_t value) {
    putU16LE(buf, (uint16_t)value);
}

inline int16_t getI16LE(const uint8_t* buf) {
    return (int16_t)getU16LE(buf);
}

inline void putU32LE(uint8_t* buf, uint32_t value) {
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = (value >> 16) & 0xFF;
    buf[3] = (value >> 24) & 0xFF;
}

inline uint32_t getU32LE(const uint8_t* buf) {
    return buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

} // namespace SfxWire

#endif // SFX_WIRE_H
