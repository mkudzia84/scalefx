/*
 * LedAnimator — event-queue LED animation role on a `PwmPort`.
 *
 * Self-contained role: owns a small fixed-capacity event queue, a master
 * brightness multiplier (percent), and the runtime state of the
 * currently-playing event.  Tick via `update()` from the role-service.
 *
 * Wire format — fixed 10 bytes per event, matching the LightFx master-
 * side `hubfx::effects::lightfx::LightEvent::serialize()` layout.  Single
 * source of truth: the master serializes one of these per channel into
 * `LED_QUEUE_LOAD` and the role-server parses it verbatim.
 *
 *   [kind:u8]                                                            // EventKind
 *   [durationMs:u16LE]   // 0 = terminal / run indefinitely
 *   [cycleMs:u16LE]      // Flash / Fading / Beacon period
 *   [brightnessPct:u8]   // 0..100 — for On / FadeIn / FadeOut / Flash
 *   [minPct:u8]          // Fading / Beacon
 *   [maxPct:u8]          // Fading / Beacon
 *   [flashPct:u8]        // Flash / Beacon — percent of cycle "on"
 *   [flags:u8]           // reserved
 *
 * `LED_QUEUE_DONE` fires once (async, TAG_ASYNC) when the queue runs to
 * the end and the final event has `durationMs > 0` (i.e. is finite).
 * If the final event has `durationMs == 0` it holds forever and DONE
 * never fires.
 *
 * The port abstraction means LedAnimator works identically against a
 * native MCU PWM pin, a PCA9685 channel, or any future expander — the
 * board picks the driver at declaration time; the role doesn't care.
 */

#ifndef SFX_LED_ANIMATOR_H
#define SFX_LED_ANIMATOR_H

#include <Arduino.h>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <cstddef>
#include <cstdint>
#include <functional>

#include <ports/pwm_port.h>

namespace sfx_core {

class LedAnimator {
public:
    static constexpr size_t MAX_EVENTS = 32;

    /// Event opcodes — values match `hubfx::effects::lightfx::LightEventKind`
    /// byte-for-byte so the master can encode straight into LED_QUEUE_LOAD.
    enum EventKind : uint8_t {
        EV_ON       = 0,   ///< constant brightness, no fade
        EV_OFF      = 1,   ///< output low
        EV_FLASH    = 2,   ///< square wave: brightness for flashPct% of cycleMs, else 0
        EV_FADE_IN  = 3,   ///< linear ramp 0 → brightnessPct over durationMs
        EV_FADE_OUT = 4,   ///< linear ramp brightnessPct → 0 over durationMs
        EV_FADING   = 5,   ///< sinusoidal between minPct and maxPct, period cycleMs
        EV_BEACON   = 6,   ///< at maxPct for flashPct% of cycleMs, else minPct
    };

    /// One queued event. In-memory layout mirrors the 10-byte wire record;
    /// callers must keep the field names aligned.
    struct Event {
        uint8_t  kind          = EV_OFF;
        uint16_t durationMs    = 0;      ///< 0 = terminal / hold forever
        uint16_t cycleMs       = 0;      ///< Flash / Fading / Beacon
        uint8_t  brightnessPct = 0;      ///< 0..100 — On / Flash / FadeIn / FadeOut
        uint8_t  minPct        = 0;      ///< Fading / Beacon
        uint8_t  maxPct        = 100;    ///< Fading / Beacon
        uint8_t  flashPct      = 50;     ///< Flash / Beacon — % of cycle "on"
        uint8_t  flags         = 0;      ///< reserved
    };

    /// Wire size of one serialized event record — keep aligned with the
    /// master-side `hubfx::effects::lightfx::kEventWireSize`.
    static constexpr size_t WIRE_EVENT_SIZE = 10;

    /// Called (async, TAG_ASYNC) when the queue finishes (final event was finite).
    using DoneCallback = std::function<void()>;

    LedAnimator() = default;
    explicit LedAnimator(sfx_peripherals::PwmPort* port) : _port(port) {}

    /// Re-bind to a different port (used when the role is re-emplaced).
    void bind(sfx_peripherals::PwmPort* port) { _port = port; }

    /// Replace the queue contents. Stops playback. Returns false if
    /// `count > MAX_EVENTS`.
    bool loadQueue(const Event* events, size_t count);

    /// Start playing the queue from index 0.
    void start();
    /// Stop playback (force OFF on the port).
    void stop();

    /// Master brightness multiplier in percent (0..100, default 100).
    /// Scales every event's brightness when writing to the port.
    void setMasterBrightnessPct(uint8_t pct) {
        if (pct > 100) pct = 100;
        _masterBrightnessPct = pct;
    }
    uint8_t masterBrightnessPct() const { return _masterBrightnessPct; }

    /// True iff the queue is currently playing.
    bool isPlaying() const { return _playing; }

    /// Number of events currently loaded.
    uint8_t queueDepth() const { return _count; }

    /// Register the done callback. Fired once per queue completion.
    void onQueueDone(DoneCallback cb) { _onDone = std::move(cb); }

    /// Tick — drive the state machine.  `now` is a SHARED SFX_MILLIS()
    /// timestamp captured once per `RoleServicePolicy::update()` pass and
    /// passed to every channel, so all LedAnimators sample the exact same
    /// instant — same-period channels stay in perfect phase and nothing
    /// drifts with the (jittery) main-loop period.  Cyclic events derive
    /// their phase from absolute `now % cycle_ms` (millis-locked), not
    /// from time-since-start, so a late tick still renders the correct
    /// phase and channels never desync.
    void tick(uint32_t now);

private:
    /// Write a brightness-percent value (0..100) to the port, scaled by
    /// master and by the port's native max duty.
    void writeOutputPct(uint8_t brightnessPct);

    /// Advance to the next event in the queue, priming any per-event
    /// state.  Fires _onDone when the queue exhausts.
    void advance();

    sfx_peripherals::PwmPort* _port = nullptr;

    /// Loop flag bit in `Event::flags` (event[0]) — phase-locked looping
    /// pattern.  Mirrors hubfx::effects::lightfx::LightEventFlags::Loop.
    static constexpr uint8_t FLAG_LOOP = 0x01;

    Event    _queue[MAX_EVENTS] {};
    uint8_t  _count            = 0;
    uint8_t  _cursor           = 0;
    bool     _playing          = false;

    // Phase-locked loop mode (set from event[0].flags & FLAG_LOOP at
    // loadQueue).  When `_loop`, the queue is one repeating cycle of
    // period `_loopPeriodMs` (= Σ durationMs) and the active segment is
    // `now % _loopPeriodMs` — so all looping channels stay phase-aligned.
    bool     _loop             = false;
    uint32_t _loopPeriodMs     = 0;

    // Active-event state machine
    uint32_t _eventStart_ms     = 0;
    uint8_t  _currentBrightPct  = 0;       ///< last value written (post-master, pre-port-scale)
    uint8_t  _masterBrightnessPct = 100;

    // FadeIn / FadeOut helpers — captured at event entry so mid-fade
    // master-brightness changes don't corrupt the ramp.
    uint8_t  _fadeFromPct      = 0;
    uint8_t  _fadeToPct        = 0;

    DoneCallback _onDone;
};

}  // namespace sfx_core

#endif  // SFX_LED_ANIMATOR_H
