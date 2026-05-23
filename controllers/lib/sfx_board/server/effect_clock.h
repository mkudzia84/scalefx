/*
 * effect_clock.h — single ms-latched time source for the effect layer.
 *
 * Rule 40 (Phase 0.5 of GunFX rollout, instructions/22): all effects
 * (EngineFx, GunFx, GearControl, landing lights, future …) MUST read
 * time from `sfx_core::EffectClock::instance()` instead of calling
 * `millis()` directly. The clock latches `millis()` ONCE per main-loop
 * pass — every effect that ticks within that pass sees the same
 * `nowMs()` and a consistent `dtMs()` (delta since previous latch).
 *
 * Why: a single tick may run EngineFx, GunFx, GearControl, the
 * landing-light animator AND a smoke-fan puff scheduler. If each
 * calls `millis()` independently, microseconds apart, their notion of
 * "now" drifts — a ROF scheduler can fire a second shot before a fan
 * puff has logged the first, motion profiles double-integrate, etc.
 * Latching once at the top of the loop guarantees lockstep behaviour
 * across every effect.
 *
 * Scope: this is for the EFFECT LAYER ONLY. Drivers, the bus, the
 * keepalive, the upload state machine — these all keep using raw
 * `millis()` (their cadence is independent of the effect tick).
 *
 * Wiring:
 *   void loop() {
 *       sfx_core::EffectClock::instance().latch();   // ONCE per pass
 *       board.process();                              // effects tick here
 *   }
 *
 * Usage from an effect:
 *   #include <server/effect_clock.h>
 *   const uint32_t now = sfx_core::EffectClock::instance().nowMs();
 *   if (now - _lastShotMs >= _intervalMs) { fire(); _lastShotMs = now; }
 *
 * Motion profiles use `dtMs()` for delta-time integration:
 *   const uint32_t dt = sfx_core::EffectClock::instance().dtMs();
 *   _pos += _vel * (float(dt) * 0.001f);
 */

#ifndef SFX_EFFECT_CLOCK_H
#define SFX_EFFECT_CLOCK_H

#include <cstdint>

#include <Arduino.h>   // millis()

namespace sfx_core {

class EffectClock {
public:
    /// Latch a fresh time sample. Called ONCE per main-loop pass by
    /// the board's run loop, BEFORE any effect tick. Subsequent calls
    /// within the same `millis()` value are no-ops (idempotent within
    /// a tick), so a misordered call doesn't shift the clock mid-tick.
    void latch() {
        const uint32_t t = millis();
        if (t == _nowMs) return;
        _dtMs   = t - _nowMs;
        _nowMs  = t;
    }

    /// Latched current time in milliseconds. Stable for the whole tick.
    uint32_t nowMs() const { return _nowMs; }

    /// Milliseconds elapsed since the previous `latch()` call. 0 on
    /// the very first latch. Use this for motion-profile integration
    /// (`pos += vel * dt`), animation interpolation, fade ramps, etc.
    uint32_t dtMs()  const { return _dtMs;  }

    /// Singleton accessor — there is exactly one clock per binary.
    /// Thread-safe in C++11+ via the function-local static guard.
    static EffectClock& instance() {
        static EffectClock c;
        return c;
    }

    EffectClock(const EffectClock&)            = delete;
    EffectClock& operator=(const EffectClock&) = delete;

private:
    EffectClock() = default;
    uint32_t _nowMs = 0;
    uint32_t _dtMs  = 0;
};

}  // namespace sfx_core

#endif  // SFX_EFFECT_CLOCK_H
