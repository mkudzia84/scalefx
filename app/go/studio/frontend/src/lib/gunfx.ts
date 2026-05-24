// gunfx.ts — Studio store + Wails wrapper for the GunFX panel.
//
// Same shape as effects.ts's engineDraft / engineConfig / engineDirty
// triad: `gunfxConfig` mirrors what firmware loaded, `gunfxDraft` is what
// the operator is editing, `gunfxDirty` is the derived diff flag.  The
// dirty-aware loader pattern (snapshot `wasDirty` before mutating) is
// the same one the Engine panel learned the hard way.
//
// All Wails bindings are stubbed in app_gunfx.go (Phase 1).  Phase 2+3
// firmware actually parses /gunfx.yaml and routes the wire commands;
// Phase 4 just wires up the UI.

import { writable, derived, get } from 'svelte/store'
import type { DirtySource } from './dirty-registry'
import {
    LoadGunFxConfig, SaveGunFxConfig, GunFxStatus,
    GunFire, GunFireWithRof,
    GunStartFiring, GunStartFiringWithRof,
    GunStopFiring, GunSmokeArm,
    GunManualSet, GunManualRelease, GunVerboseSubscribe,
} from '../../wailsjs/go/main/App'
import { EventsOn, EventsOff } from '../../wailsjs/runtime/runtime'

// ─── DTO mirrors (must match app_gunfx.go DTO names) ──────────────────

export interface PortRefT { board: string; guid: string; kind: string; idx: number }

export interface RofItemT {
    name: string
    bandLoUs: number
    bandHiUs: number
    rpm: number
    soundPath: string
    /** Stereo routing mask: bit 0 = left, bit 1 = right.
     *  0x03 = both (default), 0x01 = left only, 0x02 = right only. */
    outputMask: number
}

// Rule 43 — channel-input references are NAMES from the IO tab's
// `inputs:` block; firmware resolves to PortRef + channel at apply.
export interface RofConfigT {
    input: string
    items: RofItemT[]
}

export interface TriggerConfigT {
    input: string
    thresholdUs: number
    hysteresisUs: number
}

export interface MuzzleFlashT {
    port: PortRefT
    durationMs: number
    brightness: number
}

// Recoil — turret BEHAVIOUR, no dedicated servo (Phase 4 polish 2026-05-23).
// On each shot the chosen `axis` (pitch or yaw) kicks by `jerkUs` from
// its commanded position, holds for `holdMs`, then returns.  Servo
// slew shape lives on the axis's ServoActuatorRole (Rule 42).
export interface RecoilConfigT {
    enabled: boolean
    axis: 'pitch' | 'yaw'
    jerkUs: number
    holdMs: number
}

// Rule 44 — mirrors ServoMotionProfileDTO; same field shape as the
// live-tune ServoProfileDTO in app_roles.go so one ServoProfileEditor
// binds to both the inline-with-feature profile (persisted in
// /gunfx.yaml) and the live-tune wire commands.
export interface ServoMotionProfileT {
    minUs: number
    maxUs: number
    centerUs: number
    reversed: boolean
    maxSpeedUsPerSec: number
    maxAccelUsPerSec2: number
    maxJerkUsPerSec3: number
}

export interface HeaterT {
    port: PortRefT
    elementMv: number
    mode: 'always_on' | 'bang_bang' | 'closed_loop' | string
    targetCx10: number
    hystCx10: number
    scaling: 'passthrough' | 'linear' | 'quadratic' | string
    constantDutyPct: number
}

export interface FanT {
    port: PortRefT
    elementMv: number
    mode: 'off' | 'continuous' | 'puff_per_shot' | 'puff_on_fire_active' | string
    puffMs: number
    scaling: 'passthrough' | 'linear' | 'quadratic' | string
    constantDutyPct: number
}

export interface SmokeConfigT { heater: HeaterT; fan: FanT }

export interface GunAxisT {
    enabled: boolean
    servoPort: PortRefT
    input: string                  // Rule 43 — named-channel reference
    neutralUs: number
    // Servo motion profile lives on the port-role row in /hubfx.yaml
    // (Rule 42 storage); the GunFx panel reads it via
    // `$deviceModel.ports[i].profile` and writes via `SetPortProfile`
    // (Rule 44 editing surface).  Not in /gunfx.yaml.
}

export interface GunT {
    id: number
    name: string
    trigger: TriggerConfigT
    rof: RofConfigT
    muzzleFlash: MuzzleFlashT
    recoil: RecoilConfigT
    smoke: SmokeConfigT
    yaw: GunAxisT
    pitch: GunAxisT
}

export interface GunFxConfigT {
    schemaVersion: number
    enabled: boolean
    guns: GunT[]
}

// Verbose-status event payload (firmware emits this at ~10 Hz per
// subscribed gun; we re-emit as `gun:verbose:<id>` from app_gunfx.go).
export interface GunVerboseStatusT {
    id: number
    mode: number             // 0 rc, 1 manual
    firing: boolean
    smokeArmed: boolean
    smokeFanRunning: boolean
    heaterDutyPct: number
    heaterTempCx10: number
    yawCurrentUs: number
    yawTargetUs: number
    pitchCurrentUs: number
    pitchTargetUs: number
    rofIndex: number
    rofSelectorUs: number
    triggerUs: number
    shotsThisSession: number
}

export interface GunStatusT { id: number; firing: boolean; smokeArmed: boolean }

// ─── Defaults ─────────────────────────────────────────────────────────

const emptyPort: PortRefT = { board: '', guid: '', kind: '', idx: 0 }

const defaultAxis = (): GunAxisT => ({
    enabled: false,
    servoPort: { ...emptyPort, kind: 'servo' },
    input: '',          // Rule 43 — operator picks a named channel
    neutralUs: 1500,
})

export function defaultGun(id: number): GunT {
    return {
        id, name: '',
        trigger: { input: '', thresholdUs: 1500, hysteresisUs: 25 },
        // Seed one default ROF item so a freshly-added gun is fireable
        // immediately (operator can rename / re-band / pick a sound).
        // Empty band ([0, 0]) means "armed regardless of selector
        // channel" — a sensible default when no selector is bound.
        rof: { input: '', items: [
            { name: 'rof1', bandLoUs: 0, bandHiUs: 0, rpm: 600, soundPath: '', outputMask: 0x03 },
        ] },
        // Empty ports default to {guid:"", kind:"", idx:0} so the
        // firmware skips driving them.  The operator picks a port in
        // the panel; until then NOTHING gets flashed / heated / etc.
        muzzleFlash: { port: { ...emptyPort }, durationMs: 30, brightness: 100 },
        recoil: { enabled: true, axis: 'pitch', jerkUs: 200, holdMs: 80 },
        smoke: {
            heater: { port: { ...emptyPort }, elementMv: 0, mode: 'always_on', targetCx10: 1500, hystCx10: 50, scaling: 'linear', constantDutyPct: 100 },
            fan:    { port: { ...emptyPort }, elementMv: 0, mode: 'off',       puffMs: 200,                            scaling: 'linear', constantDutyPct: 100 },
        },
        yaw: defaultAxis(),
        pitch: defaultAxis(),
    }
}

export const emptyGunFxConfig = (): GunFxConfigT => ({ schemaVersion: 1, enabled: false, guns: [] })

// ─── Stores ───────────────────────────────────────────────────────────

/** What firmware loaded (last successful LoadGunFxConfig). */
export const gunfxConfig = writable<GunFxConfigT>(emptyGunFxConfig())
/** What the operator is editing. */
export const gunfxDraft  = writable<GunFxConfigT>(emptyGunFxConfig())

/** True when draft diverges from config. */
export const gunfxDirty = derived(
    [gunfxConfig, gunfxDraft],
    ([cfg, draft]) => JSON.stringify(cfg) !== JSON.stringify(draft),
)

// ─── Validation ───────────────────────────────────────────────────────

/** Returns the indices of ROF items whose [bandLoUs, bandHiUs] window
 *  overlaps with at least one sibling item.  Bands must NOT overlap
 *  (Rule 38) — the operator picks an item by driving the selector
 *  channel into a band; overlapping bands would arm two items at once.
 *  Studio surfaces both panel-level red borders AND a global Apply
 *  block (via `gunfxHasErrors` below). */
export function detectBandOverlaps(items: RofItemT[]): number[] {
    const out: number[] = []
    for (let i = 0; i < items.length; i++) {
        const a = items[i]
        for (let j = i + 1; j < items.length; j++) {
            const b = items[j]
            const al = a.bandLoUs || 1000, ah = a.bandHiUs || 2000
            const bl = b.bandLoUs || 1000, bh = b.bandHiUs || 2000
            if (al <= bh && bl <= ah) {
                if (!out.includes(i)) out.push(i)
                if (!out.includes(j)) out.push(j)
            }
        }
    }
    return out
}

/** True for any gun in the draft that has zero ROF items — an
 *  unfireable gun.  Only relevant when the effect is enabled
 *  (disabled effect = nothing fires, empty ROF is harmless). */
export function gunHasNoRof(g: GunT): boolean {
    return g.rof.items.length === 0
}

/** Aggregate validation state — true if the effect is enabled AND
 *  any gun has either (a) zero ROF items (unfireable) or (b) bands
 *  that overlap with another item in the same gun.  Feeds the
 *  dirty-registry so global Apply (Rule 35 + 45) is shaded out
 *  until the operator resolves the issue.  Pure derivation — no
 *  async, no side effects. */
export const gunfxHasErrors = derived(gunfxDraft, ($draft) => {
    if (!$draft || !$draft.enabled) return false
    return $draft.guns.some(g =>
        gunHasNoRof(g) ||
        (g.rof.items.length > 1 && detectBandOverlaps(g.rof.items).length > 0))
})

/** Pre-built `DirtySource` for the dirty-registry — register from
 *  App.svelte startup so the global Apply order is deterministic.
 *  Rule 46 (modular config sources): each domain module owns its full
 *  source descriptor; the panel is a pure view. */
export const gunfxConfigSource: DirtySource = {
    id:        'gunfx',
    label:     'GunFX',
    isDirty:   gunfxDirty,
    hasErrors: gunfxHasErrors,
    apply:     saveGunFxConfig,
    refresh:   loadGunFxConfig,
}

/** Live verbose-status mirror, keyed by gun id.  Populated by the
 *  `gun:verbose` Wails event subscription installed below. */
export const gunfxVerbose = writable<Record<number, GunVerboseStatusT>>({})

/** Light status (firing + smoke) per gun, populated by polling
 *  GunFxStatus on demand.  Studio Phase 4 calls refreshGunFxStatus()
 *  on Apply or on a status-tab open. */
export const gunfxStatus = writable<GunStatusT[]>([])

// ─── Loaders / savers ─────────────────────────────────────────────────

/** Download /gunfx.yaml, populate `gunfxConfig`, and (if the draft
 *  wasn't already dirty) seed `gunfxDraft` with the loaded values. */
export async function loadGunFxConfig(): Promise<void> {
    const n = (await LoadGunFxConfig()) as GunFxConfigT
    const wasDirty = get(gunfxDirty)   // SNAPSHOT FIRST (engine-panel lesson)
    gunfxConfig.set(n)
    if (!wasDirty) gunfxDraft.set(structuredClone(n))
}

/** Save the current draft to /gunfx.yaml + ask the hub to reload it. */
export async function saveGunFxConfig(): Promise<void> {
    const cfg = get(gunfxDraft)
    await SaveGunFxConfig(cfg)
    gunfxConfig.set(structuredClone(cfg))   // optimistic — server reload is fire-and-confirm
}

export async function refreshGunFxStatus(): Promise<void> {
    const st = (await GunFxStatus()) as GunStatusT[] | null
    gunfxStatus.set(st ?? [])
}

// ─── Runtime control wrappers ─────────────────────────────────────────

export async function gunFire(id: number): Promise<void> { return GunFire(id) }
/** Single shot with explicit ROF index — 0xFF (=255) means "use the
 *  firmware's currently-armed ROF" (same as `gunFire`).  Used by the
 *  Studio Fire button when the operator picks a program from the
 *  dropdown so manual tests always have a concrete program even when
 *  the RC selector stick is between bands (2026-05-24). */
export async function gunFireWithRof(id: number, rofIndex: number): Promise<void> {
    return GunFireWithRof(id, rofIndex)
}
export async function gunStartFiring(id: number, rpm: number): Promise<void> { return GunStartFiring(id, rpm) }
/** Auto-fire burst with explicit ROF index — counterpart to gunFireWithRof. */
export async function gunStartFiringWithRof(id: number, rpm: number, rofIndex: number): Promise<void> {
    return GunStartFiringWithRof(id, rpm, rofIndex)
}
export async function gunStopFiring(id: number): Promise<void> { return GunStopFiring(id) }
export async function gunSmokeArm(id: number, armed: boolean): Promise<void> { return GunSmokeArm(id, armed ? 1 : 0) }

// Manual override — bitmask flags + per-field values.  Match
// gunfx.ManualFlag* constants (kept in sync with gunfx_protocol.h).
export const ManualFlag = {
    Yaw:      0x01,
    Pitch:    0x02,
    Rof:      0x04,
    Fire:     0x08,
    Smoke:    0x10,
    FanBurst: 0x20,
} as const

export interface GunManualStateT {
    flags: number
    yawUs: number
    pitchUs: number
    rofIndex: number
    fireHold: number
    smokeArm: number
    smokeFanBurst: number
}

export async function gunManualSet(id: number, state: GunManualStateT): Promise<void> {
    return GunManualSet(id, state as any)
}
export async function gunManualRelease(id: number): Promise<void> { return GunManualRelease(id) }
export async function gunVerboseSubscribe(id: number, enable: boolean): Promise<void> {
    return GunVerboseSubscribe(id, enable ? 1 : 0)
}

// ─── Verbose-status event plumbing ────────────────────────────────────

let verboseRegistered = false
let cancelVerbose: (() => void) | null = null

/** Install the Wails event listener for `gun:verbose`.  Idempotent —
 *  the GunFxPanel mounts it once; multiple mounts share one listener. */
export function installVerboseListener(): void {
    if (verboseRegistered) return
    verboseRegistered = true
    cancelVerbose = EventsOn('gun:verbose', (ev: GunVerboseStatusT) => {
        gunfxVerbose.update(m => ({ ...m, [ev.id]: ev }))
    })
}

/** Tear it down — call from a top-level onDestroy if needed. */
export function uninstallVerboseListener(): void {
    if (cancelVerbose) cancelVerbose()
    EventsOff('gun:verbose')
    verboseRegistered = false
    cancelVerbose = null
}

// ─── Mutators (draft only) ────────────────────────────────────────────
// These all operate on the DRAFT store; saving pushes it to firmware.

export function addGun(): void {
    gunfxDraft.update(c => {
        const used = new Set(c.guns.map(g => g.id))
        let id = 0
        while (used.has(id)) id++
        return { ...c, guns: [...c.guns, defaultGun(id)] }
    })
}

export function removeGun(id: number): void {
    gunfxDraft.update(c => ({ ...c, guns: c.guns.filter(g => g.id !== id) }))
}

export function setEnabled(on: boolean): void {
    gunfxDraft.update(c => {
        if (!on) return { ...c, enabled: false }
        // Auto-seed on enable:
        //   • zero guns → drop in a default gun #0
        //   • any gun with zero ROF items → seed one ROF (a gun
        //     without a ROF item is unfireable and flagged as an
        //     error by gunfxHasErrors — the operator shouldn't have
        //     to manually fix that on the click that enables the
        //     effect).
        const guns = c.guns.length === 0
            ? [defaultGun(0)]
            : c.guns.map(g => g.rof.items.length === 0
                ? { ...g, rof: { ...g.rof, items: [
                    { name: 'rof1', bandLoUs: 0, bandHiUs: 0, rpm: 600, soundPath: '', outputMask: 0x03 },
                  ] } }
                : g)
        return { ...c, enabled: true, guns }
    })
}

export function updateGun(id: number, mutate: (g: GunT) => GunT): void {
    gunfxDraft.update(c => ({
        ...c,
        guns: c.guns.map(g => g.id === id ? mutate(g) : g),
    }))
}

/** Suggest a [loUs, hiUs] band for the NEXT ROF item that does not
 *  overlap any existing bands.  Algorithm:
 *    1. If there are no items, return the full RC range (1000–2000 µs).
 *    2. Build a sorted list of explicit (non-unbounded) bands; merge
 *       any unbounded item ([0,0] / [0,n] / [n,0]) into a full-cover
 *       sentinel so the gap search treats it as "everything".
 *    3. Scan for the largest empty gap in [1000, 2000] between sorted
 *       band edges; if its width ≥ 100 µs, hand back that gap.
 *    4. If no usable gap exists (bar is fully covered or only
 *       sub-100µs slivers remain), slice the largest existing band in
 *       half and return its upper half — caller's mutator handles the
 *       trim of the original.  Falls back to a small slice at the top
 *       (1800–2000) if everything else is degenerate.
 *  Operator-visible: a fresh item never collides with a sibling, and
 *  every new item lands SOMEWHERE on the bar so the overlay shows it
 *  immediately (the [0,0] default used to make every new item span
 *  the entire bar and stack invisibly under siblings). */
export function suggestNextRofBand(items: RofItemT[]): { lo: number; hi: number } {
    if (items.length === 0) return { lo: 1000, hi: 2000 }
    // Explicit bands only — unbounded sides effectively cover everything,
    // so any unbounded item already occupies the full bar and we should
    // slice it instead of hunting for a gap.
    const explicit = items
        .filter(i => i.bandLoUs > 0 && i.bandHiUs > 0 && i.bandHiUs > i.bandLoUs)
        .map(i => ({ lo: i.bandLoUs, hi: i.bandHiUs }))
        .sort((a, b) => a.lo - b.lo)
    const unboundedCount = items.length - explicit.length

    if (unboundedCount === 0 && explicit.length > 0) {
        // Look for a gap between existing bands or at the edges.
        let cursor = 1000
        let bestGap = { lo: 0, hi: 0, width: 0 }
        for (const b of explicit) {
            if (b.lo > cursor) {
                const w = b.lo - cursor
                if (w > bestGap.width) bestGap = { lo: cursor, hi: b.lo, width: w }
            }
            cursor = Math.max(cursor, b.hi)
        }
        if (2000 > cursor) {
            const w = 2000 - cursor
            if (w > bestGap.width) bestGap = { lo: cursor, hi: 2000, width: w }
        }
        if (bestGap.width >= 100) {
            // Round to 10µs steps for tidy operator-facing numbers.
            const lo = Math.round(bestGap.lo / 10) * 10
            const hi = Math.round(bestGap.hi / 10) * 10
            return { lo, hi }
        }
    }
    // No gap (or only unbounded siblings) — slice the largest band in
    // half.  Caller's mutator emits both halves so the operator can
    // immediately see and tune them.
    const widest = explicit.reduce<{ lo: number; hi: number } | null>(
        (acc, b) => (acc == null || (b.hi - b.lo) > (acc.hi - acc.lo)) ? b : acc, null)
    if (widest && (widest.hi - widest.lo) >= 100) {
        const mid = Math.round((widest.lo + widest.hi) / 2 / 10) * 10
        return { lo: mid, hi: widest.hi }
    }
    // Pathological fallback — drop a 200µs slice at the top of the range.
    return { lo: 1800, hi: 2000 }
}

export function addRofItem(gunId: number): void {
    updateGun(gunId, g => {
        const { lo, hi } = suggestNextRofBand(g.rof.items)
        // Trim the widest existing band when we're slicing — the
        // suggester picked the UPPER half, so the existing widest item
        // needs its `hi` shrunk to `lo`.  Only fires when the suggester
        // returned a band that's INSIDE an existing one (cap by overlap
        // detection below to keep this simple).
        const items = g.rof.items.map(it => {
            if (it.bandLoUs > 0 && it.bandHiUs > 0
                    && it.bandLoUs <= lo && it.bandHiUs >= hi
                    && it.bandLoUs !== lo) {
                return { ...it, bandHiUs: lo }
            }
            return it
        })
        return {
            ...g,
            rof: {
                ...g.rof,
                items: [...items, {
                    name: `rof${g.rof.items.length + 1}`,
                    bandLoUs: lo, bandHiUs: hi,
                    rpm: 600, soundPath: '', outputMask: 0x03,
                }],
            },
        }
    })
}

export function removeRofItem(gunId: number, index: number): void {
    updateGun(gunId, g => ({
        ...g,
        rof: { ...g.rof, items: g.rof.items.filter((_, i) => i !== index) },
    }))
}
