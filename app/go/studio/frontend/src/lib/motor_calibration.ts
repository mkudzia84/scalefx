// motor_calibration.ts — store + helpers for the MotorCalibrationDialog, the
// H-bridge gear-motor analogue of servo_calibration.ts.
//
// A servo calibration is about TRAVEL LIMITS (min/max µs); a gear motor has no
// position feedback, so its calibration is about (a) finding a DUTY that drives
// the leg reliably, (b) measuring the full-stroke TRAVEL TIME (→ a travel
// timeout), and (c) tuning the STALL GUARD that decides the motor hit the
// endstop.  The dialog drives the BiDcMotor role live (calibrate sweep, move-
// to-end, hold-jog, guard retune) and reads back travel-time + peak current.
//
// Two persistence tiers, matching what the firmware actually keeps:
//   • duty + travel-timeout  → committed back into the gear channel draft
//     (GearChannelT.deployDuty / retractDuty / timeoutMs, persisted to
//     /gearcontrol.yaml on Apply) via the `onCommit` callback.
//   • stall guard            → pushed LIVE to the role only (the role's guard
//     isn't a persisted YAML field today — same as the diagnostics tab).  The
//     gear effect re-attaches a default guard at boot; live tuning is a
//     bench/session activity.

import { writable, get, type Writable } from 'svelte/store'
import {
    GearMotorCalibrate, GearMotorMoveEnd, GearMotorJog, GearMotorStop,
    GearMotorGuardFixed, GearMotorGuardLiveRatio,
} from '../../wailsjs/go/main/App'
import { pollMotorStatus } from './motor_status'

export type GuardMode = 'live' | 'fixed'

/** What `Save to strut` writes back into the gear channel draft.  `guard` is
 *  the persisted stall-guard calibration (mirrors GearGuardT in gear.ts) —
 *  ratioX100 = ratio×100. */
export interface MotorCommitT {
    deployDuty:  number
    retractDuty: number
    timeoutMs:   number
    /** Motor rated voltage (mV) — firmware caps |duty| at maxDuty × this /
     *  rail_mV.  0 = cap off. */
    motorVoltageMv: number
    guard: {
        mode: GuardMode
        ratioX100: number
        sampleMs: number
        windowMs: number
        thresholdMa: number
        ceilingMa: number
    }
}

/** One leg / move outcome (mirror of Go DiagEndstopResult). */
export interface EndstopResultT {
    outcome: number; outcomeName: string
    travelMs: number; peakMa: number
    position: number; positionName: string
    end?: string
}
export interface CalibrationT { legA: EndstopResultT; legB: EndstopResultT }

export interface OpenMotorCalT {
    guid:      string
    portIdx:   number          // hbridge index
    label:     string          // "Hub · MOT_1 (12 V)" header text

    // Seek / jog drive magnitude (port-native duty, same scale as the gear
    // channel's deployDuty).  Seeded from the strut.
    duty:      number
    timeoutS:  number          // per-leg seek timeout, seconds
    motorVoltageMv: number     // rated-voltage duty cap (mV); 0 = off

    // Stall-guard config (live push).
    guardMode:  GuardMode
    ratio:      number         // ×  (LiveRatio), e.g. 2.5
    sample:     number         // ms baseline window (LiveRatio)
    window:     number         // ms sustained-confirm (both modes)
    threshold:  number         // mA (Fixed)
    ceiling:    number         // mA absolute backstop (LiveRatio); 0 = off

    // Last move/calibration outcome (one of these is set).
    move:   EndstopResultT | null
    calib:  CalibrationT  | null

    rowBusy:  '' | 'moving' | 'calibrating'
    busy:     boolean
    error:    string

    // Current strut values (seed). `Save to strut` derives the new values from
    // `duty` + the calibration result and hands them to onCommit; `timeoutMs`
    // is the fallback when no sweep ran.
    deployDuty:  number
    retractDuty: number
    timeoutMs:   number
    onCommit: ((c: MotorCommitT) => void) | null
}

export const openMotorCalibration: Writable<OpenMotorCalT | null> = writable(null)

/** Compute the strut's deploy/retract duties for "Save to strut": the MAGNITUDE
 *  comes from the calibrated seek duty, but the operator's Forward/Reverse
 *  DIRECTION is preserved from the sign of the strut's existing deployDuty.
 *  (Hard-coding +abs/-abs here silently reset a Reverse strut to Forward — the
 *  "calibration resets the gear direction" bug.) */
export function commitDuties(deploySeed: number, calibratedDuty: number): { deployDuty: number; retractDuty: number } {
    const mag = Math.abs(calibratedDuty)
    const forward = (deploySeed ?? 0) >= 0
    return { deployDuty: forward ? mag : -mag, retractDuty: forward ? -mag : mag }
}

// ─── Lifecycle ────────────────────────────────────────────────────────

export interface OpenMotorCalArgs {
    guid: string
    portIdx: number
    label: string
    deployDuty: number          // current strut values (seed)
    retractDuty: number
    timeoutMs: number
    motorVoltageMv: number      // strut's rated-voltage cap (seed); 0 = off
    // The strut's SAVED stall guard (seed) so the dialog opens on the last
    // calibration; absent → LiveRatio default.  ratioX100 = ratio×100.
    guard?: { mode: GuardMode; ratioX100: number; sampleMs: number; windowMs: number; thresholdMa: number; ceilingMa: number }
    onCommit: (c: MotorCommitT) => void
}

/** Open the calibration dialog for one gear motor.  Seeds the seek duty from
 *  the strut's deploy magnitude, defaults the stall guard to LiveRatio (so the
 *  calibrate sweep trips on the endstop out of the box — a fresh BiDcMotor
 *  comes up Fixed-2000 mA which most gear motors never reach), and reads the
 *  first live sample. */
export async function openMotorCalibrationFor(args: OpenMotorCalArgs): Promise<void> {
    const dutyMag = Math.abs(args.deployDuty) || 20000
    const g = args.guard
    openMotorCalibration.set({
        guid: args.guid, portIdx: args.portIdx, label: args.label,
        duty: dutyMag,
        timeoutS: Math.max(1, Math.round((args.timeoutMs || 30000) / 1000)),
        motorVoltageMv: args.motorVoltageMv ?? 6000,
        // Seed from the strut's SAVED guard so the dialog opens on the last
        // calibration; fall back to the LiveRatio default for an un-tuned strut.
        guardMode: g?.mode ?? 'live',
        ratio:     g ? g.ratioX100 / 100 : 2.5,
        sample:    g?.sampleMs ?? 200,
        window:    g?.windowMs ?? 80,
        threshold: g?.thresholdMa ?? 1000,
        ceiling:   g?.ceilingMa ?? 0,
        move: null, calib: null,
        rowBusy: '', busy: true, error: '',
        deployDuty: args.deployDuty, retractDuty: args.retractDuty,
        timeoutMs: args.timeoutMs,
        onCommit: args.onCommit,
    })
    try {
        await applyGuard()            // push the LiveRatio default so seeks trip
        await refreshMotorStatus()
    } catch (e) {
        openMotorCalibration.update(s => s ? { ...s, error: String(e) } : s)
    }
    openMotorCalibration.update(s => s ? { ...s, busy: false } : s)
}

/** Close — brake the motor (safety) and clear the dialog. */
export async function closeMotorCalibration(): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s) return
    try { await GearMotorStop(s.guid, s.portIdx) } catch { /* best effort */ }
    openMotorCalibration.set(null)
}

// ─── Field setters ────────────────────────────────────────────────────

export function setMotorField<K extends keyof OpenMotorCalT>(key: K, val: OpenMotorCalT[K]): void {
    openMotorCalibration.update(s => s ? { ...s, [key]: val } : s)
}

// ─── Live status ──────────────────────────────────────────────────────

/** Poll the role once; the live readout reads from the shared motorStatus
 *  store (so the panel-behind shows the same values). */
export async function refreshMotorStatus(): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s) return
    await pollMotorStatus(s.guid, s.portIdx)
}

// ─── Drive ────────────────────────────────────────────────────────────

function tmoMs(s: OpenMotorCalT): number { return Math.round((s.timeoutS || 20) * 1000) }

/** Push the current stall-guard config to the role (live, no re-attach).
 *  Includes the rated-voltage duty cap so calibration sweeps drive the motor
 *  exactly like a real deploy will. */
export async function applyGuard(): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s) return
    const mv = Math.max(0, Math.round(s.motorVoltageMv))
    if (s.guardMode === 'live') {
        await GearMotorGuardLiveRatio(s.guid, s.portIdx,
            Math.round(s.ratio * 100), Math.round(s.sample), 150,
            Math.round(s.window), 0, Math.round(s.ceiling), mv)
    } else {
        await GearMotorGuardFixed(s.guid, s.portIdx, Math.round(s.threshold), Math.round(s.window), mv)
    }
}

/** Apply-guard wrapper that surfaces errors on the dialog + refreshes. */
export async function motorApplyGuard(): Promise<void> {
    openMotorCalibration.update(s => s ? { ...s, error: '' } : s)
    try { await applyGuard() }
    catch (e) { openMotorCalibration.update(s => s ? { ...s, error: String(e) } : s) }
    finally { await refreshMotorStatus() }
}

/** Two-leg calibration sweep (A → B); leg B travel = full stroke. */
export async function motorCalibrate(): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s || s.rowBusy) return
    openMotorCalibration.update(x => x ? { ...x, rowBusy: 'calibrating', error: '', move: null } : x)
    try {
        const calib = await GearMotorCalibrate(s.guid, s.portIdx, s.duty, tmoMs(s)) as CalibrationT
        openMotorCalibration.update(x => x ? { ...x, calib } : x)
    } catch (e) {
        openMotorCalibration.update(x => x ? { ...x, error: String(e) } : x)
    } finally {
        openMotorCalibration.update(x => x ? { ...x, rowBusy: '' } : x)
        await refreshMotorStatus()
    }
}

/** Drive to logical end "a" (+duty) or "b" (−duty), await the outcome. */
export async function motorMoveEnd(end: 'a' | 'b'): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s || s.rowBusy) return
    openMotorCalibration.update(x => x ? { ...x, rowBusy: 'moving', error: '', calib: null } : x)
    try {
        const move = { ...(await GearMotorMoveEnd(s.guid, s.portIdx, end, s.duty, tmoMs(s)) as EndstopResultT), end }
        openMotorCalibration.update(x => x ? { ...x, move } : x)
    } catch (e) {
        openMotorCalibration.update(x => x ? { ...x, error: String(e) } : x)
    } finally {
        openMotorCalibration.update(x => x ? { ...x, rowBusy: '' } : x)
        await refreshMotorStatus()
    }
}

/** Hard brake. */
export async function motorStop(): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s) return
    try { await GearMotorStop(s.guid, s.portIdx) }
    catch (e) { openMotorCalibration.update(x => x ? { ...x, error: String(e) } : x) }
    finally { await refreshMotorStatus() }
}

/** Hold-to-jog: raw signed duty (NO stall guard). `dir` = +1 / −1. */
let jogging = false
export async function motorJogStart(dir: number): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s || s.rowBusy) return
    jogging = true
    try { await GearMotorJog(s.guid, s.portIdx, dir * s.duty) }
    catch (e) { openMotorCalibration.update(x => x ? { ...x, error: String(e) } : x) }
}
export async function motorJogStop(): Promise<void> {
    // Guard: the jog buttons fire this on `mouseleave` too, so a stray cursor
    // pass would otherwise send setSigned(0) — which the firmware treats as an
    // explicit drive that ABORTS any in-flight seek (move-to-end / calibrate).
    // Only send the brake if a jog was actually started.
    if (!jogging) return
    jogging = false
    const s = get(openMotorCalibration)
    if (!s) return
    try { await GearMotorJog(s.guid, s.portIdx, 0) }
    catch { /* best effort */ }
    finally { await refreshMotorStatus() }
}

// ─── Commit ───────────────────────────────────────────────────────────

/** The travel-timeout suggested from the last calibration: full-stroke
 *  (leg B) travel × 1.5 safety margin, rounded to 100 ms.  0 if no sweep ran. */
export function suggestedTimeoutMs(s: OpenMotorCalT): number {
    const t = s.calib?.legB?.travelMs ?? 0
    if (t <= 0) return 0
    return Math.round((t * 1.5) / 100) * 100
}

/** Save to strut — deploy = +duty, retract = −duty, and (if a sweep ran) the
 *  suggested travel timeout.  Hands the values to the gear panel's onCommit
 *  (which mutates the draft → persisted on Apply), then closes. */
export async function saveMotorToStrut(): Promise<void> {
    const s = get(openMotorCalibration)
    if (!s || !s.onCommit) { await closeMotorCalibration(); return }
    const suggested = suggestedTimeoutMs(s)
    const { deployDuty, retractDuty } = commitDuties(s.deployDuty, s.duty)
    s.onCommit({
        deployDuty,
        retractDuty,
        timeoutMs:   suggested > 0 ? suggested : s.timeoutMs,
        motorVoltageMv: Math.max(0, Math.round(s.motorVoltageMv)),
        // Persist the tuned stall guard so real deploy/retract uses it (the
        // gear effect pushes it before each seek).  ratioX100 = ratio×100.
        guard: {
            mode:        s.guardMode,
            ratioX100:   Math.round(s.ratio * 100),
            sampleMs:    Math.round(s.sample),
            windowMs:    Math.round(s.window),
            thresholdMa: Math.round(s.threshold),
            ceilingMa:   Math.round(s.ceiling),
        },
    })
    await closeMotorCalibration()
}
