/*
 * RcPwmInputRole implementation.
 */

#include "rc_pwm_input_role.h"

namespace sfx_core {

bool RcPwmInputRole::read(uint16_t* outUs) const {
    if (!_valid) return false;
    if (outUs) *outUs = _latest_us;
    return true;
}

void RcPwmInputRole::setBroadcastHz(uint8_t hz) {
    _broadcastHz = hz;
    _broadcastInterval_ms = (hz == 0) ? 0 : (1000u / hz);
    _lastBroadcastMs = 0;
}

void RcPwmInputRole::tick() {
    if (!_port) return;

    uint16_t sample = 0;
    if (_port->readMicroseconds(&sample)) {
        _latest_us = sample;
        _valid     = true;
    }

    if (_broadcastInterval_ms == 0 || !_onBroadcast) return;
    const uint32_t now = millis();
    if (now - _lastBroadcastMs >= _broadcastInterval_ms) {
        _lastBroadcastMs = now;
        _onBroadcast(_latest_us, _valid);
    }
}

}  // namespace sfx_core
