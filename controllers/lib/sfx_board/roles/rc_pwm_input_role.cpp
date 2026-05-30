/*
 * RcPwmInputRole implementation.
 */

#include "rc_pwm_input_role.h"

#include <serial/ports.h>   // InputPortFlags::PULSE

namespace sfx_core {

bool RcPwmInputRole::bind(sfx_peripherals::InputPort* port) {
    if (!port) return false;
    if ((port->capabilities() & InputPortFlags::PULSE) == 0) return false;
    if (!port->configurePulseCapture()) return false;
    _port  = port;
    _count = 0;
    _valid = false;
    return true;
}

bool RcPwmInputRole::read(uint16_t* outUs) const {
    if (!_valid || _count == 0) return false;
    if (outUs) *outUs = _channels[0];
    return true;
}

void RcPwmInputRole::setBroadcastHz(uint8_t hz) {
    // Host wire subscribe — gates ONLY the wire broadcast.  The local effect
    // feed keeps ticking at _broadcastInterval_ms (the fixed firmware rate), so
    // hz==0 silences the wire without starving effects.
    _broadcastHz  = hz;
    _wireEnabled  = (hz != 0);
}

void RcPwmInputRole::tick() {
    if (!_port) return;

    // Drain the PPM decoder every tick (keeps the RMT queue empty) and
    // cache the channel set.  A return of 0 means signal-lost / no frame.
    uint16_t frame[kMaxChannels];
    const uint8_t n = _port->readPpmChannels(frame, kMaxChannels);
    _valid = (n > 0);
    if (n > 0) {
        _count = n;
        for (uint8_t i = 0; i < n; i++) _channels[i] = frame[i];
    }

    if (_broadcastInterval_ms == 0 || !_onBroadcast) return;
    const uint32_t now = millis();
    if (now - _lastBroadcastMs >= _broadcastInterval_ms) {
        _lastBroadcastMs = now;
        _onBroadcast(_count, _valid);
    }
}

}  // namespace sfx_core
