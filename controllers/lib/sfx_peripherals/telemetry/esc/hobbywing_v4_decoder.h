/*
 * hobbywing_v4_decoder.h — HOBBYWING Platinum PRO V4 / FlyFun V5 decoder
 * policy.
 *
 *   19200 8N1, 0x9B-headed 19-byte data frames, NO checksum (range-sanity
 *   checks only).  V/I need per-model scale factors delivered in a 13-byte
 *   info frame (0xB9 footer) — until one is seen, V/I read 0 and only
 *   RPM/throttle/PWM/temps (absolute NTC β-model math) publish.
 *
 *   Field map + NTC constants from the Rotorflight esc_sensor.c
 *   implementation (NEEDS BENCH VALIDATION).
 */

#ifndef SFX_HOBBYWING_V4_DECODER_H
#define SFX_HOBBYWING_V4_DECODER_H

#include <cmath>
#include <cstring>

#include "esc_decoder.h"

namespace sfx_peripherals {

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

}  // namespace sfx_peripherals

#endif  // SFX_HOBBYWING_V4_DECODER_H
