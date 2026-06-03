// range-suggest.ts — shared "auto-pick the next band" helper.
//
// Used by every µs-band-with-overlay widget (GunFx ROF bands, LightFx
// program selector ranges, future expander dispatchers).  Operators
// don't want hand-edited µs ranges to TOUCH — adjacent bands sharing
// a µs boundary make the stick flap between them at the edge, and
// visually they look like one continuous block instead of two.  This
// helper places a small GUARD GAP between adjacent ranges so newly
// added items always land with breathing room.
//
// Conventions:
//   - RC stick range is `[1000, 2000]` µs.  Other carriers can pass
//     `bounds`.
//   - GUARD is the buffer width between adjacent bands.  Default 1 µs —
//     bands sit right next to each other but never share a boundary µs.
//     Bands that already touch the carrier bound (1000 or 2000) do NOT
//     get a guard on that side — wasting carrier travel for an
//     edge-of-bar guard is silly.
//   - On bisect (no usable gap), the existing band is TRIMMED on its
//     high side so the operator gets two non-overlapping siblings in
//     one click; the caller applies `trimSiblingIdx` + `trimSiblingNewHi`
//     when mutating its draft.

export interface Band { lo: number; hi: number }

export interface SuggestResult {
    band: Band
    /** -1 when the suggester didn't need to bisect; otherwise the index
     *  of the existing band that must be trimmed (its high edge
     *  shortened to `trimSiblingNewHi`).  Caller's mutator applies the
     *  trim alongside inserting `band`. */
    trimSiblingIdx:    number
    trimSiblingNewHi:  number
}

export interface SuggestOpts {
    bounds:      [number, number]   // carrier range, e.g. [1000, 2000]
    guardUs:     number              // gap between adjacent bands
    minWidthUs:  number              // smallest band width the suggester emits
    stepUs:      number              // round µs to multiples of this (operator-tidy)
}

const DEFAULTS: SuggestOpts = {
    bounds:     [1000, 2000],
    // Bands sit RIGHT NEXT TO EACH OTHER with just a 1 µs guard between them
    // (enough that adjacent bands never share a boundary µs / read as an
    // overlap, but visually contiguous).  stepUs is 1 so that 1 µs gap survives
    // rounding — a coarser step would round the gap away to 0 (touching) or
    // blow it out to a step-sized gap.
    guardUs:    1,
    minWidthUs: 100,
    stepUs:     1,
}

/** Pick the next band for an existing list, honouring guard gaps.
 *
 *  Two paths:
 *    1. If a gap between existing bands (or at either carrier edge) is
 *       wide enough to hold `minWidth + 2*guard`, place the new band
 *       INSIDE the gap with `guard` shaved from each interior side.
 *       Edges-of-bar siblings skip the guard on the bound side so the
 *       new band can actually run to 1000 / 2000.
 *    2. Otherwise bisect the widest existing band: split it at the
 *       midpoint with `guard/2` shaved off each half so the resulting
 *       siblings don't touch.  Caller trims the original via
 *       `trimSiblingIdx` + `trimSiblingNewHi`.
 *
 *  Empty input → returns the full carrier as the first band (no trim).
 *  Pathological inputs (every band tiny / nothing splittable) → returns
 *  a fixed top-edge fallback so the caller never gets back something
 *  invalid.  Width filter discards inverted / zero-width siblings. */
export function suggestNextBand(existing: readonly Band[], opts: Partial<SuggestOpts> = {}): SuggestResult {
    const o = { ...DEFAULTS, ...opts }
    const [bLo, bHi] = o.bounds
    const round = (n: number) => Math.round(n / o.stepUs) * o.stepUs

    if (existing.length === 0) {
        return { band: { lo: bLo, hi: bHi }, trimSiblingIdx: -1, trimSiblingNewHi: 0 }
    }

    // Index-stamp + width-filter + sort.
    const sorted = existing.map((b, idx) => ({ lo: b.lo, hi: b.hi, idx }))
        .filter(b => b.hi > b.lo)
        .sort((a, b) => a.lo - b.lo)

    // ── Path 1: find a usable gap ─────────────────────────────────────
    let bestGap = { lo: 0, hi: 0, w: 0, atLeftBound: false, atRightBound: false }
    let cursor = bLo
    for (const b of sorted) {
        if (b.lo > cursor) {
            const w = b.lo - cursor
            if (w > bestGap.w) bestGap = {
                lo: cursor, hi: b.lo, w,
                atLeftBound:  cursor === bLo,
                atRightBound: false,
            }
        }
        cursor = Math.max(cursor, b.hi)
    }
    if (bHi > cursor) {
        const w = bHi - cursor
        if (w > bestGap.w) bestGap = {
            lo: cursor, hi: bHi, w,
            atLeftBound:  cursor === bLo,
            atRightBound: true,
        }
    }

    // Guard required only on the interior sides.
    const leftGuard  = bestGap.atLeftBound  ? 0 : o.guardUs
    const rightGuard = bestGap.atRightBound ? 0 : o.guardUs
    if (bestGap.w >= o.minWidthUs + leftGuard + rightGuard) {
        return {
            band: {
                lo: round(bestGap.lo + leftGuard),
                hi: round(bestGap.hi - rightGuard),
            },
            trimSiblingIdx: -1, trimSiblingNewHi: 0,
        }
    }

    // ── Path 2: bisect the widest existing band ──────────────────────
    let widest = sorted[0]
    for (const b of sorted) if (b.hi - b.lo > widest.hi - widest.lo) widest = b

    const mid       = (widest.lo + widest.hi) / 2
    const halfGuard = o.guardUs / 2
    const trimmedHi = round(mid - halfGuard)
    const newLo     = round(mid + halfGuard)
    const newHi     = widest.hi

    // Pathological: bisect leaves either side narrower than minWidth.
    // Drop a fallback at the top of the bar so the caller still gets a
    // valid result; existing bands stay untouched.
    if (trimmedHi - widest.lo < o.minWidthUs || newHi - newLo < o.minWidthUs) {
        return {
            band: { lo: round(bHi - o.minWidthUs * 2), hi: bHi },
            trimSiblingIdx: -1, trimSiblingNewHi: 0,
        }
    }

    return {
        band: { lo: newLo, hi: newHi },
        trimSiblingIdx: widest.idx, trimSiblingNewHi: trimmedHi,
    }
}
