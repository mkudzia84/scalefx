/*
 * kontronik_decoder.h — KONTRONIK ESC telemetry decoder policy.
 *
 *   115200 8E1, 10 ms cadence, unsolicited push-pull broadcast:
 *     "KODL" live frame  40 B — rpm/V/I/mAh/throttle/temps/BEC/faults
 *     "KODI" info frame  44 B — device identity + fw version (+ maximums)
 *   CRC32 (reflected 0xEDB88320) over everything before the trailing u32.
 *
 *   Official spec: ../docs/Kontronik_Telemetrieprotokoll_V5.pdf (2024) —
 *   NOTE the transmitted RPM is ELECTRICAL rpm (divide by pole pairs × gear
 *   ratio for shaft/head speed → the monitor's RPM divider).  The header
 *   sheet's "40 B" only covers KODL; the KODI field sum is 44 B.
 *   Bench-verified on a KOLIBRI 2026-07-14 (100 frames/s, zero errors).
 */

#ifndef SFX_KONTRONIK_DECODER_H
#define SFX_KONTRONIK_DECODER_H

#include <cstdio>
#include <cstring>

#include "esc_decoder.h"

namespace sfx_peripherals {

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

}  // namespace sfx_peripherals

#endif  // SFX_KONTRONIK_DECODER_H
