/*
 * RcPwmInputRole implementation.
 */

#include "rc_pwm_input_role.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

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
    _bcast.subscribe(hz);   // host wire subscribe; local feed keeps ticking
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

    // Local effect feed + (when subscribed) wire broadcast — full cadence
    // while the signal is valid; a slow heartbeat while it's lost (no RX /
    // junk) so a subscribed host isn't flooded with NO-SIGNAL frames.  Full
    // rate resumes the instant the RX is (re)connected.
    if (_onBroadcast && _bcast.dueGated(SFX_MILLIS(), _valid)) _onBroadcast(_count, _valid);
}

}  // namespace sfx_core
