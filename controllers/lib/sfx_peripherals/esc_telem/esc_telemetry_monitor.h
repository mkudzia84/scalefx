/*
 * esc_telemetry_monitor.h — passive ESC telemetry decoders → TelemetryHub.
 *
 *   One monitor drains an RX-only UART stream carrying a manufacturer ESC
 *   telemetry broadcast, validates frames, and publishes a normalized sensor
 *   set into the shared TelemetryHub — from where it flows to the Jeti
 *   radio (via the JetiExpander responder) AND Studio's Telemetry panel,
 *   exactly like a downstream Jeti EX device.
 *
 *   STRUCTURE (mirrors the BatteryPolicy pattern in sfx_peripherals/power):
 *   each manufacturer protocol is its own DECODER class satisfying the
 *   `EscTelemetryDecoder` concept below; `EscTelemetryMonitorT<TDecoders...>`
 *   composes them into one runtime-selected monitor (std::variant — exactly
 *   one decoder alive).  Adding a protocol = write a decoder + append it to
 *   the `EscTelemetryMonitor` alias; the monitor, role, wire config and UI
 *   pick it up from the pack (no switch statements to extend).
 *
 *   Protocols (all UNSOLICITED broadcasts — the ESC transmits push-pull on
 *   its own; the master side is a bare RX pin, no pull-up, no polling):
 *
 *     KONTRONIK    115200 8E1, 10 ms cadence, "KODL" live (40 B) / "KODI"
 *                  info (44 B) frames, CRC32 (reflected 0xEDB88320).
 *                  Official spec: Kontronik Telemetrieprotokoll V5
 *                  (docs/Kontronik_Telemetrieprotokoll_V5.pdf) — NOTE the
 *                  transmitted RPM is ELECTRICAL rpm (divide by pole pairs ×
 *                  gear ratio for shaft/head speed → the monitor's RPM
 *                  divider).  Bench-verified on a KOLIBRI 2026-07-14.
 *     SCORPION     Tribunus "Unsc Telem" mode: 38400 8N1, 0x55-headed
 *                  22-byte records, CRC16-CCITT (reflected 0x8408 — same
 *                  poly as Jeti).  Field map from the Rotorflight decoder;
 *                  NEEDS BENCH VALIDATION.
 *     HOBBYWING V4 Platinum PRO V4 / FlyFun V5: 19200 8N1, 0x9B-headed
 *                  19-byte data frames, NO checksum (range-sanity checks
 *                  only).  V/I need per-model scale factors delivered in a
 *                  13-byte info frame — until one is seen, V/I read 0 and
 *                  only RPM/throttle/PWM/temps (absolute NTC math) publish.
 *     HOBBYWING V5 Platinum V5: 115200 8N1, FE 01 00 03 30 5C header,
 *                  32-byte frames, CRC16-MODBUS (reflected 0xA001).
 *
 *   Fault propagation: a decoder MAY expose a fault table (kFaultMask +
 *   faultMessage) — the monitor publishes a per-device MESSAGE into the
 *   TelemetryHub on every fault-bits CHANGE; the Jeti responder forwards it
 *   to the radio as an EX Message packet (warning/error popup) and Studio
 *   surfaces it on the Telemetry tab.  Decoders without a table fall back
 *   to a generic "ESC FAULT 0x…" warning.
 *
 *   Field layouts cross-checked against the Rotorflight esc_sensor.c
 *   implementation (the heli-world reference) 2026-07-14.
 */

#ifndef SFX_ESC_TELEMETRY_MONITOR_H
#define SFX_ESC_TELEMETRY_MONITOR_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <concepts>
#include <variant>

#include <platform/sfx_stream.h>
#include <telemetry/telemetry_hub.h>

namespace sfx_peripherals {

/// Wire/config protocol ids — part of the esc-telemetry role's attach config
/// (Rule 11 append-only: new protocols take the next value, never renumber).
enum class EscProtocol : uint8_t {
    Kontronik   = 0,
    Scorpion    = 1,
    HobbywingV4 = 2,
    HobbywingV5 = 3,
};

/// Per-protocol UART defaults (published by each decoder, collected by the
/// monitor template — see uartParamsFor()).
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

// ── Shared CRC primitives (bitwise — tiny code, no tables) ──────────────────
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

// ============================================================================
// KONTRONIK — "KODL" live (40 B) / "KODI" info (44 B), CRC32
// ============================================================================
class KontronikDecoder {
public:
    static constexpr EscProtocol kProtocol   = EscProtocol::Kontronik;
    static constexpr uint32_t    kBaud       = 115200;
    static constexpr bool        kParityEven = true;    // spec: 8E1
    static constexpr const char* kName       = "KONTRONIK";

    /// Operation-error bits we treat as REPORTABLE.  Bit 17 (ProgAllow =
    /// "programming is still allowed") is a benign idle-state flag — it sits
    /// permanently on while the motor is off, so it is masked from both the
    /// Faults sensor and the message path.  (Spec V5, Betriebsfehler table.)
    static constexpr uint32_t kFaultMask = 0x00FDFFFFu;   // bits 0..23 minus 17

    /// Map fault bits → radio message.  Returns the EX message class
    /// (2 = warning, 3 = recoverable error, 4 = non-recoverable error;
    /// 0 = nothing reportable).  Text is the HIGHEST-severity fault, with a
    /// "+N" suffix when more bits are set.  Table: Kontronik spec V5.
    static uint8_t faultMessage(uint32_t faults, char* out, size_t cap) {
        struct F { uint8_t bit; uint8_t cls; const char* text; };
        static constexpr F kTab[] = {
            {0,  4, "ESC UNDERVOLTAGE"},   // battery undervoltage
            {1,  4, "ESC OVERVOLTAGE"},    // battery overvoltage
            {2,  4, "ESC OVERCURRENT"},    // overcurrent integral error
            {3,  2, "ESC CURRENT WARN"},   // overcurrent integral warning
            {4,  2, "ESC TEMP WARN"},      // power-stage overtemp warning
            {5,  4, "ESC OVERTEMP"},       // power-stage overtemp error
            {6,  3, "BEC UNDERVOLTAGE"},
            {7,  3, "BEC OVERVOLTAGE"},
            {8,  3, "BEC OVERCURRENT"},
            {9,  3, "BEC OVERTEMP"},
            {10, 2, "ESC SHUTDOWN"},       // rundown shutdown (speed control)
            {11, 2, "CAPACITY LIMIT"},     // preset discharge capacity reached
            {12, 3, "ESC OPERATION ERR"},
            {13, 2, "ESC OPERATION WARN"},
            {14, 3, "ESC SELFTEST ERR"},
            {15, 3, "ESC EEPROM ERR"},
            {16, 3, "ESC WATCHDOG"},
            // 17 ProgAllow — masked (benign)
            {18, 2, "BATT U-MIN LIMIT"},   // TelMe preset undervoltage
            {19, 2, "CURRENT LIMIT"},      // TelMe preset overcurrent
            {20, 2, "ESC TEMP LIMIT"},     // TelMe preset ESC overtemp
            {21, 2, "BEC TEMP LIMIT"},     // TelMe preset BEC overtemp
            {22, 2, "BEC CURRENT LIMIT"},  // TelMe preset BEC overcurrent
            {23, 2, "DISCHARGE LIMIT"},    // TelMe preset discharge capacity
        };
        faults &= kFaultMask;
        if (!faults || !cap) return 0;
        const F* top = nullptr;
        uint8_t n = 0;
        for (const auto& f : kTab) {
            if (!(faults & (1u << f.bit))) continue;
            ++n;
            if (!top || f.cls > top->cls) top = &f;
        }
        if (!top) return 0;
        if (n > 1) std::snprintf(out, cap, "%s +%u", top->text, (unsigned)(n - 1));
        else       std::snprintf(out, cap, "%s", top->text);
        return top->cls;
    }

    void reset() { _len = 0; }

    EscFeed feed(uint8_t c, EscTelemData& d) {
        using namespace esc_detail;
        _buf[_len++] = c;
        // Hold sync until the 4-byte magic matches.
        static const uint8_t kMagic[3] = {'K', 'O', 'D'};
        if (_len <= 3) {
            if (_buf[_len - 1] != kMagic[_len - 1]) { _len = 0; if (c == 'K') _buf[_len++] = c; }
            return EscFeed::None;
        }
        if (_len == 4 && _buf[3] != 'L' && _buf[3] != 'I') { _len = 0; return EscFeed::None; }
        // KODL live = 40 B; KODI info = 44 B (spec V5 field sum; bench-verified
        // on a KOLIBRI 2026-07-14 — CRC32 sits at need-4..need-1).
        const size_t need = (_buf[3] == 'I') ? kKodiLen : kKodlLen;
        if (_len < need) return EscFeed::None;

        // CRC32 over everything before the trailing u32, stored LE (accept BE
        // too — the vendor sample code emits big-endian byte order).
        const uint32_t calc = crc32r(_buf, need - 4);
        const uint32_t le = rd32(&_buf[need - 4]);
        const uint32_t be = ((uint32_t)_buf[need - 4] << 24) | ((uint32_t)_buf[need - 3] << 16)
                          | ((uint32_t)_buf[need - 2] << 8) | _buf[need - 1];
        if (calc != le && calc != be) { resync(); return EscFeed::Error; }

        EscFeed ev;
        if (_buf[3] == 'L') {
            d.rpm           = rd32(&_buf[4]);                    // ELECTRICAL rpm
            d.voltage_mV    = (uint32_t)rd16(&_buf[8]) * 10u;    // 10 mV units
            d.current_cA    = (int16_t)rd16(&_buf[10]) * 10;     // 0.1 A → cA
            d.capacity_mAh  = rd16(&_buf[16]);
            d.becCurrent_mA = rd16(&_buf[18]);
            d.becVoltage_mV = rd16(&_buf[20]);
            d.throttlePct   = (int8_t)_buf[24];
            d.outputPct     = _buf[25];
            d.tempEsc_C     = (int8_t)_buf[26];
            d.tempBec_C     = (int8_t)_buf[27];
            d.faults        = rd32(&_buf[28]);
            ev = EscFeed::Live;
        } else {                                     // 'I' — device info
            const uint16_t devVar = rd16(&_buf[4]);
            d.fwVersion = rd16(&_buf[6]);
            static const char* kDev[] = {"KONTRONIK", "KOSMIK", "KOLIBRI", "JIVEPro", "KONTROL-X", "UHV"};
            const uint8_t devIdx = (uint8_t)(devVar >> 10);
            std::snprintf(d.deviceName, sizeof(d.deviceName), "%s",
                          devIdx < 6 ? kDev[devIdx] : "KONTRONIK");
            ev = EscFeed::Info;
        }
        _len = 0;
        return ev;
    }

private:
    void resync() {
        // Shift-resync: drop the first byte, keep the tail — so a mid-stream
        // join never needs a full frame gap to lock on.
        std::memmove(_buf, _buf + 1, --_len);
    }
    static constexpr size_t kKodlLen = 40;
    static constexpr size_t kKodiLen = 44;
    uint8_t _buf[48] = {};
    size_t  _len = 0;
};

// ============================================================================
// SCORPION Tribunus "Unsc Telem" — 0x55 header, 22 B, CRC16-CCITT
// ============================================================================
// Layout per the Rotorflight decoder (NEEDS BENCH VALIDATION):
//   [0]=0x55 hdr, [4..5] throttle (0.5 %), [8..9] current (0.1 A),
//   [10..11] voltage (0.1 V), [12..13] mAh, [14] temp °C,
//   [15] PWM (0.5 %), [16] BEC V (0.1 V), [17..18] rpm (×5),
//   [19] error, [20..21] CRC16-CCITT LE over [0..19].
class ScorpionDecoder {
public:
    static constexpr EscProtocol kProtocol   = EscProtocol::Scorpion;
    static constexpr uint32_t    kBaud       = 38400;
    static constexpr bool        kParityEven = false;
    static constexpr const char* kName       = "SCORPION";

    void reset() { _len = 0; }

    EscFeed feed(uint8_t c, EscTelemData& d) {
        using namespace esc_detail;
        if (_len == 0 && c != 0x55) return EscFeed::None;
        _buf[_len++] = c;
        if (_len < kLen) return EscFeed::None;
        const uint16_t calc = crc16ccitt(_buf, kLen - 2);
        if (calc != rd16(&_buf[20])) {
            std::memmove(_buf, _buf + 1, --_len);
            return EscFeed::Error;
        }
        d.throttlePct   = (int8_t)(rd16(&_buf[4]) / 2);
        d.current_cA    = (int16_t)rd16(&_buf[8]) * 10;
        d.voltage_mV    = (uint32_t)rd16(&_buf[10]) * 100u;
        d.capacity_mAh  = rd16(&_buf[12]);
        d.tempEsc_C     = (int8_t)_buf[14];
        d.outputPct     = (uint8_t)(_buf[15] / 2);
        d.becVoltage_mV = (uint32_t)_buf[16] * 100u;
        d.rpm           = (uint32_t)rd16(&_buf[17]) * 5u;
        d.faults        = _buf[19];
        _len = 0;
        return EscFeed::Live;
    }

private:
    static constexpr size_t kLen = 22;
    uint8_t _buf[24] = {};
    size_t  _len = 0;
};

// ============================================================================
// HOBBYWING V4 — 0x9B header, 19 B data / 13 B info (0xB9 footer), NO CRC
// ============================================================================
class HobbywingV4Decoder {
public:
    static constexpr EscProtocol kProtocol   = EscProtocol::HobbywingV4;
    static constexpr uint32_t    kBaud       = 19200;
    static constexpr bool        kParityEven = false;
    static constexpr const char* kName       = "HOBBYWING4";

    void reset() { _len = 0; _haveScale = false; }

    EscFeed feed(uint8_t c, EscTelemData& d) {
        if (_len == 0 && c != 0x9B) return EscFeed::None;
        _buf[_len++] = c;
        if (_len == kInfoLen && _buf[kInfoLen - 1] == 0xB9) {
            // 13-byte info frame (footer 0xB9): carries the per-model V/I
            // scale factors (Rotorflight derivation).
            const float vs = (float)_buf[5] / ((float)_buf[6] * 10.0f);
            const float cs = _buf[8] ? (float)_buf[7] / (float)_buf[8] : 0.0f;
            if (vs > 0.0f) {
                _voltScale = vs;
                _curScale  = cs;
                _curOffset = cs > 0.0f ? (float)_buf[9] / cs : 0.0f;
                _haveScale = true;
            }
            _len = 0;
            return EscFeed::Info;
        }
        if (_len < kDataLen) return EscFeed::None;
        // Range-sanity validation (the protocol has no checksum).
        if (!(_buf[4] < 4 && _buf[6] < 4 && _buf[11] < 0x10 && _buf[13] < 0x10 &&
              _buf[15] < 0x10 && _buf[17] < 0x10)) {
            std::memmove(_buf, _buf + 1, --_len);
            return EscFeed::Error;
        }
        const uint16_t thr  = (uint16_t)((_buf[4] << 8) | _buf[5]);     // big-endian!
        const uint16_t pwm  = (uint16_t)((_buf[6] << 8) | _buf[7]);
        d.rpm         = ((uint32_t)_buf[8] << 16) | ((uint32_t)_buf[9] << 8) | _buf[10];
        d.throttlePct = (int8_t)((thr > 1024 ? 1024 : thr) * 100 / 1024);
        d.outputPct   = (uint8_t)((pwm > 1024 ? 1024 : pwm) * 100 / 1024);
        const uint16_t vAdc = (uint16_t)((_buf[11] << 8) | _buf[12]);
        const uint16_t iAdc = (uint16_t)((_buf[13] << 8) | _buf[14]);
        if (_haveScale) {
            d.voltage_mV = (uint32_t)(vAdc * _voltScale * 1000.0f);
            const float amps = (iAdc > _curOffset && _curScale > 0.0f)
                             ? (iAdc - _curOffset) * _curScale : 0.0f;
            d.current_cA = (int32_t)(amps * 100.0f);
        }
        d.tempEsc_C = ntc((uint16_t)((_buf[15] << 8) | _buf[16]));
        d.tempBec_C = ntc((uint16_t)((_buf[17] << 8) | _buf[18]));   // CAP temp slot
        _len = 0;
        return EscFeed::Live;
    }

private:
    static int16_t ntc(uint16_t adc) {
        // Rotorflight constants (β-model): absolute — no calibration needed.
        constexpr float kGamma = 0.00025316455696f;
        constexpr float kDelta = 0.00296226896087f;
        float x = adc < 1 ? 1.0f : (adc > 4095 ? 4095.0f : (float)adc);
        const float r = x / (4096.0f - x);
        const float a = std::log(r) * kGamma + kDelta;
        return (int16_t)(1.0f / a - 273.15f);
    }
    static constexpr size_t kDataLen = 19;
    static constexpr size_t kInfoLen = 13;
    uint8_t _buf[24] = {};
    size_t  _len = 0;
    // Info-frame scale factors (per-model).
    bool  _haveScale = false;
    float _voltScale = 0.0f, _curScale = 0.0f, _curOffset = 0.0f;
};

// ============================================================================
// HOBBYWING V5 — FE 01 00 03 30 5C header, 32 B, CRC16-MODBUS
// ============================================================================
class HobbywingV5Decoder {
public:
    static constexpr EscProtocol kProtocol   = EscProtocol::HobbywingV5;
    static constexpr uint32_t    kBaud       = 115200;
    static constexpr bool        kParityEven = false;
    static constexpr const char* kName       = "HOBBYWING5";

    void reset() { _len = 0; }

    EscFeed feed(uint8_t c, EscTelemData& d) {
        using namespace esc_detail;
        static const uint8_t kHdr[6] = {0xFE, 0x01, 0x00, 0x03, 0x30, 0x5C};
        if (_len < 6) {
            if (c != kHdr[_len]) { _len = (c == kHdr[0]) ? 1 : 0; if (_len) _buf[0] = c; return EscFeed::None; }
            _buf[_len++] = c;
            return EscFeed::None;
        }
        _buf[_len++] = c;
        if (_len < kLen) return EscFeed::None;
        const uint16_t calc = crc16modbus(_buf, kLen - 2);
        if (calc != rd16(&_buf[kLen - 2])) { _len = 0; return EscFeed::Error; }
        d.throttlePct   = (int8_t)_buf[9];
        d.faults        = _buf[12];
        d.rpm           = (uint32_t)rd16(&_buf[13]) * 10u;
        d.voltage_mV    = (uint32_t)rd16(&_buf[15]) * 100u;
        d.current_cA    = (int32_t)rd16(&_buf[17]) * 10;
        d.tempEsc_C     = (int8_t)_buf[19];
        d.tempBec_C     = (int8_t)_buf[20];
        d.tempMotor_C   = (int8_t)_buf[21];
        d.becVoltage_mV = (uint32_t)_buf[22] * 100u;
        d.becCurrent_mA = (uint32_t)_buf[23] * 100u;
        _len = 0;
        return EscFeed::Live;
    }

private:
    static constexpr size_t kLen = 32;
    uint8_t _buf[36] = {};
    size_t  _len = 0;
};

// ============================================================================
// EscTelemetryMonitorT — the composed monitor
// ============================================================================
template <EscTelemetryDecoder... TDecoders>
class EscTelemetryMonitorT {
public:
    /// UART params for a wire protocol id — folded out of the decoder pack
    /// (Rule 33: recovered from the carrier types, no parallel table).
    static bool uartParamsFor(uint8_t protocol, EscUartParams& out) {
        bool found = false;
        (void)(((uint8_t)TDecoders::kProtocol == protocol
                ? (out = EscUartParams{TDecoders::kBaud, TDecoders::kParityEven}, found = true)
                : false) || ...);
        return found;
    }

    /// Display name for a wire protocol id ("ESC" when unknown).
    static const char* protocolName(uint8_t protocol) {
        const char* name = "ESC";
        (void)(((uint8_t)TDecoders::kProtocol == protocol
                ? (name = TDecoders::kName, true) : false) || ...);
        return name;
    }

    /// @return false when no decoder in the pack claims `protocol`.
    bool begin(sfx::Stream* s, uint8_t protocol) {
        _s = s;
        _frames = _errors = _sensorPushes = 0;
        _rxBytes = 0;
        _snapLen = 0;
        _hubDev = 0xFF;
        _lastPushMs = 0;
        _lastFaults = 0;
        _proto = protocol;
        _d = EscTelemData{};
        bool ok = false;
        (void)(((uint8_t)TDecoders::kProtocol == protocol
                ? (_dec.template emplace<TDecoders>(), ok = true) : false) || ...);
        if (!ok) { _dec.template emplace<std::monostate>(); return false; }
        std::visit([](auto& dec) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(dec)>, std::monostate>) dec.reset();
        }, _dec);
        std::snprintf(_d.deviceName, sizeof(_d.deviceName), "%s", protocolName(protocol));
        return true;
    }

    void end() {
        // Expire our hub device promptly so the radio/Studio drop stale rows.
        _s = nullptr;
        _hubDev = 0xFF;
        _dec.template emplace<std::monostate>();
    }

    /// RPM divider ×100 applied at publish (pole pairs × gear ratio) — the
    /// Kontronik stream carries ELECTRICAL rpm; other protocols motor rpm.
    /// 100 = 1.00 (as transmitted).  0 is coerced to 100.
    void setRpmDividerX100(uint16_t x100) { _rpmDivX100 = x100 ? x100 : 100; }
    uint16_t rpmDividerX100() const { return _rpmDivX100; }

    /// Drain + parse.  Cheap: byte-level sync hunt, bounded per pass.
    void update(uint32_t nowMs) {
        if (!_s) return;
        int guard = 256;                     // bound one pass (Rule: no unbounded drains)
        while (guard-- > 0 && _s->available() > 0) {
            const int c = _s->read();
            if (c < 0) break;
            _rxBytes++;
            if (_snapLen < sizeof(_snap)) _snap[_snapLen++] = (uint8_t)c;
            const EscFeed ev = std::visit([&](auto& dec) -> EscFeed {
                if constexpr (std::is_same_v<std::decay_t<decltype(dec)>, std::monostate>) return EscFeed::None;
                else return dec.feed((uint8_t)c, _d);
            }, _dec);
            switch (ev) {
                case EscFeed::Live:  _frames++; publish(nowMs); break;
                case EscFeed::Info:  _frames++; break;
                case EscFeed::Error: _errors++; break;
                case EscFeed::None:  break;
            }
        }
    }

    // Diagnostics
    uint32_t rxBytes() const { return _rxBytes; }
    /// Copy-and-reset the first-bytes snapshot of the current window (bench
    /// diagnostics — lets the instrumentation log show RAW line bytes).
    uint8_t takeSnap(uint8_t* out, uint8_t cap) {
        const uint8_t n = (_snapLen < cap) ? _snapLen : cap;
        for (uint8_t i = 0; i < n; ++i) out[i] = _snap[i];
        _snapLen = 0;
        return n;
    }
    uint32_t frames()  const { return _frames; }
    uint32_t errors()  const { return _errors; }
    uint32_t pushes()  const { return _sensorPushes; }
    const EscTelemData& data() const { return _d; }

private:
    // ── Hub publication (radio + Studio, throttled) ───────────────────
    void publish(uint32_t nowMs) {
        if (nowMs - _lastPushMs < kPushIntervalMs) return;
        _lastPushMs = nowMs;
        _sensorPushes++;

        auto& hub = sfx_telemetry::TelemetryHub::instance();
        sfx_telemetry::TelemetryHub::ScopedLock lk(hub);
        using sfx_telemetry::SensorKind;
        // Synthetic identity: usn tags the ESC family, lsn the protocol —
        // unique against real Jeti devices on the same radio.  Re-upsert every
        // push: refreshes the device's lastMs AND its NAME, which upgrades
        // from the protocol default to the real identity once the info frame
        // decodes (Kontronik KODI → "KOLIBRI").
        const bool first = (_hubDev == 0xFF);
        _hubDev = hub.upsertDevice((uint16_t)(0xE5C0u | (uint16_t)_proto),
                                   0x0001, _d.deviceName, /*local=*/false, nowMs);
        if (_hubDev == 0xFF) return;
        if (first) {
            hub.setSensor(_hubDev, 1, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 1, "RPM", "rpm");
            hub.setSensor(_hubDev, 2, SensorKind::Int, 2, 0, nowMs); hub.setLabel(_hubDev, 2, "Voltage", "V");
            hub.setSensor(_hubDev, 3, SensorKind::Int, 1, 0, nowMs); hub.setLabel(_hubDev, 3, "Current", "A");
            hub.setSensor(_hubDev, 4, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 4, "Used", "mAh");
            hub.setSensor(_hubDev, 5, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 5, "Throttle", "%");
            hub.setSensor(_hubDev, 6, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 6, "Temp ESC", "\xB0""C");
            hub.setSensor(_hubDev, 7, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 7, "Temp BEC", "\xB0""C");
            hub.setSensor(_hubDev, 8, SensorKind::Int, 1, 0, nowMs); hub.setLabel(_hubDev, 8, "BEC U", "V");
            hub.setSensor(_hubDev, 9, SensorKind::Int, 0, 0, nowMs); hub.setLabel(_hubDev, 9, "Faults", "");
        }
        // RPM divider: transmitted (electrical/motor) rpm → shaft/head rpm.
        const uint32_t rpmOut = (uint32_t)(((uint64_t)_d.rpm * 100u) / _rpmDivX100);

        // Fault reporting: mask decoder-declared benign bits, then publish
        // the masked value as the Faults sensor AND — on CHANGE — a device
        // MESSAGE (text + class) the Jeti responder forwards to the radio.
        uint32_t masked = _d.faults;
        char     msg[24] = {};
        uint8_t  cls = 0;
        std::visit([&](auto& dec) {
            using D = std::decay_t<decltype(dec)>;
            if constexpr (!std::is_same_v<D, std::monostate>) {
                if constexpr (requires { D::kFaultMask; }) masked &= D::kFaultMask;
                if (masked) {
                    if constexpr (requires { D::faultMessage(0u, msg, sizeof msg); }) {
                        cls = D::faultMessage(masked, msg, sizeof msg);
                    } else {
                        std::snprintf(msg, sizeof msg, "ESC FAULT %04lX", (unsigned long)masked);
                        cls = 2;   // generic warning
                    }
                }
            }
        }, _dec);
        if (masked != _lastFaults) {
            _lastFaults = masked;
            hub.setMessage(_hubDev, cls, masked ? msg : "", nowMs);
        }

        hub.setSensor(_hubDev, 1, SensorKind::Int, 0, (int32_t)rpmOut, nowMs);
        hub.setSensor(_hubDev, 2, SensorKind::Int, 2, (int32_t)(_d.voltage_mV / 10), nowMs);
        hub.setSensor(_hubDev, 3, SensorKind::Int, 1, _d.current_cA / 10, nowMs);
        hub.setSensor(_hubDev, 4, SensorKind::Int, 0, (int32_t)_d.capacity_mAh, nowMs);
        hub.setSensor(_hubDev, 5, SensorKind::Int, 0, _d.throttlePct, nowMs);
        hub.setSensor(_hubDev, 6, SensorKind::Int, 0, _d.tempEsc_C, nowMs);
        hub.setSensor(_hubDev, 7, SensorKind::Int, 0, _d.tempBec_C, nowMs);
        hub.setSensor(_hubDev, 8, SensorKind::Int, 1, (int32_t)(_d.becVoltage_mV / 100), nowMs);
        hub.setSensor(_hubDev, 9, SensorKind::Int, 0, (int32_t)masked, nowMs);
    }

    static constexpr uint32_t kPushIntervalMs = 500;   // hub/radio refresh (2 Hz)

    sfx::Stream* _s = nullptr;
    uint8_t      _proto = 0xFF;
    std::variant<std::monostate, TDecoders...> _dec;
    uint32_t     _frames = 0, _errors = 0, _sensorPushes = 0;
    uint32_t     _rxBytes = 0;
    uint8_t      _snap[16];
    uint8_t      _snapLen = 0;
    uint32_t     _lastPushMs = 0;
    uint32_t     _lastFaults = 0;
    uint16_t     _rpmDivX100 = 100;
    uint8_t      _hubDev = 0xFF;
    EscTelemData _d;
};

/// The shipped protocol set — extend by appending a decoder type.
using EscTelemetryMonitor = EscTelemetryMonitorT<KontronikDecoder,
                                                 ScorpionDecoder,
                                                 HobbywingV4Decoder,
                                                 HobbywingV5Decoder>;

}  // namespace sfx_peripherals

#endif  // SFX_ESC_TELEMETRY_MONITOR_H
