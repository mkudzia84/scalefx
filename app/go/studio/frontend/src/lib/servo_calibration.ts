// servo_calibration.ts — store + helpers for the ServoCalibrationDialog.
//
// The dialog is a modal overlay (driven by `openServoCalibration`) that
// gives the operator live ±µs jog buttons + min/max/speed/accel/jerk/
// reversed editing + Save / Cancel — for one port at a time.
//
// Two key UX patterns from the archived dialog (kept here):
//   1. **Widen-during-calibration** — on open we push a temporary
//      profile with min=300 / max=2700 + fast slew so the operator
//      can jog the servo across its full physical range to find the
//      end-stops; Save commits the operator-picked min/max, Cancel
//      restores the original profile.
//   2. **Auto-expand on jog past limit** — if the operator jogs the
//      target ABOVE the current `draft.maxUs`, the draft max moves
//      with them.  Same for min in the other direction.  This keeps
//      the "jog past the edge" gesture from being silently clamped
//      out of view.
//
// Live position polling (SERVO_GET_STATUS) is intentionally skipped
// today — the role's MotionProfile1D slews towards the commanded
// target; for a calibration session the operator-commanded target
// is the source of truth and live polling would add a 50 Hz query
// loop the wire doesn't need.  Easy to layer in later.

import { writable, get, type Writable } from 'svelte/store'
import { ServoSetTarget, SetPortProfile } from '../../wailsjs/go/main/App'
import { markHubDirty } from './devicemodel'

// ─── DTO mirrors ──────────────────────────────────────────────────────

export interface ServoProfileT {
    minUs:              number
    maxUs:              number
    centerUs:           number
    reversed:           boolean
    maxSpeedUsPerSec:   number
    maxAccelUsPerSec2:  number
    maxJerkUsPerSec3:   number
}

export function defaultServoProfile(): ServoProfileT {
    return {
        minUs: 1000, maxUs: 2000, centerUs: 1500,
        reversed: false,
        maxSpeedUsPerSec: 800, maxAccelUsPerSec2: 1600, maxJerkUsPerSec3: 0,
    }
}

// ─── Open-session state ───────────────────────────────────────────────

export interface OpenServoCalT {
    guid:       string             // hub-local = ''
    portIdx:    number
    portLabel:  string             // "Hub · IN_3 (5 V)" etc — header text
    origin:     ServoProfileT      // for Cancel restore
    draft:      ServoProfileT      // what Save commits
    currentUs:  number             // last commanded jog target
    busy:       boolean
    error:      string
}

export const openServoCalibration: Writable<OpenServoCalT | null> = writable(null)

// ─── Lifecycle ────────────────────────────────────────────────────────

/** Hard-coded "calibration-safe" envelope — what we widen the live
 *  profile to so jogging covers the working range during calibration.
 *  800–2200 µs brackets the normal 1000–2000 RC band with end-stop-trim
 *  headroom, while staying clear of the far mechanical extremes.  Fast
 *  slew so each jog click feels instant.  (Keep in lock-step with the
 *  dialog's SLIDER_MIN/MAX.) */
const CAL_ENVELOPE: ServoProfileT = {
    minUs: 800, maxUs: 2200, centerUs: 1500,
    reversed: false,                          // direction matters for the operator's mental model — keep their setting
    maxSpeedUsPerSec: 4000, maxAccelUsPerSec2: 8000, maxJerkUsPerSec3: 0,
}
function widenedFrom(origin: ServoProfileT): ServoProfileT {
    return { ...CAL_ENVELOPE, reversed: origin.reversed }
}

/** Open the calibration dialog for `(guid, portIdx)`.  Seeds the
 *  draft from `current`, pushes the widened calibration profile to
 *  the role so jogging is unconstrained, jogs to the current target
 *  so the operator starts from the live position. */
export async function openServoCalibrationFor(
    guid: string, portIdx: number, portLabel: string,
    current: ServoProfileT, startingTargetUs?: number,
): Promise<void> {
    const origin = { ...current }
    const startUs = startingTargetUs ?? clamp(current.centerUs, current.minUs, current.maxUs)
    openServoCalibration.set({
        guid, portIdx, portLabel,
        origin, draft: { ...current },
        currentUs: startUs,
        busy: true, error: '',
    })
    try {
        // Widen — so the jog buttons can travel the full range even if
        // the operator's draft min/max are still narrow.  Push directly
        // via SetPortProfile so the overlay also reflects it (cancelled
        // on close).
        await pushProfile(guid, portIdx, widenedFrom(origin))
        // Park at the starting position so the operator's first jog
        // moves a known distance from a known point.
        await ServoSetTarget(guid, portIdx, startUs)
    } catch (e) {
        openServoCalibration.update(s => s ? { ...s, error: String(e) } : s)
    }
    openServoCalibration.update(s => s ? { ...s, busy: false } : s)
}

/** Cancel — restore the original profile + park at center.  The
 *  dialog closes regardless of whether the restore succeeded; a
 *  failure here just means the role is left at the widened envelope,
 *  which is unsafe-fast but functionally fine until the next reload. */
export async function cancelServoCalibration(): Promise<void> {
    const s = get(openServoCalibration)
    if (!s) return
    try {
        await pushProfile(s.guid, s.portIdx, s.origin)
        await ServoSetTarget(s.guid, s.portIdx, s.origin.centerUs)
    } catch (e) {
        // Best-effort — log via the dialog's error banner before close.
        // eslint-disable-next-line no-console
        console.warn('servo calibration cancel restore failed', e)
    }
    openServoCalibration.set(null)
}

/** Save — push the operator's draft as the new profile, persist it
 *  to /hubfx.yaml via SetPortProfile (which marks the hub dirty +
 *  fires the live push), park at center.  Leaves the dialog open
 *  with `.error` set on failure so the operator can retry. */
export async function saveServoCalibration(): Promise<void> {
    const s = get(openServoCalibration)
    if (!s) return
    openServoCalibration.update(x => x ? { ...x, busy: true, error: '' } : x)
    try {
        // SetPortProfile pushes live to the role + updates the studio
        // overlay, but does NOT persist to /hubfx.yaml — that happens on
        // Apply (SaveHubConfig).  Raise the hub-dirty flag so the global
        // ConfigToolbar lights up and the operator can Apply (Rule 46).
        // Without this the saved profile is live on the role but the
        // toolbar reads "in sync" and the operator can't persist it.
        await SetPortProfile(s.guid, /*ServoKind=*/1, s.portIdx, s.draft as any)
        markHubDirty()
        // Park at center so the saved limits are exercised on the next
        // operator-driven deploy/retract (no surprise mid-range hold).
        await ServoSetTarget(s.guid, s.portIdx, s.draft.centerUs)
        openServoCalibration.set(null)
    } catch (e) {
        openServoCalibration.update(x => x ? { ...x, busy: false, error: String(e) } : x)
    }
}

// ─── Jog ──────────────────────────────────────────────────────────────

/** Move the servo by `delta` µs from the current jog target.  If
 *  the new target would land outside the DRAFT's min/max range, the
 *  draft expands to include it — the operator's edge-finding gesture
 *  is the authoritative declaration of the limit.  Clamped to the
 *  CALIBRATION envelope so we never command outside the
 *  physically-safe range (300–2700 µs). */
export async function jogServo(delta: number): Promise<void> {
    const s = get(openServoCalibration)
    if (!s || s.busy) return
    let next = s.currentUs + delta
    if (next < CAL_ENVELOPE.minUs) next = CAL_ENVELOPE.minUs
    if (next > CAL_ENVELOPE.maxUs) next = CAL_ENVELOPE.maxUs
    // Auto-expand the draft min/max if the jog passes them.
    const draft = { ...s.draft }
    if (next < draft.minUs) draft.minUs = next
    if (next > draft.maxUs) draft.maxUs = next
    openServoCalibration.update(x => x ? { ...x, draft, currentUs: next } : x)
    try {
        await ServoSetTarget(s.guid, s.portIdx, next)
    } catch (e) {
        openServoCalibration.update(x => x ? { ...x, error: String(e) } : x)
    }
}

/** Jog directly to an absolute µs target (used by the slider + the
 *  "set as min" / "set as max" / "go to center" actions). */
export async function jogServoTo(targetUs: number): Promise<void> {
    const s = get(openServoCalibration)
    if (!s || s.busy) return
    const next = clamp(targetUs, CAL_ENVELOPE.minUs, CAL_ENVELOPE.maxUs)
    const draft = { ...s.draft }
    if (next < draft.minUs) draft.minUs = next
    if (next > draft.maxUs) draft.maxUs = next
    openServoCalibration.update(x => x ? { ...x, draft, currentUs: next } : x)
    try {
        await ServoSetTarget(s.guid, s.portIdx, next)
    } catch (e) {
        openServoCalibration.update(x => x ? { ...x, error: String(e) } : x)
    }
}

/** Capture the current jog target as the new min (or max). */
export function captureAsMin(): void {
    openServoCalibration.update(s => s
        ? { ...s, draft: { ...s.draft, minUs: s.currentUs } }
        : s)
}
export function captureAsMax(): void {
    openServoCalibration.update(s => s
        ? { ...s, draft: { ...s.draft, maxUs: s.currentUs } }
        : s)
}
export function captureAsCenter(): void {
    openServoCalibration.update(s => s
        ? { ...s, draft: { ...s.draft, centerUs: s.currentUs } }
        : s)
}

/** Edit a single draft field (speed / accel / jerk / reversed / etc).
 *  No wire push here — Save batches the full profile in one shot. */
export function setDraftField<K extends keyof ServoProfileT>(key: K, val: ServoProfileT[K]): void {
    openServoCalibration.update(s => s
        ? { ...s, draft: { ...s.draft, [key]: val } }
        : s)
}

// ─── Lightweight outside-dialog reversed toggle ──────────────────────
//
// The compact ServoWidget on the form has a "↔ Reversed" button that
// flips `profile.reversed` and saves immediately without opening the
// dialog — same single round-trip as any other live-push field.

export async function toggleReversed(
    guid: string, portIdx: number, current: ServoProfileT,
): Promise<ServoProfileT> {
    const next = { ...current, reversed: !current.reversed }
    await SetPortProfile(guid, /*ServoKind=*/1, portIdx, next as any)
    markHubDirty()   // reversed persists into /hubfx.yaml ports[].profile (Rule 46)
    return next
}

// ─── Helpers ──────────────────────────────────────────────────────────

function clamp(v: number, lo: number, hi: number): number {
    return v < lo ? lo : v > hi ? hi : v
}

/** Internal — push a profile to a hub-local port directly via the
 *  SetPortProfile Wails surface (which also marks hub-config dirty).
 *  For calibration we WANT the dirty flag on Cancel too, because the
 *  cancel path restores ORIGIN — which itself differs from whatever
 *  was on the device before the operator started fiddling.  In
 *  practice ORIGIN === device-current at open, so the diff is zero
 *  and dirty stays at its prior value. */
async function pushProfile(guid: string, portIdx: number, p: ServoProfileT): Promise<void> {
    await SetPortProfile(guid, /*ServoKind=*/1, portIdx, p as any)
}

/** Format a profile as a one-line summary for the compact widget.
 *  E.g. "1100–1900 µs · 800 µs/s · rev" or "1000–2000 µs · 800 µs/s". */
export function summariseProfile(p: ServoProfileT | null | undefined): string {
    if (!p) return 'no profile (role defaults)'
    const range = `${p.minUs}–${p.maxUs} µs`
    const speed = p.maxSpeedUsPerSec > 0 ? ` · ${p.maxSpeedUsPerSec} µs/s` : ' · unlimited'
    const accel = p.maxAccelUsPerSec2 > 0 ? ` · a${p.maxAccelUsPerSec2}` : ''
    const jerk  = p.maxJerkUsPerSec3  > 0 ? ` · j${p.maxJerkUsPerSec3}` : ''
    const rev   = p.reversed ? ' · ↔ rev' : ''
    return range + speed + accel + jerk + rev
}
