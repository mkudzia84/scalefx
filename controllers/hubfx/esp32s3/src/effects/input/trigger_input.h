/*
 * trigger_input.h — typed input-channel converter for HubFX effects.
 *
 *   RC PWM, SBUS, and JetiEx roles all output µs per channel on the
 *   wire.  Effects don't want raw µs — they want a typed value:
 *     - "is the throttle toggle on?"           (Boolean)
 *     - "what's the current flight mode?"       (EnumN: 0..N-1)
 *     - "what's the throttle level?"            (ProportionalU8: 0..100)
 *     - "what's the aileron deflection?"        (ProportionalS8: -100..+100)
 *     - "pass the raw µs through for a servo"   (Raw)
 *
 *   `TriggerInput` does the µs → typed conversion with hysteresis /
 *   deadband and fires a registered callback only on actual value
 *   *changes*.  It's pure data + math — no transport.  The owning
 *   service (`InputDispatcher`) feeds it raw pulses from topology
 *   role events.
 *
 *   Failsafe behaviour is configurable: when SBUS reports failsafe
 *   or the signal-loss timeout fires, the input either holds the
 *   last value, snaps to "low" (Boolean → false, Prop → 0/-100,
 *   Enum → 0), or snaps to "high" (Boolean → true, Prop → 100/+100,
 *   Enum → N-1).  For flying models, fail-low is the right default
 *   (engine stops, gear deploys, gun safes on RC loss).
 */

#ifndef HUBFX_TRIGGER_INPUT_H
#define HUBFX_TRIGGER_INPUT_H

#include <cstdint>

namespace hubfx::effects::input {

enum class TriggerKind : uint8_t {
    Boolean        = 0,   ///< bool — toggle / button / arming switch
    EnumN          = 1,   ///< u8 (0..N-1) — N-position switch (2..6 typical)
    ProportionalU8 = 2,   ///< u8 (0..100) — throttle, dimmer
    ProportionalS8 = 3,   ///< i8 (-100..+100) — bipolar: aileron / trim
    Raw            = 4,   ///< u16 µs passthrough — servo mirror / debug
};

enum class FailsafeBehaviour : uint8_t {
    Hold      = 0,   ///< keep last accepted value, mark valid=false
    ForceLow  = 1,   ///< Bool=false, Prop=min, Enum=0
    ForceHigh = 2,   ///< Bool=true,  Prop=max, Enum=N-1
};

/// Configuration for one trigger input.  Irrelevant fields for the
/// chosen kind are simply ignored — there's no per-kind sub-struct
/// because the table stays flat and the fields are tiny.
struct TriggerMapping {
    TriggerKind       kind          = TriggerKind::Boolean;
    uint16_t          inputMinUs    = 1000;     ///< µs at the "0% / -100% / false" end
    uint16_t          inputMaxUs    = 2000;     ///< µs at the "100% / +100% / true" end
    bool              reversed      = false;    ///< swap min↔max

    // Boolean
    uint16_t          thresholdUs   = 1500;
    uint16_t          hysteresisUs  = 50;

    // EnumN
    uint8_t           enumPositions = 3;        ///< 2..6 supported

    // ProportionalS8 only
    uint8_t           centerDeadbandPct = 2;    ///< % of span at output==0

    FailsafeBehaviour failsafe      = FailsafeBehaviour::ForceLow;
};

/// Decoded value handed back by `TriggerInput::feed()` and to the
/// registered callback.  `changed == true` is the edge signal.
///
/// `initial == true` marks the FIRST callback after a `configure()` —
/// the value the channel happened to hold at subscribe time (boot,
/// config-reload), NOT a deliberate transition.  Level consumers (e.g.
/// the LightFx program selector — adopt the switch position at boot)
/// ignore it; edge consumers (e.g. the EngineFx start/stop toggle —
/// must not fire on boot) gate on `!initial`.  Think of it as the
/// stream's "current value" replay vs. a real change.
struct TriggerValue {
    TriggerKind kind;
    union {
        bool     b;
        uint8_t  e;     // EnumN position 0..N-1
        uint8_t  u;     // ProportionalU8 0..100
        int8_t   s;     // ProportionalS8 -100..+100
        uint16_t us;    // Raw passthrough
    };
    bool valid   = false;
    bool changed = false;
    bool initial = false;   ///< first event since configure() (baseline, not an edge)
};

class TriggerInput {
public:
    /// Called when the typed value crosses an edge (Boolean flip,
    /// EnumN position change, Prop delta ≥ 1 unit, Raw any change).
    /// Fires only when `changed == true`.
    using OnChangeFn = void (*)(void* ctx, const TriggerValue& value);

    TriggerInput() = default;

    void configure(const TriggerMapping& mapping,
                   OnChangeFn cb = nullptr, void* ctx = nullptr) {
        _mapping  = mapping;
        _cb       = cb;
        _ctx      = ctx;
        _haveLast = false;
        _last     = TriggerValue{};
        _last.kind = mapping.kind;
    }

    const TriggerMapping& mapping() const { return _mapping; }
    TriggerValue          last()    const { return _last; }

    /// Feed one raw pulse.  Computes the typed value, applies
    /// hysteresis / deadband / failsafe rules, sets `changed`, and
    /// fires the callback if changed.  Always returns the (now-current)
    /// value so the dispatcher can also use it directly.
    TriggerValue feed(uint16_t pulseUs, bool valid);

private:
    TriggerValue compute(uint16_t pulseUs) const;
    TriggerValue applyFailsafe() const;

    bool isChanged(const TriggerValue& v) const;
    void fireIfChanged(TriggerValue& v);

    // Per-kind math helpers.
    bool    toBoolean   (uint16_t us)             const;
    uint8_t toEnumN     (uint16_t us, uint8_t N)  const;
    uint8_t toPropU8    (uint16_t us)             const;
    int8_t  toPropS8    (uint16_t us)             const;

    TriggerMapping _mapping{};
    TriggerValue   _last{};
    bool           _haveLast = false;
    OnChangeFn     _cb       = nullptr;
    void*          _ctx      = nullptr;
};

}  // namespace hubfx::effects::input

#include "trigger_input.ipp"

#endif  // HUBFX_TRIGGER_INPUT_H
