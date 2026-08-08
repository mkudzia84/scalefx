// endpoint_setter.ts — store + helpers for the ServoEndpointsDialog.
//
// THE 2.46.0 position model (explicit beats implicit): a function's two
// named positions (door open/close, strut deploy/retract, landing
// open/close) are ABSOLUTE µs numbers stored in the EFFECT's config.
// The servo's calibration is a pure motion CAP (min/max/speed) — there
// is no direction flag anywhere; which number is bigger IS the direction.
//
// This dialog is the setter: live-jog the servo inside its calibrated
// caps, park it where the mechanism should sit for position A (e.g.
// "Open"), capture, same for B, Save.  Save hands both numbers to the
// caller's onSave, which writes them into the owning effect's config
// AND persists immediately (device state, not draft config — the
// Save-vs-Apply gap lesson from the calibration dialog applies here
// identically).
//
// The µs sentinels 0xFFFF ("calibrated max") and 0 ("calibrated min")
// mark an endpoint the operator has not set yet — the firmware clamps
// them to the servo's calibrated ends, and the dialog renders them as
// the corresponding cap value.

import { writable, get, type Writable } from 'svelte/store'
import { ServoSetTarget } from '../../wailsjs/go/main/App'

export const US_CAL_MAX = 0xFFFF   // sentinel: "the servo's calibrated max"
export const US_CAL_MIN = 0        // sentinel: "the servo's calibrated min"

export interface OpenEndpointsT {
    guid:      string
    portIdx:   number
    portLabel: string              // header text, e.g. "SRV9 · Main R Doors"
    labelA:    string              // e.g. "Open" / "Deploy"
    labelB:    string              // e.g. "Closed" / "Retract"
    /// The servo's calibrated caps — the jog range.
    minUs:     number
    maxUs:     number
    /// Draft endpoint values (µs; sentinels resolved to caps at open).
    aUs:       number
    bUs:       number
    currentUs: number              // live jog position
    busy:      boolean
    error:     string
    onSave:    (aUs: number, bUs: number) => Promise<void>
}

export const openEndpoints: Writable<OpenEndpointsT | null> = writable(null)

function clamp(v: number, lo: number, hi: number): number {
    return v < lo ? lo : v > hi ? hi : v
}

/** Resolve a stored endpoint (possibly a sentinel) to a concrete µs
 *  inside the servo's caps. */
export function resolveEndpointUs(us: number, minUs: number, maxUs: number): number {
    if (us >= US_CAL_MAX || us > maxUs) return maxUs
    if (us <= US_CAL_MIN || us < minUs) return minUs
    return us
}

/** One-line summary for panel rows: "open 2050 · close 1390". */
export function summariseEndpoints(labelA: string, aUs: number,
                                   labelB: string, bUs: number,
                                   minUs: number, maxUs: number): string {
    const fmt = (us: number) =>
        us >= US_CAL_MAX ? `cal.max` : us <= US_CAL_MIN ? `cal.min` : `${us}`
    void minUs; void maxUs
    return `${labelA.toLowerCase()} ${fmt(aUs)} · ${labelB.toLowerCase()} ${fmt(bUs)} µs`
}

/** Open the dialog.  `aUs`/`bUs` may be sentinels; the jog starts at the
 *  resolved A position so the operator sees where "A" currently lands. */
export async function openEndpointsFor(opts: {
    guid: string, portIdx: number, portLabel: string,
    labelA: string, labelB: string,
    minUs: number, maxUs: number,
    aUs: number, bUs: number,
    onSave: (aUs: number, bUs: number) => Promise<void>,
}): Promise<void> {
    const a = resolveEndpointUs(opts.aUs, opts.minUs, opts.maxUs)
    const b = resolveEndpointUs(opts.bUs, opts.minUs, opts.maxUs)
    openEndpoints.set({
        guid: opts.guid, portIdx: opts.portIdx, portLabel: opts.portLabel,
        labelA: opts.labelA, labelB: opts.labelB,
        minUs: opts.minUs, maxUs: opts.maxUs,
        aUs: a, bUs: b, currentUs: a,
        busy: true, error: '',
        onSave: opts.onSave,
    })
    try {
        // Park at A so the first jog moves from a known point.  The role
        // clamps to the calibration caps — no widen needed (or wanted:
        // positions must live inside the caps by definition).
        await ServoSetTarget(opts.guid, opts.portIdx, a)
    } catch (e) {
        openEndpoints.update(s => s ? { ...s, error: String(e) } : s)
    }
    openEndpoints.update(s => s ? { ...s, busy: false } : s)
}

/** Jog by ±delta µs (clamped to the servo's caps). */
export async function jogEndpoint(delta: number): Promise<void> {
    const s = get(openEndpoints)
    if (!s || s.busy) return
    const next = clamp(s.currentUs + delta, s.minUs, s.maxUs)
    openEndpoints.update(x => x ? { ...x, currentUs: next } : x)
    try {
        await ServoSetTarget(s.guid, s.portIdx, next)
    } catch (e) {
        openEndpoints.update(x => x ? { ...x, error: String(e) } : x)
    }
}

/** Jog to an absolute µs (slider). */
export async function jogEndpointTo(targetUs: number): Promise<void> {
    const s = get(openEndpoints)
    if (!s || s.busy) return
    const next = clamp(targetUs, s.minUs, s.maxUs)
    openEndpoints.update(x => x ? { ...x, currentUs: next } : x)
    try {
        await ServoSetTarget(s.guid, s.portIdx, next)
    } catch (e) {
        openEndpoints.update(x => x ? { ...x, error: String(e) } : x)
    }
}

/** Capture the current jog position as endpoint A / B. */
export function captureA(): void {
    openEndpoints.update(s => s ? { ...s, aUs: s.currentUs } : s)
}
export function captureB(): void {
    openEndpoints.update(s => s ? { ...s, bUs: s.currentUs } : s)
}

/** Drive the servo to the current DRAFT A/B (preview buttons). */
export async function previewA(): Promise<void> {
    const s = get(openEndpoints)
    if (!s || s.busy) return
    openEndpoints.update(x => x ? { ...x, currentUs: s.aUs } : x)
    try { await ServoSetTarget(s.guid, s.portIdx, s.aUs) }
    catch (e) { openEndpoints.update(x => x ? { ...x, error: String(e) } : x) }
}
export async function previewB(): Promise<void> {
    const s = get(openEndpoints)
    if (!s || s.busy) return
    openEndpoints.update(x => x ? { ...x, currentUs: s.bUs } : x)
    try { await ServoSetTarget(s.guid, s.portIdx, s.bUs) }
    catch (e) { openEndpoints.update(x => x ? { ...x, error: String(e) } : x) }
}

/** Edit a draft field numerically. */
export function setEndpointField(which: 'a' | 'b', us: number): void {
    openEndpoints.update(s => {
        if (!s) return s
        const v = clamp(Math.round(us), s.minUs, s.maxUs)
        return which === 'a' ? { ...s, aUs: v } : { ...s, bUs: v }
    })
}

/** Save — hand the two numbers to the owner (which persists immediately),
 *  then close.  On failure the dialog stays open with the error shown. */
export async function saveEndpoints(): Promise<void> {
    const s = get(openEndpoints)
    if (!s) return
    openEndpoints.update(x => x ? { ...x, busy: true, error: '' } : x)
    try {
        await s.onSave(s.aUs, s.bUs)
        openEndpoints.set(null)
    } catch (e) {
        openEndpoints.update(x => x ? { ...x, busy: false, error: String(e) } : x)
    }
}

/** Cancel / close (no wire restore needed — jogging inside the caps is
 *  harmless and the next effect command re-positions the servo). */
export function cancelEndpoints(): void {
    openEndpoints.set(null)
}

/** Silent close for the disconnect path. */
export function closeEndpointsSilent(): void {
    openEndpoints.set(null)
}
