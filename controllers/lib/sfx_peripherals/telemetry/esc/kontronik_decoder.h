/*
 * kontronik_decoder.h — KONTRONIK ESC telemetry decoder policy.
 *
 *   115200 8E1, 10 ms cadence, unsolicited push-pull broadcast:
 *     "KODL" live frame — rpm/V/I/mAh/throttle/temps/BEC/faults
 *     "KODI" info frame — device identity + fw version (+ maximums)
 *   CRC32 (reflected 0xEDB88320) over everything before the trailing u32.
 *
 *   FRAME LENGTH AND CRC REGION ARE DEVICE-DEPENDENT (spec V5 column
 *   marks + 2026-08-12 bench raw capture of a live JIVE PRO):
 *     KOSMIK / KOLIBRI:  KODL 40 B, KODI 44 B — CRC over frame minus
 *                        trailer (the spec's sample code).
 *     JIVE PRO:          KODL 38 B (Reserved1/2 omitted; stride verified
 *                        against back-to-back magics in a raw 48 B capture)
 *                        with the CRC computed over only the FIRST 32
 *                        bytes — the state + timing bytes before the
 *                        trailer are NOT covered (firmware quirk, verified
 *                        bit-perfect against two captured frames).  KODI is
 *                        41 B (Reserved1/2/3 omitted) with the CRC over the
 *                        first 36 bytes (max-BEC-temp uncovered) — measured
 *                        from a live badframe capture, fw 1.14.
 *   Field offsets are the spec layout in every variant (nothing shifts).
 *   We validate (length, region) candidate pairs as bytes arrive and lock
 *   onto whichever matches (the fixed 40 B assumption rejected 100 %% of
 *   JIVE PRO frames — errs tracked rxB byte-for-byte).
 *
 *   Official spec: ../docs/Kontronik_Telemetrieprotokoll_V5.pdf (2024) —
 *   NOTE the transmitted RPM is ELECTRICAL rpm (divide by pole pairs × gear
 *   ratio for shaft/head speed → the monitor's RPM divider).
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
        // Texts stay <= 15 chars so the " +N" multi-fault suffix always
        // fits the 18-char EX Message cap (spec packet <= 29 B) — short but
        // human readable on the transmitter.
        static constexpr F kTab[] = {
            {0,  4, "ESC UNDERVOLT"},      // battery undervoltage
            {1,  4, "ESC OVERVOLT"},       // battery overvoltage
            {2,  4, "ESC OVERCURRENT"},    // overcurrent integral error
            {3,  2, "ESC CURR WARN"},      // overcurrent integral warning
            {4,  2, "ESC TEMP WARN"},      // power-stage overtemp warning
            {5,  4, "ESC OVERTEMP"},       // power-stage overtemp error
            {6,  3, "BEC UNDERVOLT"},
            {7,  3, "BEC OVERVOLT"},
            {8,  3, "BEC OVERCURRENT"},
            {9,  3, "BEC OVERTEMP"},
            {10, 2, "ESC SHUTDOWN"},       // rundown shutdown (speed control)
            {11, 2, "CAPACITY LIMIT"},     // preset discharge capacity reached
            {12, 3, "ESC OPER ERROR"},
            {13, 2, "ESC OPER WARN"},
            {14, 3, "ESC SELFTEST"},       // self-test error found
            {15, 3, "ESC EEPROM ERR"},
            {16, 3, "ESC WATCHDOG"},
            // 17 ProgAllow — masked (benign)
            {18, 2, "BATT U LIMIT"},       // TelMe preset undervoltage
            {19, 2, "CURRENT LIMIT"},      // TelMe preset overcurrent
            {20, 2, "ESC TEMP LIMIT"},     // TelMe preset ESC overtemp
            {21, 2, "BEC TEMP LIMIT"},     // TelMe preset BEC overtemp
            {22, 2, "BEC CURR LIMIT"},     // TelMe preset BEC overcurrent
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

    void reset() { _len = 0; _badLen = 0; }

    /// Diagnostic: copy + clear the first CRC-rejected KODI frame seen since
    /// the last call (0 = none).  Lets the instrumentation dump unknown
    /// device variants without guessing.
    size_t takeBadFrame(uint8_t* out, size_t cap) {
        size_t n = (_badLen < cap) ? _badLen : cap;
        for (size_t i = 0; i < n; ++i) out[i] = _bad[i];
        _badLen = 0;
        return n;
    }

    EscFeed feed(uint8_t c, EscTelemData& d) {
        using namespace esc_detail;
        if (_len >= sizeof(_buf)) _len = 0;           // can't happen — bounded below
        _buf[_len++] = c;

        // Re-anchor: the buffer must START with a magic prefix.  The old
        // `_len == cand` candidate test only fired when the byte count
        // happened to EQUAL a candidate at an aligned moment — after one
        // failure the window pinned full and slid one byte per feed, so the
        // shorter candidate was never tested again (the 2026-08-12 JIVE PRO
        // regression's second half).  Anchoring + testing every candidate
        // ≤ _len each feed is immune to window phase.
        hunt();
        if (_len < 4) return EscFeed::None;

        const bool  info  = (_buf[3] == 'I');
        const Cand* cands = info ? kKodiCands : kKodlCands;
        const size_t nCand = info ? kNumKodi  : kNumKodl;
        for (size_t i = 0; i < nCand; ++i) {
            const size_t n = cands[i].frameLen;
            if (_len < n || !crcOk(n, cands[i].crcLen)) continue;
            if (info) {
                const uint16_t devVar = rd16(&_buf[4]);
                d.fwVersion = rd16(&_buf[6]);
                static const char* kDev[] = {"KONTRONIK", "KOSMIK", "KOLIBRI", "JIVE PRO", "KONTROL X", "UHV"};
                const uint8_t devIdx = (uint8_t)(devVar >> 10);
                std::snprintf(d.deviceName, sizeof(d.deviceName), "%s",
                              devIdx < 6 ? kDev[devIdx] : "KONTRONIK");
            } else {
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
            }
            consume(n);          // keep any tail — it's the next frame's head
            return info ? EscFeed::Info : EscFeed::Live;
        }
        if (_len >= cands[nCand - 1].frameLen) {
            // Aligned magic but no candidate CRC-validates — corrupt frame.
            // Keep the first rejected INFO frame for the diagnostic dump —
            // KODI variants are rare (every 100th packet), so a raw capture
            // is the only practical way to pin an unknown length/region.
            if (info && _badLen == 0) {
                std::memcpy(_bad, _buf, _len);
                _badLen = _len;
            }
            consume(1);          // shed the 'K' so the hunt moves on
            hunt();
            return EscFeed::Error;
        }
        return EscFeed::None;    // still collecting
    }

private:
    /// True when the buffered prefix is (so far) a valid frame magic.
    bool prefixOk() const {
        static const uint8_t m[3] = {'K', 'O', 'D'};
        for (size_t i = 0; i < _len && i < 3; ++i)
            if (_buf[i] != m[i]) return false;
        if (_len >= 4 && _buf[3] != 'L' && _buf[3] != 'I') return false;
        return true;
    }
    /// Slide off leading bytes until the buffer starts with a magic prefix
    /// (or empties).  Silent — hunting is not an error.
    void hunt() {
        while (_len && !prefixOk()) consume(1);
    }
    void consume(size_t n) {
        _len -= n;
        std::memmove(_buf, _buf + n, _len);
    }
    /// CRC32 (reflected) over the frame's first `crcLen` bytes vs the
    /// trailing u32 at `n - 4` — accept LE or BE trailer (the vendor sample
    /// code emits big-endian byte order).
    bool crcOk(size_t n, size_t crcLen) const {
        using namespace esc_detail;
        const uint32_t calc = crc32r(_buf, crcLen);
        const uint32_t le = rd32(&_buf[n - 4]);
        const uint32_t be = ((uint32_t)_buf[n - 4] << 24) | ((uint32_t)_buf[n - 3] << 16)
                          | ((uint32_t)_buf[n - 2] << 8) | _buf[n - 1];
        return calc == le || calc == be;
    }
    /// (frame length, CRC-covered length) candidate pairs, ascending by
    /// frame length — see the header comment.  The JIVE PRO's KODL CRC
    /// stops 2 bytes short of the trailer (state + timing uncovered).
    struct Cand { size_t frameLen; size_t crcLen; };
    static constexpr size_t kNumKodl = 3;
    static constexpr Cand kKodlCands[kNumKodl] = {{38, 32}, {38, 34}, {40, 36}};
    static constexpr size_t kNumKodi = 3;
    static constexpr Cand kKodiCands[kNumKodi] = {{41, 36}, {41, 37}, {44, 40}};
    uint8_t _buf[48] = {};
    size_t  _len = 0;
    uint8_t _bad[48] = {};
    size_t  _badLen = 0;
};

}  // namespace sfx_peripherals

#endif  // SFX_KONTRONIK_DECODER_H
