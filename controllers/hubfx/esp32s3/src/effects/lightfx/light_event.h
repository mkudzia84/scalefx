/*
 * light_event.h — high-level LED event definition + wire serializer.
 *
 *   Replaces the legacy `controllers/lib/sfx_peripherals/led/led_events.h`
 *   playback library.  The master-side effects code only NEEDS to
 *   *describe* an event and ship it down to the `LedAnimator` role on
 *   whichever board owns the LED — the role decodes the payload, ticks
 *   the animation, and writes the PWM hardware.  All animation math
 *   lives on the target side now.
 *
 *   Uniform 10-byte fixed-size record per event keeps the wire layout
 *   simple to decode on the role side and lets a 24-event queue fit
 *   easily in a 512-byte Pico packet (240 bytes payload + a small
 *   header).  Fields that don't apply to a given event kind are
 *   ignored on the decode side.
 *
 *   Event record layout:
 *
 *       [kind:u8]           // LightEventKind
 *       [duration_ms:u16LE] // 0 = run indefinitely (terminal event)
 *       [cycle_ms:u16LE]    // Flashing / Fading / Beacon period
 *       [brightness:u8]     // 0..100, "main" intensity for the kind
 *       [min_pct:u8]        // Fading / Beacon low bound
 *       [max_pct:u8]        // Fading / Beacon high bound
 *       [flash_pct:u8]      // Beacon: fraction of cycle the lamp is high
 *       [flags:u8]          // reserved for future kinds
 *
 *   Queue load wire payload (for `LED_QUEUE_LOAD` / 0x58):
 *
 *       [portIdx:u8][count:u8] [event:10b] × count
 */

#ifndef HUBFX_LIGHT_EVENT_H
#define HUBFX_LIGHT_EVENT_H

#include <cstddef>
#include <cstdint>

namespace hubfx::effects::lightfx {

/// One-byte enum matching the legacy `LedEvent::typeId()` IDs so the
/// LedAnimator role on the target board can switch on `kind` to pick
/// the right animation tick.  Append-only.
enum class LightEventKind : uint8_t {
    On      = 0,   ///< constant on at `brightness`
    Off     = 1,   ///< constant off for `duration_ms`
    Flash   = 2,   ///< on/off square wave, `cycle_ms` period, `flash_pct` duty
    FadeIn  = 3,   ///< linear ramp 0 → `brightness` over `duration_ms`
    FadeOut = 4,   ///< linear ramp `brightness` → 0 over `duration_ms`
    Fading  = 5,   ///< sinusoidal between `min_pct` and `max_pct`, period `cycle_ms`
    Beacon  = 6,   ///< brief flash to `max_pct` for `flash_pct` of `cycle_ms`,
                   ///< else `min_pct`
};

/// Event `flags` bits (byte 9 of the wire record).
namespace LightEventFlags {
    /// The channel's event list is a PHASE-LOCKED LOOPING PATTERN: the
    /// LedAnimator plays it as a cycle whose period is the sum of all the
    /// events' `duration_ms`, and derives the active event from absolute
    /// `now % period`.  This means every channel with the same total
    /// period stays perfectly phase-aligned (shared clock), and arbitrary
    /// multi-pulse patterns — single flash, double flash, offset so two
    /// channels never overlap — are expressed purely as a sequence of
    /// timed On/Off (and fade) events.  Set on the FIRST event of the
    /// channel (the animator reads event[0]).  Without it, the queue
    /// plays once and the terminal event holds (the original behaviour).
    inline constexpr uint8_t Loop = 0x01;

    /// FIRE-AND-FORGET: suppress the `LED_QUEUE_DONE` async event when this
    /// queue runs to the end.  For high-rate effect-internal blinks (the gun
    /// muzzle flash fires one finite On event per shot — 10+/s at auto-fire),
    /// the per-completion DONE event has no consumer and just floods the wire +
    /// diag log.  Set on event[0]; genuine user PROGRAMS leave it clear so a
    /// host still learns when a one-shot sequence finishes.
    inline constexpr uint8_t NoDone = 0x02;
}

/// Wire size of one serialized event record.  Asserted at compile time.
static constexpr size_t kEventWireSize = 10;

/// High-level event description.  Build one of these on the master,
/// then `serialize()` into the LED_QUEUE_LOAD payload.
struct LightEvent {
    LightEventKind kind         = LightEventKind::On;
    uint16_t       durationMs   = 0;     ///< 0 = infinite / terminal
    uint16_t       cycleMs      = 0;     ///< Flash / Fading / Beacon
    uint8_t        brightnessPct = 100;  ///< 0..100
    uint8_t        minPct        = 0;    ///< Fading / Beacon
    uint8_t        maxPct        = 100;  ///< Fading / Beacon
    uint8_t        flashPct      = 50;   ///< Beacon: percent of cycle on
    uint8_t        flags         = 0;    ///< reserved

    /// Convenience builders for the common cases — readable call sites
    /// and a single place to default the irrelevant fields.

    static LightEvent on(uint8_t brightnessPct = 100, uint16_t durationMs = 0) {
        LightEvent e;
        e.kind          = LightEventKind::On;
        e.brightnessPct = brightnessPct;
        e.durationMs    = durationMs;
        return e;
    }
    static LightEvent off(uint16_t durationMs = 0) {
        LightEvent e;
        e.kind        = LightEventKind::Off;
        e.durationMs  = durationMs;
        e.brightnessPct = 0;
        return e;
    }
    static LightEvent flash(uint16_t cycleMs, uint8_t brightnessPct = 100,
                            uint8_t flashPct = 50, uint16_t durationMs = 0) {
        LightEvent e;
        e.kind          = LightEventKind::Flash;
        e.cycleMs       = cycleMs;
        e.brightnessPct = brightnessPct;
        e.flashPct      = flashPct;
        e.durationMs    = durationMs;
        return e;
    }
    static LightEvent fadeIn(uint16_t durationMs, uint8_t brightnessPct = 100) {
        LightEvent e;
        e.kind          = LightEventKind::FadeIn;
        e.durationMs    = durationMs;
        e.brightnessPct = brightnessPct;
        return e;
    }
    static LightEvent fadeOut(uint16_t durationMs, uint8_t brightnessPct = 100) {
        LightEvent e;
        e.kind          = LightEventKind::FadeOut;
        e.durationMs    = durationMs;
        e.brightnessPct = brightnessPct;
        return e;
    }
    static LightEvent fading(uint16_t cycleMs, uint8_t minPct = 0,
                             uint8_t maxPct = 100, uint16_t durationMs = 0) {
        LightEvent e;
        e.kind        = LightEventKind::Fading;
        e.cycleMs     = cycleMs;
        e.minPct      = minPct;
        e.maxPct      = maxPct;
        e.durationMs  = durationMs;
        return e;
    }
    static LightEvent beacon(uint16_t cycleMs, uint8_t maxPct = 100,
                             uint8_t flashPct = 10, uint8_t minPct = 0,
                             uint16_t durationMs = 0) {
        LightEvent e;
        e.kind        = LightEventKind::Beacon;
        e.cycleMs     = cycleMs;
        e.maxPct      = maxPct;
        e.minPct      = minPct;
        e.flashPct    = flashPct;
        e.durationMs  = durationMs;
        return e;
    }

    /// Write the 10-byte wire record into `out`.  Returns
    /// `kEventWireSize` on success, 0 if `cap < kEventWireSize`.
    size_t serialize(uint8_t* out, size_t cap) const {
        if (cap < kEventWireSize) return 0;
        out[0] = static_cast<uint8_t>(kind);
        out[1] = static_cast<uint8_t>(durationMs & 0xFF);
        out[2] = static_cast<uint8_t>((durationMs >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(cycleMs & 0xFF);
        out[4] = static_cast<uint8_t>((cycleMs >> 8) & 0xFF);
        out[5] = brightnessPct;
        out[6] = minPct;
        out[7] = maxPct;
        out[8] = flashPct;
        out[9] = flags;
        return kEventWireSize;
    }
};

/// Serialize a sequence of events into a `LED_QUEUE_LOAD` wire payload:
/// `[portIdx:u8][count:u8] [event:10b] × count`.  Returns the total
/// number of bytes written.  Caller is responsible for ensuring `cap`
/// is large enough — `2 + count * kEventWireSize` bytes.
inline size_t serializeQueueLoad(uint8_t portIdx,
                                 const LightEvent* events, uint8_t count,
                                 uint8_t* out, size_t cap) {
    const size_t need = 2 + static_cast<size_t>(count) * kEventWireSize;
    if (cap < need) return 0;
    out[0] = portIdx;
    out[1] = count;
    size_t off = 2;
    for (uint8_t i = 0; i < count; ++i) {
        size_t n = events[i].serialize(&out[off], cap - off);
        if (n == 0) return 0;
        off += n;
    }
    return off;
}

}  // namespace hubfx::effects::lightfx

#endif  // HUBFX_LIGHT_EVENT_H
