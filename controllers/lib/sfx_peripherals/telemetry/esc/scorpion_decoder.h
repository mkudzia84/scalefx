/*
 * scorpion_decoder.h — SCORPION Tribunus ESC telemetry decoder policy.
 *
 *   "Unsc Telem" (unsolicited telemetry) mode: 38400 8N1, 0x55-headed
 *   22-byte records, CRC16-CCITT (reflected 0x8408 — same poly as Jeti).
 *
 *   Layout per the Rotorflight decoder (NEEDS BENCH VALIDATION):
 *     [0]=0x55 hdr, [4..5] throttle (0.5 %), [8..9] current (0.1 A),
 *     [10..11] voltage (0.1 V), [12..13] mAh, [14] temp °C,
 *     [15] PWM (0.5 %), [16] BEC V (0.1 V), [17..18] rpm (×5),
 *     [19] error, [20..21] CRC16-CCITT LE over [0..19].
 */

#ifndef SFX_SCORPION_DECODER_H
#define SFX_SCORPION_DECODER_H

#include <cstring>

#include "esc_decoder.h"

namespace sfx_peripherals {

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

}  // namespace sfx_peripherals

#endif  // SFX_SCORPION_DECODER_H
