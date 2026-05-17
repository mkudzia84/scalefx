/*
 * RcPwmInputRole — pulse-width capture on a `ServoPort` in input mode.
 *
 * Stub for the single-channel input-capture role.  Each tick polls the
 * port via `readMicroseconds()`; if a fresh sample is available, the
 * latest pulse width is cached for synchronous reads and (optionally)
 * broadcast at a configurable rate.
 *
 * SBUS / Jeti EX / PPM (multi-channel framed protocols) are NOT this
 * role — they need their own `SerialInputPort` driver type when
 * implemented.  See `instructions/15-GENERIC-EXPANDER-REFACTOR.md`.
 */

#ifndef SFX_RC_PWM_INPUT_ROLE_H
#define SFX_RC_PWM_INPUT_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <ports/servo_port.h>

namespace sfx_core {

class RcPwmInputRole {
public:
    /// Async broadcast callback: `(us, valid)`.
    using BroadcastCallback = std::function<void(uint16_t us, bool valid)>;

    RcPwmInputRole() = default;
    explicit RcPwmInputRole(sfx_peripherals::ServoPort* port) : _port(port) {}

    bool bind(sfx_peripherals::ServoPort* port) {
        if (!port || !port->supportsInput()) return false;
        _port = port;
        return true;
    }

    /// Read the most recent valid sample.  Returns false if the port
    /// hasn't produced a sample yet (`outUs` left unmodified).
    bool read(uint16_t* outUs) const;

    /// Broadcast rate in Hz.  0 disables.  Async packets fire from `tick()`.
    void setBroadcastHz(uint8_t hz);

    /// Most recent reading.  `valid()` is false until the first sample lands.
    uint16_t latest_us() const { return _latest_us; }
    bool     valid()    const { return _valid; }

    void onBroadcast(BroadcastCallback cb) { _onBroadcast = std::move(cb); }

    /// Tick — polls the port for a fresh sample and runs the broadcast timer.
    void tick();

private:
    sfx_peripherals::ServoPort* _port = nullptr;

    uint16_t _latest_us           = 0;
    bool     _valid               = false;
    uint8_t  _broadcastHz         = 0;     ///< 0 = disabled
    uint32_t _broadcastInterval_ms = 0;
    uint32_t _lastBroadcastMs     = 0;

    BroadcastCallback _onBroadcast;
};

}  // namespace sfx_core

#endif  // SFX_RC_PWM_INPUT_ROLE_H
