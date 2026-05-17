/*
 * DcMotorRole implementation.
 */

#include "dc_motor_role.h"

namespace sfx_core {

void DcMotorRole::setDuty(uint16_t duty) {
    _commandedDuty = duty;
    if (_port) _port->setDuty(duty);
    if (duty == 0) {
        _overcurrentStartMs = 0;
        _peakDuringWindow_mA = 0;
    }
}

void DcMotorRole::setStallGuard(uint16_t threshold_mA, uint16_t window_ms) {
    _stallThreshold_mA  = threshold_mA;
    _stallWindow_ms     = window_ms;
    _overcurrentStartMs = 0;
}

void DcMotorRole::clearStall() {
    _stalled             = false;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
}

void DcMotorRole::tick() {
    if (!_iSense || _stallThreshold_mA == 0 || _stalled) return;
    const int16_t i_mA = _iSense->current_mA();
    const uint32_t now = millis();

    if (i_mA >= (int16_t)_stallThreshold_mA) {
        if (_overcurrentStartMs == 0) {
            _overcurrentStartMs  = now;
            _peakDuringWindow_mA = (uint16_t)i_mA;
        } else {
            if ((uint16_t)i_mA > _peakDuringWindow_mA) _peakDuringWindow_mA = (uint16_t)i_mA;
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
