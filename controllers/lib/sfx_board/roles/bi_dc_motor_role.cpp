/*
 * BiDcMotorRole implementation.
 */

#include "bi_dc_motor_role.h"

namespace sfx_core {

void BiDcMotorRole::setSigned(int16_t signedDuty) {
    _commandedSigned = signedDuty;
    if (_port) _port->setSigned(signedDuty);
    if (signedDuty == 0) {
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
}

void BiDcMotorRole::brake() {
    _commandedSigned = 0;
    if (_port) _port->brake();
    clearStall();
}

void BiDcMotorRole::coast() {
    _commandedSigned = 0;
    if (_port) _port->coast();
    clearStall();
}

void BiDcMotorRole::setStallGuard(uint16_t threshold_mA, uint16_t window_ms) {
    _stallThreshold_mA  = threshold_mA;
    _stallWindow_ms     = window_ms;
    _overcurrentStartMs = 0;
}

void BiDcMotorRole::clearStall() {
    _stalled             = false;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
}

void BiDcMotorRole::tick() {
    if (!_iSense || _stallThreshold_mA == 0 || _stalled) return;
    const int16_t  i_raw = _iSense->current_mA();
    const uint16_t i_mag = (uint16_t)std::abs((int)i_raw);
    const uint32_t now   = millis();

    if (i_mag >= _stallThreshold_mA) {
        if (_overcurrentStartMs == 0) {
            _overcurrentStartMs  = now;
            _peakDuringWindow_mA = i_mag;
        } else {
            if (i_mag > _peakDuringWindow_mA) _peakDuringWindow_mA = i_mag;
            if (now - _overcurrentStartMs >= _stallWindow_ms) {
                _stalled = true;
                if (_onStall) _onStall(_peakDuringWindow_mA, _stallWindow_ms);
            }
        }
    } else {
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
}

}  // namespace sfx_core
