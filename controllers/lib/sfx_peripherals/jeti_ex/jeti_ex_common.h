/*
 * Jeti EX Bus — Common protocol definitions
 *
 * Covers the EX Bus frame format, EX telemetry data encoding,
 * CRC functions, and data type definitions.
 *
 * References:
 *   - Jeti Duplex EX Bus Protocol (v1.00+)
 *   - Jeti EX Telemetry Protocol (v1.00)
 *
 * EX Bus frame (from receiver):
 *   [start:u8][type:u8][len:u8][pktId:u8][dataId:u8][subLen:u8][payload...][crc16LE]
 *   (type = 0x01 response-allowed / 0x03 data-only)
 *
 * EX telemetry data block (in a slave response payload), per the in-repo spec
 * docs/EX_Bus_protocol_v1.21_EN.pdf + verified on a real radio (2026-05-30):
 *   [0x9F sep][type|len][USN16LE][LSN16LE][reserved][sensor entries...][crc8]
 *   type bits 7-6: 0b01 (0x40)=data, 0b00=text, 0b10 (0x80)=alarm.  len counts
 *   bytes from type|len through the last data byte (sep + crc8 excluded).
 *   value bytes are little-endian; the HIGHEST byte carries sign|decimals|mag.
 *
 * Channel value encoding: 16-bit unsigned, units of 1/8 µs
 *   Center = 12000 → 1500 µs
 *   Low    = 8000  → 1000 µs
 *   High   = 16000 → 2000 µs
 */

#ifndef SFX_JETI_EX_COMMON_H
#define SFX_JETI_EX_COMMON_H

#include "platform/sfx_platform.h"
#if SFX_PLATFORM_ESP32

#include <Arduino.h>
#include <cstdint>
#include <cstring>

namespace JetiEx {

// ════════════════════════════════════════════════════════════════
//  EX Bus frame constants
// ════════════════════════════════════════════════════════════════

constexpr uint8_t  START_ADDR0      = 0x3E;   ///< Master → slave, address 0
constexpr uint8_t  START_ADDR1      = 0x3D;   ///< Master → slave, address 1
constexpr uint8_t  RESPONSE_HEADER  = 0x3B;   ///< Slave → master (our response)

/// Data identifiers within an EX Bus frame
constexpr uint8_t  DATA_CHANNEL     = 0x31;   ///< Channel data (16 × u16 LE)
constexpr uint8_t  DATA_TELEMETRY   = 0x3A;   ///< Telemetry request / response
constexpr uint8_t  DATA_JETIBOX     = 0x3B;   ///< JetiBox text menu request

/// Maximum EX Bus frame size (header + 32 ch bytes + overhead + CRC)
constexpr uint8_t  MAX_FRAME_SIZE   = 48;

/// Minimum valid frame size (start + len + pktId + dataId + subLen + crc×2)
constexpr uint8_t  MIN_FRAME_SIZE   = 7;

/// EX telemetry data separator byte (the spec's "0xNF"; NOT 0x7E)
constexpr uint8_t  EX_SEPARATOR     = 0x9F;

// ════════════════════════════════════════════════════════════════
//  EX Telemetry sensor data types
// ════════════════════════════════════════════════════════════════

enum class ExDataType : uint8_t {
    Int6     = 0,   ///< 6-bit signed (1 value byte)
    Int14    = 1,   ///< 14-bit signed (2 value bytes)
    Int22    = 4,   ///< 22-bit signed (3 value bytes)
    DateTime = 5,   ///< Date/time (3 value bytes)
    Int30    = 8,   ///< 30-bit signed (4 value bytes)
    Gps      = 9,   ///< GPS coordinate (4 value bytes)
};

/// Number of value bytes for each data type (after the id/type header byte)
inline uint8_t dataTypeSize(ExDataType t) {
    switch (t) {
        case ExDataType::Int6:     return 1;
        case ExDataType::Int14:    return 2;
        case ExDataType::Int22:    return 3;
        case ExDataType::DateTime: return 3;
        case ExDataType::Int30:    return 4;
        case ExDataType::Gps:      return 4;
        default:                   return 0;
    }
}

// ════════════════════════════════════════════════════════════════
//  Sensor value descriptor
// ════════════════════════════════════════════════════════════════

/// Maximum sensor values per device (IDs 0–15 in simple encoding)
constexpr uint8_t MAX_SENSOR_VALUES = 15;

/// Maximum config parameters
constexpr uint8_t MAX_CONFIG_PARAMS = 16;

struct SensorValue {
    uint8_t    id       = 0;          ///< Sensor value ID (0–15)
    ExDataType type     = ExDataType::Int14;
    uint8_t    decimals = 0;          ///< Decimal point position (0–3)
    int32_t    value    = 0;          ///< Current value (integer, scaled by 10^decimals)
    const char* label   = nullptr;    ///< Display label (max 20 chars)
    const char* unit    = nullptr;    ///< Unit string (max 5 chars)
    bool       active   = false;
};

// ════════════════════════════════════════════════════════════════
//  Config parameter descriptor
// ════════════════════════════════════════════════════════════════

enum class ParamType : uint8_t {
    Int8  = 0,
    Int16 = 1,
    Bool  = 2,
    List  = 3,
};

struct ConfigParam {
    uint8_t    id         = 0;
    ParamType  type       = ParamType::Int8;
    int32_t    value      = 0;
    int32_t    minVal     = 0;
    int32_t    maxVal     = 100;
    int32_t    defaultVal = 0;
    const char* label     = nullptr;    ///< Display label (max 16 chars)
    bool       active     = false;
};

// ════════════════════════════════════════════════════════════════
//  CRC functions
// ════════════════════════════════════════════════════════════════

/**
 * @brief CRC-16 for Jeti EX Bus frame validation (RX + TX).
 *
 * REFLECTED CCITT: poly 0x8408 (bit-reversed 0x1021), init 0x0000, LSB-first,
 * no final XOR.  This is the variant Jeti EX Bus actually uses — verified
 * against live receiver frames on the input_monitor bench rig (2026-05-29,
 * crcErr=0 over thousands of frames).  The earlier MSB-first 0x1021 form
 * rejected every real frame (it was never tested on hardware).
 */
inline uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x0001) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408)
                                 : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

/**
 * @brief CRC-8 for EX telemetry data (poly 0x07, init 0x00)
 * Same polynomial as ScaleFX COBS-CRC.
 */
inline uint8_t crc8_ex(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
        }
    }
    return crc;
}

// ════════════════════════════════════════════════════════════════
//  EX Telemetry value encoding helpers
// ════════════════════════════════════════════════════════════════

/**
 * @brief Encode a sensor value into EX data bytes
 *
 * @param buf     Output buffer (must hold at least 5 bytes)
 * @param id      Sensor value ID (0–15)
 * @param type    Data type
 * @param value   Integer value (scaled by 10^decimals)
 * @param dp      Decimal point position (0–3)
 * @return Number of bytes written (header + value bytes)
 */
inline uint8_t encodeSensorValue(uint8_t* buf, uint8_t id, ExDataType type,
                                  int32_t value, uint8_t dp)
{
    uint8_t sign = 0;
    uint32_t mag = static_cast<uint32_t>(value);
    if (value < 0) {
        sign = 1;
        mag = static_cast<uint32_t>(-value);
    }

    buf[0] = (id << 4) | static_cast<uint8_t>(type);

    // Value bytes are little-endian: LOW byte(s) first, the HIGHEST byte last
    // carries sign(bit7) | decimals(bits6-5) | 5 magnitude bits (bits4-0).
    const uint8_t hi = static_cast<uint8_t>((sign << 7) | ((dp & 0x03) << 5));

    switch (type) {
    case ExDataType::Int6:
        buf[1] = hi | (mag & 0x1F);
        return 2;

    case ExDataType::Int14:
        buf[1] = mag & 0xFF;
        buf[2] = hi | ((mag >> 8) & 0x1F);
        return 3;

    case ExDataType::Int22:
        buf[1] = mag & 0xFF;
        buf[2] = (mag >> 8) & 0xFF;
        buf[3] = hi | ((mag >> 16) & 0x1F);
        return 4;

    case ExDataType::Int30:
        buf[1] = mag & 0xFF;
        buf[2] = (mag >> 8) & 0xFF;
        buf[3] = (mag >> 16) & 0xFF;
        buf[4] = hi | ((mag >> 24) & 0x1F);
        return 5;

    default:
        return 0; // GPS, DateTime not implemented yet
    }
}

/**
 * @brief Decode one EX data value entry — inverse of encodeSensorValue().
 *
 * Reads `[id<<4 | type][value bytes]` from `buf` (`avail` bytes available).
 * On success fills id/type/value/dp and sets `consumed` to the entry length.
 * Returns false on truncation or an unsupported type (GPS/DateTime).
 */
inline bool decodeSensorValue(const uint8_t* buf, size_t avail,
                              uint8_t& id, ExDataType& type, int32_t& value,
                              uint8_t& dp, uint8_t& consumed)
{
    if (avail < 1) return false;
    id   = buf[0] >> 4;
    type = static_cast<ExDataType>(buf[0] & 0x0F);
    const uint8_t vbytes = dataTypeSize(type);
    if (vbytes == 0 || type == ExDataType::DateTime || type == ExDataType::Gps)
        return false;                       // only Int6/14/22/30 supported here
    if (avail < 1u + vbytes) return false;

    // Little-endian value: buf[1..vbytes], buf[vbytes] (highest) holds the
    // sign/decimals + top 5 magnitude bits; earlier bytes are lower magnitude.
    const uint8_t  sign = buf[vbytes] >> 7;
    dp = (buf[vbytes] >> 5) & 0x03;
    uint32_t mag = buf[vbytes] & 0x1F;       // top value byte: 5 magnitude bits
    for (uint8_t i = vbytes - 1; i >= 1; --i)
        mag = (mag << 8) | buf[i];
    value    = sign ? -static_cast<int32_t>(mag) : static_cast<int32_t>(mag);
    consumed = static_cast<uint8_t>(1 + vbytes);
    return true;
}

// ════════════════════════════════════════════════════════════════
//  EX telemetry block builders (the 0x9F frame inside a response)
// ════════════════════════════════════════════════════════════════
//
// Both carry the device identity (USN/LSN) so a multi-device expander can
// stamp each frame with the source device — the radio groups by (USN,LSN).
// Layout: [0x9F sep][type|len][USN16LE][LSN16LE][reserved][body...][crc8].
// `len` (low 6 bits of [1]) counts bytes [1..lastbody]; type bits 7-6:
// 0b01 = data, 0b00 = text.  Returns the total block length (incl. crc8).

/// Write the shared 7-byte head; caller fills [1] (type|len) + body, then crc8.
inline uint8_t exBlockHead(uint8_t* buf, uint16_t usn, uint16_t lsn) {
    buf[0] = EX_SEPARATOR;
    buf[2] = usn & 0xFF;  buf[3] = (usn >> 8) & 0xFF;
    buf[4] = lsn & 0xFF;  buf[5] = (lsn >> 8) & 0xFF;
    buf[6] = 0x00;                       // reserved
    return 7;
}

/// One-sensor DATA block.
inline uint8_t buildExDataBlock(uint8_t* buf, uint16_t usn, uint16_t lsn,
                                uint8_t id, ExDataType type, uint8_t dp, int32_t value) {
    uint8_t pos = exBlockHead(buf, usn, lsn);
    pos += encodeSensorValue(&buf[pos], id, type, value, dp);
    buf[1] = 0x40 | (pos - 1);           // data (0b01) | len
    buf[pos] = crc8_ex(&buf[1], pos - 1);
    return (uint8_t)(pos + 1);
}

/// TEXT block — device name when id==0 (unit ignored), else a sensor label.
inline uint8_t buildExTextBlock(uint8_t* buf, uint16_t usn, uint16_t lsn,
                                uint8_t id, const char* label, const char* unit) {
    uint8_t pos = exBlockHead(buf, usn, lsn);
    buf[pos++] = id & 0x0F;
    uint8_t llen = label ? (uint8_t)strlen(label) : 0; if (llen > 20) llen = 20;
    uint8_t ulen = unit  ? (uint8_t)strlen(unit)  : 0; if (ulen > 7)  ulen = 7;
    buf[pos++] = (uint8_t)((llen << 3) | (ulen & 0x07));
    for (uint8_t i = 0; i < llen; ++i) buf[pos++] = (uint8_t)label[i];
    for (uint8_t i = 0; i < ulen; ++i) buf[pos++] = (uint8_t)unit[i];
    buf[1] = 0x00 | (pos - 1);           // text (0b00) | len
    buf[pos] = crc8_ex(&buf[1], pos - 1);
    return (uint8_t)(pos + 1);
}

} // namespace JetiEx

#endif // SFX_PLATFORM_ESP32
#endif // SFX_JETI_EX_COMMON_H
