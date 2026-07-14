/*
 * esc_decoder.h — the shared surface every ESC telemetry decoder policy
 * implements (concept + normalized data model + wire-format helpers).
 *
 *   Mirrors the BatteryPolicy pattern (sfx_peripherals/power): each
 *   manufacturer protocol is ONE decoder class in its own header next to
 *   this file, satisfying the `EscTelemetryDecoder` concept below;
 *   `EscTelemetryMonitorT<TDecoders...>` (../esc_telemetry_monitor.h)
 *   composes them into one runtime-selected monitor.  Adding a protocol =
 *   write a decoder header + append it to the `EscTelemetryMonitor` alias.
 *
 *   Decoders are PURE parsers: byte in → EscFeed event + EscTelemData out.
 *   No streams, no hub, no logging — that all lives in the monitor.
 */

#ifndef SFX_ESC_DECODER_H
#define SFX_ESC_DECODER_H

#include <cstdint>
#include <cstddef>
#include <concepts>

namespace sfx_peripherals {

/// Wire/config protocol ids — part of the esc-telemetry role's attach config
/// (Rule 11 append-only: new protocols take the next value, never renumber).
enum class EscProtocol : uint8_t {
    Kontronik   = 0,
    Scorpion    = 1,
    HobbywingV4 = 2,
    HobbywingV5 = 3,
};

/// Per-protocol UART defaults (published by each decoder as static traits,
/// collected by the monitor template — see uartParamsFor()).
struct EscUartParams {
    uint32_t baud;
    bool     parityEven;   // false = 8N1, true = 8E1
};

/// Normalized snapshot of the last valid frame (0 = not reported/unknown).
struct EscTelemData {
    uint32_t rpm            = 0;      // as transmitted (electrical for Kontronik)
    uint32_t voltage_mV     = 0;      // pack voltage
    int32_t  current_cA     = 0;      // pack current, 0.01 A units (signed)
    uint32_t capacity_mAh   = 0;      // consumed
    int8_t   throttlePct    = 0;      // -100..100
    uint8_t  outputPct      = 0;      // power-stage opening 0..100
    int16_t  tempEsc_C      = 0;
    int16_t  tempBec_C      = 0;      // BEC (Kontronik/HW5) — 0 when absent
    int16_t  tempMotor_C    = 0;      // HW5 only
    uint32_t becVoltage_mV  = 0;
    uint32_t becCurrent_mA  = 0;
    uint32_t faults         = 0;      // protocol-native fault/error bits (raw)
    // Device identity (Kontronik KODI info frame; others: protocol name only).
    char     deviceName[24] = {};
    uint16_t fwVersion      = 0;      // maj<<8|min when known
};

/// One byte's outcome inside a decoder.
enum class EscFeed : uint8_t {
    None,    ///< mid-frame / hunting — nothing happened
    Live,    ///< a live-data frame completed and refreshed EscTelemData
    Info,    ///< an info/identity frame completed (name/fw/scales updated)
    Error,   ///< a framing/CRC/sanity failure was counted
};

// ============================================================================
// Concept: EscTelemetryDecoder
// ============================================================================
//
// The duck-typed surface EscTelemetryMonitorT requires from every protocol
// decoder.  Static traits describe the wire (protocol id, UART params,
// display name); feed() consumes ONE byte and reports what it produced.
// OPTIONAL capabilities (detected with `requires` at compile time):
//   static constexpr uint32_t kFaultMask;            // benign bits removed
//   static uint8_t faultMessage(uint32_t faults,     // → EX message class
//                               char* out, size_t cap);  // 0 = nothing to say
template <typename T>
concept EscTelemetryDecoder = requires(T t, uint8_t c, EscTelemData& d) {
    { T::kProtocol }   -> std::convertible_to<EscProtocol>;
    { T::kBaud }       -> std::convertible_to<uint32_t>;
    { T::kParityEven } -> std::convertible_to<bool>;
    { T::kName }       -> std::convertible_to<const char*>;
    { t.reset() }      -> std::same_as<void>;
    { t.feed(c, d) }   -> std::same_as<EscFeed>;
};

// ── Shared CRC / little-endian primitives (bitwise — tiny code, no tables) ──
namespace esc_detail {
inline uint32_t crc32r(const uint8_t* p, size_t n) {          // 0xEDB88320
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}
inline uint16_t crc16ccitt(const uint8_t* p, size_t n) {      // reflected 0x8408, init 0
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0x8408u & (0u - (crc & 1u)));
    }
    return crc;
}
inline uint16_t crc16modbus(const uint8_t* p, size_t n) {     // reflected 0xA001, init 0xFFFF
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xA001u & (0u - (crc & 1u)));
    }
    return crc;
}
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
}  // namespace esc_detail

}  // namespace sfx_peripherals

#endif  // SFX_ESC_DECODER_H
