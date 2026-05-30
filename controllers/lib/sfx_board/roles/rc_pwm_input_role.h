/*
 * RcPwmInputRole — PPM (pulse) capture on an `InputPort`.
 *
 * Attaches to an `InputPort` and switches it to PULSE mode (RMT-based
 * PPM sum-signal capture).  Decodes up to `kMaxChannels` channels from
 * one wire; a plain single-channel RC PWM is just a 1-channel frame.
 * Each tick drains the decoder and caches the channel set for
 * synchronous reads (channel 1) and, optionally, broadcasts the full
 * frame at a configurable rate.  Per Rule 43 the master maps each
 * channel index → a named function via /hubfx.yaml's inputs[] block.
 *
 * SBUS / Jeti EX (framed UART protocols) live in their own roles —
 * `SbusInputRole` / `JetiExInputRole` — since they consume a byte stream.
 */

#ifndef SFX_RC_PWM_INPUT_ROLE_H
#define SFX_RC_PWM_INPUT_ROLE_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <ports/input_port.h>
#include <rx_input/rx_common.h>   // RxConfig::MAX_CHANNELS / CENTER_US

#include "input_broadcaster.h"

namespace sfx_core {

class RcPwmInputRole {
public:
    /// Max PPM channels decoded from one wire (matches RxConfig).
    static constexpr uint8_t kMaxChannels = RxConfig::MAX_CHANNELS;

    /// Async broadcast trigger: fires at the configured rate.  Args echo
    /// the current frame (count, valid); the handler rebuilds the full
    /// channel payload from the role (mirrors SbusInputRole's pattern).
    using BroadcastCallback = std::function<void(uint8_t count, bool valid)>;

    RcPwmInputRole() = default;
    explicit RcPwmInputRole(sfx_peripherals::InputPort* port) { bind(port); }

    /// Switches the port into PULSE mode.  Returns false if the port
    /// doesn't advertise pulse-capture capability or the mode switch
    /// failed (peripheral busy etc.).
    bool bind(sfx_peripherals::InputPort* port);

    /// Read channel 1's most recent valid sample (back-compat single
    /// read).  Returns false if no valid frame yet (`outUs` untouched).
    bool read(uint16_t* outUs) const;

    /// Host subscribe/unsubscribe to the WIRE broadcast (live-channel view):
    /// hz!=0 = subscribe, hz==0 = unsubscribe.  Does NOT affect the LOCAL
    /// effect feed, which runs at a fixed firmware rate so the model flies with
    /// no host attached.
    void setBroadcastHz(uint8_t hz);
    /// True while a host has subscribed to the wire broadcast.
    bool wireEnabled() const { return _bcast.wireEnabled(); }

    /// Channel 1 (back-compat) + validity.  `valid()` is false until the
    /// first frame lands or after signal loss.
    uint16_t latest_us() const { return _count ? _channels[0] : 0; }
    bool     valid()    const { return _valid; }

    /// Decoded channel value in µs (1-based).  Returns CENTER_US for an
    /// out-of-range channel so consumers get a safe neutral.
    uint16_t channel_us(uint8_t ch) const {
        if (ch < 1 || ch > _count) return RxConfig::CENTER_US;
        return _channels[ch - 1];
    }
    /// Number of channels in the last decoded frame.
    uint8_t channelCount() const { return _count; }

    void onBroadcast(BroadcastCallback cb) { _onBroadcast = std::move(cb); }

    /// Tick — drains the PPM decoder, caches channels, runs the broadcast timer.
    void tick();

private:
    sfx_peripherals::InputPort* _port = nullptr;

    uint16_t _channels[kMaxChannels] = {};
    uint8_t  _count                = 0;
    bool     _valid                = false;
    InputBroadcaster  _bcast;        ///< shared cadence + wire-subscribe state
    BroadcastCallback _onBroadcast;
};

}  // namespace sfx_core

#endif  // SFX_RC_PWM_INPUT_ROLE_H
