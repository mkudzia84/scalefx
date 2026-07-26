/*
 * input_broadcaster.h — shared broadcast cadence + wire-subscribe state for
 * input roles (RC PWM / SBUS / Jeti EX).
 *
 * Every input role decodes channels at a fixed firmware rate and feeds them to
 * two consumers:
 *   - the LOCAL on-board InputDispatcher (effects) — must run ALWAYS, even with
 *     no host attached (the model flies with no USB);
 *   - the WIRE broadcast (a connected host's live-channel view) — OFF by
 *     default, on only while a host has SUBSCRIBED.
 *
 * Both fire on the same cadence (`due()`), but the wire half is gated on
 * `wireEnabled()` in the role_service emitter.  This object owns the cadence
 * timer + the subscribe flag so the three roles don't each re-implement it.
 *
 * Header-only, no platform deps — the roles embed it by value.
 */

#ifndef SFX_INPUT_BROADCASTER_H
#define SFX_INPUT_BROADCASTER_H

#include <cstdint>

namespace sfx_core {

class InputBroadcaster {
public:
    /// Emit spacing while the input signal is INVALID (no RX / junk).  A slow
    /// heartbeat instead of the full cadence: it still refreshes a subscribed
    /// host's NO-SIGNAL indicator and still drives failsafe, but does NOT
    /// flood the wire (and the host's live-view / diagnostic log) with 50 Hz
    /// empty frames when nothing is plugged into the input.  4 Hz.
    static constexpr uint32_t kSignalLostHeartbeatMs = 250;

    /// Host subscribe/unsubscribe to the WIRE broadcast.  hz!=0 = subscribe,
    /// hz==0 = unsubscribe.  Does NOT change the local feed cadence — effects
    /// keep getting channels regardless.
    void subscribe(uint8_t hz) { _wireEnabled = (hz != 0); }

    /// True while a host wants the live wire stream.
    bool wireEnabled() const { return _wireEnabled; }

    /// Set the LOCAL feed cadence (firmware-fixed; the host does not control
    /// this).  hz==0 keeps the 50 Hz default so the feed never stalls.
    void setLocalHz(uint8_t hz) { _intervalMs = (hz == 0) ? 20 : (1000u / hz); }

    /// True at most once per cadence interval — drives the role's emit.  The
    /// emit always feeds effects locally; the wire half checks wireEnabled().
    /// `minIntervalMs` (when larger than the normal cadence) throttles THIS
    /// call — used to slow the emit to a heartbeat while the signal is lost;
    /// it never speeds the emit up, so a subscribed host still gets the full
    /// rate the instant the signal returns.
    bool due(uint32_t nowMs, uint32_t minIntervalMs = 0) {
        const uint32_t interval = (minIntervalMs > _intervalMs) ? minIntervalMs : _intervalMs;
        if (nowMs - _lastMs < interval) return false;
        _lastMs = nowMs;
        return true;
    }

    /// Cadence gate that auto-throttles to `kSignalLostHeartbeatMs` while the
    /// signal is invalid, so an unplugged input doesn't flood a subscribed
    /// host.  Full cadence resumes the instant `signalValid` goes true again
    /// (hot-plug: reconnect the RX and the live view springs back to 50 Hz).
    bool dueGated(uint32_t nowMs, bool signalValid) {
        return due(nowMs, signalValid ? 0 : kSignalLostHeartbeatMs);
    }

private:
    bool     _wireEnabled = false;   ///< host subscribed to the wire stream
    uint32_t _intervalMs  = 20;      ///< LOCAL feed cadence (50 Hz default)
    uint32_t _lastMs      = 0;       ///< last emit timestamp
};

}  // namespace sfx_core

#endif  // SFX_INPUT_BROADCASTER_H
