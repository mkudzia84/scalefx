/*
 * Serial Protocol - Constants and Utilities
 * 
 * Shared protocol definitions for both binary and text implementations.
 */

#ifndef SERIAL_PROTOCOL_H
#define SERIAL_PROTOCOL_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Protocol Constants & Utilities
// ============================================================================

namespace SerialProtocol {

// Buffer sizes
constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t MAX_PACKET_SIZE = 2 + MAX_PAYLOAD_SIZE + 1;
constexpr size_t COBS_BUFFER_SIZE = MAX_PACKET_SIZE + MAX_PACKET_SIZE / 254 + 2;
constexpr uint8_t FRAME_DELIMITER = 0x00;

// Universal Packet Types (0xF0-0xFF)
constexpr uint8_t SFX_PKT_INIT        = 0xF0;
constexpr uint8_t SFX_PKT_SHUTDOWN    = 0xF1;
constexpr uint8_t SFX_PKT_KEEPALIVE   = 0xF2;
constexpr uint8_t SFX_PKT_INIT_READY  = 0xF3;
constexpr uint8_t SFX_PKT_STATUS      = 0xF4;
constexpr uint8_t SFX_PKT_ERROR       = 0xF5;
constexpr uint8_t SFX_PKT_ACK         = 0xF6;
constexpr uint8_t SFX_PKT_NACK        = 0xF7;
constexpr uint8_t SFX_PKT_REBOOT      = 0xF8;
constexpr uint8_t SFX_PKT_BOOTSEL     = 0xF9;
constexpr uint8_t SFX_PKT_STATUS_REQ  = 0xFA;  // Master requests status

// GunFX-Specific Packet Types (0x01-0x2F)
constexpr uint8_t GUNFX_PKT_TRIGGER_ON      = 0x01;
constexpr uint8_t GUNFX_PKT_TRIGGER_OFF     = 0x02;
constexpr uint8_t GUNFX_PKT_SRV_SET         = 0x10;
constexpr uint8_t GUNFX_PKT_SRV_SETTINGS    = 0x11;
constexpr uint8_t GUNFX_PKT_SRV_RECOIL_JERK = 0x12;
constexpr uint8_t GUNFX_PKT_SMOKE_HEAT      = 0x20;
constexpr uint8_t GUNFX_PKT_SMOKE_SETTINGS  = 0x21;

// Binary protocol functions
uint8_t crc8(const uint8_t* data, size_t len);
size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output);
size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t maxOutput);
size_t buildPacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen);
size_t encodePacket(uint8_t* output, uint8_t type, const uint8_t* payload, size_t payloadLen);
bool parsePacket(const uint8_t* input, size_t length, uint8_t* type, 
                 const uint8_t** payload, size_t* payloadLen);

// Payload encoding helpers
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

// ============================================================================
// Text Protocol Command Names
// ============================================================================

namespace TextCmd {
    constexpr const char* INIT        = "INIT";
    constexpr const char* SHUTDOWN    = "SHUTDOWN";
    constexpr const char* KEEPALIVE   = "KEEPALIVE";
    constexpr const char* READY       = "READY";
    constexpr const char* STATUS      = "STATUS";
    constexpr const char* STATUS_REQ  = "STATUS_REQ";
    constexpr const char* ERROR       = "ERROR";
    constexpr const char* ACK         = "ACK";
    constexpr const char* NACK        = "NACK";
    constexpr const char* REBOOT      = "REBOOT";
    constexpr const char* BOOTSEL     = "BOOTSEL";
    
    // GunFX commands
    constexpr const char* TRIGGER_ON  = "TRIGGER_ON";
    constexpr const char* TRIGGER_OFF = "TRIGGER_OFF";
    constexpr const char* SERVO_SET   = "SERVO_SET";
    constexpr const char* SERVO_CONFIG = "SERVO_CONFIG";
    constexpr const char* SERVO_RECOIL_JERK = "SERVO_RECOIL_JERK";
    constexpr const char* SMOKE_HEAT  = "SMOKE_HEAT";
    constexpr const char* SMOKE_SETTINGS = "SMOKE_SETTINGS";
    constexpr const char* INIT_READY  = "INIT_READY";
}

// Map packet type to text command name
const char* packetTypeToText(uint8_t type);
uint8_t textToPacketType(const char* cmd);

} // namespace SerialProtocol

#endif // SERIAL_PROTOCOL_H
