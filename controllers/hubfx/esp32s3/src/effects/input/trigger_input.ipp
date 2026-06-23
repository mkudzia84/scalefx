/*
 * trigger_input.ipp — TriggerInput math + state-transition logic.
 */

#ifndef HUBFX_TRIGGER_INPUT_IPP
#define HUBFX_TRIGGER_INPUT_IPP

namespace hubfx::effects::input {

// ─── feed() — public entry point ────────────────────────────────────

inline TriggerValue TriggerInput::feed(uint16_t pulseUs, bool valid) {
    // Shadow the raw µs unconditionally so consumers (Studio verbose
    // status mirror) can surface the live µs trace even when the
    // trigger is subscribed as Boolean / EnumN / Prop*.
    _lastPulseUs    = pulseUs;
    _lastPulseValid = valid;

    TriggerValue v;
    v.kind = _mapping.kind;

    if (!valid) {
        v = applyFailsafe();
    } else {
        v = compute(pulseUs);
        v.valid = true;
    }
    v.changed = isChanged(v);
    v.initial = !_haveLast;     // first feed since configure() — baseline, not an edge
    fireIfChanged(v);
    return v;
}

// ─── compute(): kind-dispatch ───────────────────────────────────────

inline TriggerValue TriggerInput::compute(uint16_t pulseUs) const {
    TriggerValue v;
    v.kind = _mapping.kind;
    switch (_mapping.kind) {
        case TriggerKind::Boolean:        v.b  = toBoolean(pulseUs);                       break;
        case TriggerKind::EnumN:          v.e  = toEnumN(pulseUs, _mapping.enumPositions); break;
        case TriggerKind::ProportionalU8: v.u  = toPropU8(pulseUs);                        break;
        case TriggerKind::ProportionalS8: v.s  = toPropS8(pulseUs);                        break;
        case TriggerKind::Raw:            v.us = pulseUs;                                  break;
    }
    return v;
}

// ─── applyFailsafe(): build the value that maps to "no signal" ─────

inline TriggerValue TriggerInput::applyFailsafe() const {
    TriggerValue v;
    v.kind  = _mapping.kind;
    v.valid = false;
    if (_mapping.failsafe == FailsafeBehaviour::Hold && _haveLast) {
        v   = _last;
        v.valid = false;     // signal is gone even when value is held
        return v;
    }
    const bool high = (_mapping.failsafe == FailsafeBehaviour::ForceHigh);
    switch (_mapping.kind) {
        case TriggerKind::Boolean:
            v.b = high;
            break;
        case TriggerKind::EnumN:
            v.e = high ? static_cast<uint8_t>(_mapping.enumPositions - 1) : 0;
            break;
        case TriggerKind::ProportionalU8:
            v.u = high ? 100 : 0;
            break;
        case TriggerKind::ProportionalS8:
            v.s = static_cast<int8_t>(high ? 100 : -100);
            break;
        case TriggerKind::Raw:
            v.us = high ? _mapping.inputMaxUs : _mapping.inputMinUs;
            break;
    }
    return v;
}

// ─── change detection per kind ──────────────────────────────────────

inline bool TriggerInput::isChanged(const TriggerValue& v) const {
    if (!_haveLast) return true;
    if (v.kind != _last.kind) return true;
    if (v.valid != _last.valid) return true;
    switch (v.kind) {
        case TriggerKind::Boolean:        return v.b  != _last.b;
        case TriggerKind::EnumN:          return v.e  != _last.e;
        case TriggerKind::ProportionalU8: return v.u  != _last.u;
        case TriggerKind::ProportionalS8: return v.s  != _last.s;
        case TriggerKind::Raw:            return v.us != _last.us;
    }
    return false;
}

inline void TriggerInput::fireIfChanged(TriggerValue& v) {
    if (v.changed) {
        _last     = v;
        _haveLast = true;
        if (_cb) _cb(_ctx, v);
    }
}

// ─── Per-kind math ──────────────────────────────────────────────────

inline bool TriggerInput::toBoolean(uint16_t us) const {
    // Hysteresis around `thresholdUs`.  Direction respects `reversed`:
    // reversed=true means low µs is "on".
    const uint16_t hi = static_cast<uint16_t>(_mapping.thresholdUs + _mapping.hysteresisUs);
    const uint16_t lo = (_mapping.thresholdUs > _mapping.hysteresisUs)
                          ? static_cast<uint16_t>(_mapping.thresholdUs - _mapping.hysteresisUs)
                          : 0;
    const bool above = _haveLast && _last.b ? (us >= lo) : (us >= hi);
    return _mapping.reversed ? !above : above;
}

inline uint8_t TriggerInput::toEnumN(uint16_t us, uint8_t N) const {
    if (N < 2) return 0;
    if (N > 6) N = 6;

    // Optionally reverse the input range mapping.
    uint16_t loIn = _mapping.inputMinUs;
    uint16_t hiIn = _mapping.inputMaxUs;
    if (_mapping.reversed) {
        uint16_t t = loIn; loIn = hiIn; hiIn = t;
    }
    // Clamp.
    if (loIn == hiIn) return 0;
    const bool ascending = (hiIn > loIn);
    if (ascending) {
        if (us <= loIn) return 0;
        if (us >= hiIn) return static_cast<uint8_t>(N - 1);
    } else {
        if (us >= loIn) return 0;
        if (us <= hiIn) return static_cast<uint8_t>(N - 1);
    }
    const int32_t span    = (int32_t)hiIn - (int32_t)loIn;
    const int32_t offset  = (int32_t)us  - (int32_t)loIn;
    int32_t pos = (offset * (int32_t)N) / span;
    if (pos < 0)         pos = 0;
    if (pos >= (int32_t)N) pos = N - 1;

    // Hysteresis around each position boundary — keep the previous
    // position when we're within `hysteresisUs` of an edge.
    if (_haveLast && _last.kind == TriggerKind::EnumN) {
        const int32_t bandUs = (int32_t)_mapping.hysteresisUs;
        const int32_t step   = span / (int32_t)N;
        // `step` is negative for reversed (descending) channels — the band-edge
        // distance math below is sign-agnostic (offset, lower and upper all live
        // in the same signed offset-space, and the `±bandUs` comparison is
        // symmetric), so guard on `step != 0` to engage hysteresis in BOTH
        // directions rather than only when ascending.
        if (step != 0) {
            // Debounce BOTH boundaries adjacent to the previous band, not just
            // the upper one — hold `_last.e` while `offset` is within `bandUs`
            // of the lower edge (_last.e*step) OR the upper edge
            // ((_last.e+1)*step), so a value hovering on either side of a step
            // can't chatter between adjacent positions.
            const int32_t lower = (int32_t)_last.e * step;
            const int32_t upper = ((int32_t)_last.e + 1) * step;
            const int32_t dLower = offset - lower;
            const int32_t dUpper = offset - upper;
            if ((dLower > -bandUs && dLower < bandUs) ||
                (dUpper > -bandUs && dUpper < bandUs)) {
                return _last.e;
            }
        }
    }
    return static_cast<uint8_t>(pos);
}

inline uint8_t TriggerInput::toPropU8(uint16_t us) const {
    uint16_t loIn = _mapping.inputMinUs;
    uint16_t hiIn = _mapping.inputMaxUs;
    if (_mapping.reversed) { uint16_t t = loIn; loIn = hiIn; hiIn = t; }
    if (loIn == hiIn) return 0;
    const bool ascending = (hiIn > loIn);
    if (ascending) {
        if (us <= loIn) return 0;
        if (us >= hiIn) return 100;
        return static_cast<uint8_t>(((int32_t)us - (int32_t)loIn) * 100
                                    / ((int32_t)hiIn - (int32_t)loIn));
    }
    if (us >= loIn) return 0;
    if (us <= hiIn) return 100;
    return static_cast<uint8_t>(((int32_t)loIn - (int32_t)us) * 100
                                / ((int32_t)loIn - (int32_t)hiIn));
}

inline int8_t TriggerInput::toPropS8(uint16_t us) const {
    // Centre is the midpoint of [inputMin, inputMax].
    const int32_t midUs = ((int32_t)_mapping.inputMinUs + (int32_t)_mapping.inputMaxUs) / 2;
    const int32_t halfSpan = ((int32_t)_mapping.inputMaxUs - (int32_t)_mapping.inputMinUs) / 2;
    if (halfSpan <= 0) return 0;
    const int32_t deadbandUs = ((int32_t)halfSpan * (int32_t)_mapping.centerDeadbandPct) / 100;
    int32_t offset = (int32_t)us - midUs;
    if (_mapping.reversed) offset = -offset;
    if (offset > -deadbandUs && offset < deadbandUs) return 0;
    int32_t v = (offset * 100) / halfSpan;
    if (v >  100) v =  100;
    if (v < -100) v = -100;
    return static_cast<int8_t>(v);
}

}  // namespace hubfx::effects::input

#endif  // HUBFX_TRIGGER_INPUT_IPP
