// servo_calibration.ts — store + helpers for the ServoCalibrationDialog.
//
// The dialog is a modal overlay (driven by `openServoCalibration`) that
// gives the operator live ±µs jog buttons + min/max/speed/accel/jerk
// editing + Save / Cancel — for one port at a time.  (The reversed flag
// was retired in 2.46.0 — direction lives as absolute open/close µs in
// each effect's config, set with the ServoEndpointsDialog.)
//
// Two key UX patterns from the archived dialog (kept here):
//   1. **Widen-during-calibration** — on open we push a temporary
//      profile with min=300 / max=2700 + fast slew so the operator
//      can jog the servo across its full physical range to find the
//      end-stops; Save commits the operator-picked min/max, Cancel
//      restores the original profile.
//   2. **Explicit limit capture** — jogging NEVER changes the draft
//      min/max; only ⤓ Set as min / ⤒ Set as max / the numeric fields
//      do.  (The old "auto-expand on jog past limit" silently replaced
//      the operator's limits with wherever they swept — a full-range
//      sweep of a black-box retract controller always saved 800–2200,
//      the calibration envelope.  Bit on the 2026-08 bench; removed.)
//
// Live position polling (SERVO_GET_STATUS) is intentionally skipped
// today — the role's MotionProfile1D slews towards the commanded
// target; for a calibration session the operator-commanded target
// is the source of truth and live polling would add a 50 Hz query
// loop the wire doesn't need.  Easy to layer in later.

import { writable, get, type Writable } from 'svelte/store'
import { ServoSetTarget, SetPortProfile, ServoSetProfileLive, SaveHubConfig } from '../../wailsjs/go/main/App'
import { clearHubDirty, type Port } from './devicemodel'
import type { PortRefLike } from './components/port_keys'

// ─── DTO mirrors ──────────────────────────────────────────────────────

export interface ServoProfileT {
    minUs:              number
    maxUs:              number
    centerUs:           number
    maxSpeedUsPerSec:   number
    maxAccelUsPerSec2:  number
    maxJerkUsPerSec3:   number
}

export function defaultServoProfile(): ServoProfileT {
    return {
        minUs: 1000, maxUs: 2000, centerUs: 1500,
        maxSpeedUsPerSec: 800, maxAccelUsPerSec2: 1600, maxJerkUsPerSec3: 0,
    }
}

/** REACTIVE FACTORY for a port→profile lookup.  Pass the current device
 *  model (`$deviceModel`) so Svelte rebuilds the closure on every model
 *  change — calling it directly in `$:` froze calibration on first render
 *  (the landing-panel trap).  Returns a fresh copy of the port's servo
 *  profile, or null if the ref is unset / has no profile.  Was copy-pasted
 *  into Gear/GunFx/Landing — hoisted 2026-06-13. */
export function makeProfileForPort(dm: { ports: readonly Port[] }) {
    return (port: PortRefLike | null | undefined): ServoProfileT | null => {
        if (!port || !port.kind) return null
        for (const p of dm.ports) {
            if (p.ref.guid === port.guid && p.kindName === port.kind
                && p.ref.index === port.idx && p.profile) {
                return { ...p.profile } as ServoProfileT
            }
        }
        return null
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
    maxSpeedUsPerSec: 4000, maxAccelUsPerSec2: 8000, maxJerkUsPerSec3: 0,
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
        // the operator's draft min/max are still narrow.  LIVE-ONLY push
        // (role, not overlay): the envelope is session state and must
        // never be persistable — an Apply or link drop mid-dialog used
        // to write 800–2200/4000 into /hubfx.yaml as the servo's profile.
        await pushProfileLive(guid, portIdx, { ...CAL_ENVELOPE })
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
 *  which is unsafe-fast but functionally fine until the next reload.
 *  Live-only restore — the overlay never held the envelope, so there
 *  is nothing to undo there. */
export async function cancelServoCalibration(): Promise<void> {
    const s = get(openServoCalibration)
    if (!s) return
    try {
        await pushProfileLive(s.guid, s.portIdx, s.origin)
        await ServoSetTarget(s.guid, s.portIdx, s.origin.centerUs)
    } catch (e) {
        // Best-effort — log via the dialog's error banner before close.
        // eslint-disable-next-line no-console
        console.warn('servo calibration cancel restore failed', e)
    }
    openServoCalibration.set(null)
}

/** Close the dialog WITHOUT any wire traffic — the disconnect path.
 *  The link is gone, so the origin restore cannot be delivered; on
 *  reconnect the role still runs the widened envelope until the next
 *  calibrate/Apply, but the overlay (and thus /hubfx.yaml) was never
 *  touched, so nothing persists. */
export function closeServoCalibrationSilent(): void {
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
        // overlay.  Then PERSIST IMMEDIATELY (SaveHubConfig): calibration
        // is DEVICE state, not draft config — parking it behind the global
        // Apply left a window where a board unplug/reboot re-stamped the
        // role from the stale /hubfx.yaml and silently reverted the
        // calibration (incl. the REV flag — the "gear ignores my reverse"
        // bench saga, 2026-08-08 evening).  Save = durable, full stop.
        await SetPortProfile(s.guid, /*ServoKind=*/1, s.portIdx, s.draft as any)
        await SaveHubConfig()
        clearHubDirty()
        // Park at center so the saved limits are exercised on the next
        // operator-driven deploy/retract (no surprise mid-range hold).
        await ServoSetTarget(s.guid, s.portIdx, s.draft.centerUs)
        openServoCalibration.set(null)
    } catch (e) {
        openServoCalibration.update(x => x ? { ...x, busy: false, error: String(e) } : x)
    }
}

// ─── Jog ──────────────────────────────────────────────────────────────

/** Move the servo by `delta` µs from the current jog target.  Clamped
 *  to the CALIBRATION envelope so we never command outside the
 *  physically-safe range.  Jogging NEVER touches the draft min/max —
 *  exploring/sweeping is free; limits change only via the capture
 *  buttons or the numeric fields (the old auto-expand silently saved
 *  the sweep extents as the limits). */
export async function jogServo(delta: number): Promise<void> {
    const s = get(openServoCalibration)
    if (!s || s.busy) return
    let next = s.currentUs + delta
    if (next < CAL_ENVELOPE.minUs) next = CAL_ENVELOPE.minUs
    if (next > CAL_ENVELOPE.maxUs) next = CAL_ENVELOPE.maxUs
    openServoCalibration.update(x => x ? { ...x, currentUs: next } : x)
    try {
        await ServoSetTarget(s.guid, s.portIdx, next)
    } catch (e) {
        openServoCalibration.update(x => x ? { ...x, error: String(e) } : x)
    }
}

/** Jog directly to an absolute µs target (used by the slider).  Same
 *  contract as jogServo: position only, never the draft limits. */
export async function jogServoTo(targetUs: number): Promise<void> {
    const s = get(openServoCalibration)
    if (!s || s.busy) return
    const next = clamp(targetUs, CAL_ENVELOPE.minUs, CAL_ENVELOPE.maxUs)
    openServoCalibration.update(x => x ? { ...x, currentUs: next } : x)
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

/** Edit a single draft field (speed / accel / jerk / etc).
 *  No wire push here — Save batches the full profile in one shot. */
export function setDraftField<K extends keyof ServoProfileT>(key: K, val: ServoProfileT[K]): void {
    openServoCalibration.update(s => s
        ? { ...s, draft: { ...s.draft, [key]: val } }
        : s)
}

// ─── Helpers ──────────────────────────────────────────────────────────

function clamp(v: number, lo: number, hi: number): number {
    return v < lo ? lo : v > hi ? hi : v
}

/** Internal — TRANSIENT profile push (widen on open / origin restore on
 *  cancel): goes to the ROLE only, never the overlay, never hub-dirty.
 *  Only Save writes through SetPortProfile so /hubfx.yaml can only ever
 *  receive an operator-confirmed profile. */
async function pushProfileLive(guid: string, portIdx: number, p: ServoProfileT): Promise<void> {
    await ServoSetProfileLive(guid, portIdx, p as any)
}

/** Format a profile as a one-line summary for the compact widget.
 *  E.g. "1100–1900 µs · 800 µs/s · rev" or "1000–2000 µs · 800 µs/s". */
export function summariseProfile(p: ServoProfileT | null | undefined): string {
    if (!p) return 'no profile (role defaults)'
    const range = `${p.minUs}–${p.maxUs} µs`
    const speed = p.maxSpeedUsPerSec > 0 ? ` · ${p.maxSpeedUsPerSec} µs/s` : ' · unlimited'
    const accel = p.maxAccelUsPerSec2 > 0 ? ` · a${p.maxAccelUsPerSec2}` : ''
    const jerk  = p.maxJerkUsPerSec3  > 0 ? ` · j${p.maxJerkUsPerSec3}` : ''
    return range + speed + accel + jerk
}
