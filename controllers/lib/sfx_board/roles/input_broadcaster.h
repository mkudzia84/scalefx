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
    bool due(uint32_t nowMs) {
        if (nowMs - _lastMs < _intervalMs) return false;
        _lastMs = nowMs;
        return true;
    }

private:
    bool     _wireEnabled = false;   ///< host subscribed to the wire stream
    uint32_t _intervalMs  = 20;      ///< LOCAL feed cadence (50 Hz default)
    uint32_t _lastMs      = 0;       ///< last emit timestamp
};

}  // namespace sfx_core

#endif  // SFX_INPUT_BROADCASTER_H
