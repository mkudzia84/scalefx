/*
 * hobbywing_v5_decoder.h — HOBBYWING Platinum V5 ESC telemetry decoder
 * policy.
 *
 *   115200 8N1, FE 01 00 03 30 5C header, 32-byte frames,
 *   CRC16-MODBUS (reflected 0xA001, init 0xFFFF).
 *
 *   Field map from the Rotorflight esc_sensor.c implementation
 *   (NEEDS BENCH VALIDATION).
 */

#ifndef SFX_HOBBYWING_V5_DECODER_H
#define SFX_HOBBYWING_V5_DECODER_H

#include "esc_decoder.h"

namespace sfx_peripherals {

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

}  // namespace sfx_peripherals

#endif  // SFX_HOBBYWING_V5_DECODER_H
