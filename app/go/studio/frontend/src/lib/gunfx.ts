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
import {
    LoadGunFxConfig, SaveGunFxConfig, GunFxStatus,
    GunFire, GunStartFiring, GunStopFiring, GunSmokeArm,
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

// Recoil — gun-side params only (servo motion shape lives on the role).
export interface RecoilConfigT {
    port: PortRefT
    // profile field still in DTO for back-compat with the Phase 1 stub
    // but semantically only `centerUs` is read; jerk/hold tune the pulse.
    profile?: ServoMotionProfileT
    jerkUs: number
    holdMs: number
}

export interface ServoMotionProfileT {
    minUs: number
    maxUs: number
    centerUs: number
    inverted: boolean
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
    // Profile placeholder for Wails compat; motion shape lives on the
    // role layer per Rule 42 (servo motion profile is configured via
    // the IO tab's port-role row, NOT here).
    profile?: ServoMotionProfileT
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
const defaultProfile = (): ServoMotionProfileT => ({
    minUs: 1000, maxUs: 2000, centerUs: 1500, inverted: false,
    maxSpeedUsPerSec: 800, maxAccelUsPerSec2: 1600, maxJerkUsPerSec3: 0,
})

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
        rof: { input: '', items: [] },
        muzzleFlash: { port: { ...emptyPort, kind: 'pwm' }, durationMs: 30, brightness: 100 },
        recoil: { port: { ...emptyPort, kind: 'servo' }, profile: defaultProfile(), jerkUs: 200, holdMs: 80 },
        smoke: {
            heater: { port: { ...emptyPort, kind: 'pwm' }, elementMv: 0, mode: 'always_on', targetCx10: 1500, hystCx10: 50, scaling: 'linear', constantDutyPct: 100 },
            fan:    { port: { ...emptyPort, kind: 'pwm' }, elementMv: 0, mode: 'off',       puffMs: 200,                            scaling: 'linear', constantDutyPct: 100 },
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
export async function gunStartFiring(id: number, rpm: number): Promise<void> { return GunStartFiring(id, rpm) }
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
    gunfxDraft.update(c => ({ ...c, enabled: on }))
}

export function updateGun(id: number, mutate: (g: GunT) => GunT): void {
    gunfxDraft.update(c => ({
        ...c,
        guns: c.guns.map(g => g.id === id ? mutate(g) : g),
    }))
}

export function addRofItem(gunId: number): void {
    updateGun(gunId, g => ({
        ...g,
        rof: {
            ...g.rof,
            items: [...g.rof.items, { name: `rof${g.rof.items.length + 1}`, bandLoUs: 0, bandHiUs: 0, rpm: 600, soundPath: '' }],
        },
    }))
}

export function removeRofItem(gunId: number, index: number): void {
    updateGun(gunId, g => ({
        ...g,
        rof: { ...g.rof, items: g.rof.items.filter((_, i) => i !== index) },
    }))
}
