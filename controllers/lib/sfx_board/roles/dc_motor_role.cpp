/*
 * DcMotorRole implementation.
 */

#include "dc_motor_role.h"

#include "../server/effect_clock.h"

namespace sfx_core {

void DcMotorRole::setDuty(uint16_t duty) {
    _commandedDuty = duty;
    if (_port) _port->setDuty(duty);
    if (duty == 0) {
        _overcurrentActive   = false;
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
}

void DcMotorRole::setPct(uint8_t pct) {
    const uint16_t maxDuty = _port ? _port->maxDuty() : 0;
    setDuty(scaleDuty(pct, maxDuty, _portRailMv, _element));
}

void DcMotorRole::setStallGuard(uint16_t threshold_mA, uint16_t window_ms) {
    _stallThreshold_mA  = threshold_mA;
    _stallWindow_ms     = window_ms;
    _overcurrentActive  = false;
    _overcurrentStartMs = 0;
}

void DcMotorRole::clearStall() {
    _stalled             = false;
    _overcurrentActive   = false;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
}

void DcMotorRole::tick() {
    if (!_iSense || _stallThreshold_mA == 0 || _stalled) return;
    const int16_t i_mA = _iSense->current_mA();
    const uint32_t now = EffectClock::instance().nowMs();

    if (i_mA >= (int16_t)_stallThreshold_mA) {
        if (!_overcurrentActive) {
            _overcurrentActive   = true;
            _overcurrentStartMs  = now;
            _peakDuringWindow_mA = (uint16_t)i_mA;
        } else {
            if ((uint16_t)i_mA > _peakDuringWindow_mA) _peakDuringWindow_mA = (uint16_t)i_mA;
            if (now - _overcurrentStartMs >= _stallWindow_ms) {
                _stalled = true;
                // Cut power on stall BEFORE firing the callback — the role must
                // not keep driving a stalled motor. Write duty 0 directly (not
                // via brake(), which would clear the latch we just set).
                _commandedDuty = 0;
                if (_port) _port->setDuty(0);
                if (_onStall) _onStall(_peakDuringWindow_mA, _stallWindow_ms);
            }
        }
    } else {
        _overcurrentActive   = false;
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
}

}  // namespace sfx_core
