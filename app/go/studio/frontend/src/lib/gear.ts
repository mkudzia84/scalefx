// gear.ts — Studio store + Wails wrapper for the Gear / Undercarriage
// panel (instructions/29 §3, firmware landed 2026-06-07).
//
// Same triad shape as gunfx.ts / landing.ts (Rule 46):
//   gearConfig — last loaded/applied state (truth from device)
//   gearDraft  — what the operator is editing
//   gearDirty  — derived: draft differs from config
//   gearConfigSource — DirtySource exposed to the global ConfigToolbar
//
// Plus the runtime live-state stream — Wails event `gear:phase`
// (emitted by app_gear.go::installGearStream) lands in `gearPhases`
// keyed by channel id so the per-channel pill (Retracted · doors-opening
// / Deploying · motor-running / …) updates without polling.  A slow
// status poll self-heals dropped async events (Rule 53 lossy queue).

import { writable, derived, get, type Readable } from 'svelte/store'
import type { DirtySource } from './dirty-registry'
import {
    LoadGearConfig, SaveGearConfig, GearStatus,
    GearDeploy, GearRetract, GearStop, GearAll, GearReset,
    GearEStop, GearEStopAll, GearStep,
    GearDoors, GearMoveStrut, GearDoorsAll, GearMoveStrutAll,
} from '../../wailsjs/go/main/App'
import { EventsOn, EventsOff } from '../../wailsjs/runtime/runtime'
import { deviceModel, RoleKind, type Port } from './devicemodel'

// ─── DTO mirrors (must match app_gear.go DTO names) ───────────────────

export interface PortRefT { board: string; guid: string; kind: string; idx: number }

/** One door servo on a gear.  Open drives the servo to its calibrated MAX
 *  end, close to its calibrated MIN end (the per-door REV flag in the servo
 *  calibration flips the physical direction) — same parametrisation as a
 *  landing-light servo: travel lives in the servo calibration, not here. */
export interface GearDoorT {
    port: PortRefT
}

export type DoorMode    = 'sync' | 'delay' | 'sequence'
export type ClosePolicy = 'both' | 'first' | 'none'
export type CoordMode   = 'independent' | 'door_sync' | 'full_sync' | 'sequenced'

/** Persisted stall-guard calibration for a strut's motor (Studio's "Save to
 *  strut").  Mirrors the firmware GearDef guard + BIMOTOR_SET_GUARD; the gear
 *  effect pushes it to the motor before each seek so a saved calibration
 *  drives real deploy/retract.  ratioX100 = ratio×100 (250 = 2.5×). */
export interface GearGuardT {
    mode: 'live' | 'fixed'
    ratioX100: number
    sampleMs: number
    windowMs: number
    thresholdMa: number
    ceilingMa: number
}

export interface GearChannelT {
    id: number
    name: string
    motor: PortRefT
    deployDuty: number
    retractDuty: number
    timeoutMs: number
    /** Motor rated voltage — firmware caps |duty| at maxDuty × this / rail_mV
     *  (live per-motor vSense, else declared rail).  0 = cap off. */
    motorVoltageMv: number
    guard: GearGuardT
    doors: GearDoorT[]
    doorMode: DoorMode
    doorDelayMs: number
    closePolicy: ClosePolicy
}

/** OPTIONAL one-channel RC up/down binding (Rule 43 named channel from
 *  /hubfx.yaml inputs[]).  Above the threshold (switch ON) = deploy (gear down);
 *  invert flips.  Empty name = manual-only.  Failsafe is firmware-fixed
 *  to the DEPLOY side (gear down on RC loss). */
export interface GearInputT {
    name: string
    thresholdUs: number
    hysteresisUs: number
    invert: boolean
}

/** OPTIONAL transit sounds — the matching WAV loops on the dedicated
 *  Gear mixer channel while any gear moves in that direction. */
export interface GearSoundsT {
    deploy: string
    retract: string
    outputMask: number     // 1=L 2=R 3=both (speaker_routing.ts masks)
}

export interface GearConfigT {
    schemaVersion: number
    enabled: boolean
    coord: CoordMode
    /** item 6: emergency-deploy the gear when an input link drops. */
    deployOnConnectionLoss: boolean
    input: GearInputT
    sounds: GearSoundsT
    gears: GearChannelT[]
}

/** Live per-channel state from the `gear:phase` event / status poll. */
export interface GearPhaseT {
    id: number
    phase: number          // 0..7 — unconfigured/up/movingDown/down/movingUp/error/unknown/held
    phaseName: string
    subPhase: number       // 0..6 — idle / doors-opening / doors-open / strut-moving / strut-done / doors-closing / doors-closed
    subPhaseName: string
    errReason?: number     // GearError code behind an Error phase (0 otherwise)
    errReasonTag?: string  // short tag — "timeout" / "no motor" / "fault"
    doorsOpen?: boolean    // every door at its open end — the manual-control gate
    strutState?: number    // 0..3 — unknown/up/out/moving (GearStrutState)
    strutName?: string     // "up" / "out" / "moving" / "unknown"
}

export interface GearStatusEntryT {
    id: number
    phase: number
    phaseName: string
    subPhase: number
    subPhaseName: string
    errReason?: number
    errReasonTag?: string
    doorsOpen?: boolean
    strutState?: number
    strutName?: string
}

// GearStrutState wire values (mirror gearcontrol_protocol.h GearStrutState).
export const StrutUnknown = 0
export const StrutUp      = 1
export const StrutOut     = 2
export const StrutMoving  = 3

// ─── Defaults ─────────────────────────────────────────────────────────

const emptyPort = (kind: string): PortRefT => ({ board: '', guid: '', kind, idx: 0 })

export function defaultGearDoor(): GearDoorT {
    return { port: emptyPort('servo') }
}

/** Default stall guard = the firmware/role LiveRatio default (2.5×). */
export function defaultGearGuard(): GearGuardT {
    return { mode: 'live', ratioX100: 250, sampleMs: 200, windowMs: 80, thresholdMa: 1000, ceilingMa: 0 }
}

export function defaultGearChannel(id: number): GearChannelT {
    return {
        id,
        name: `gear${id}`,
        motor: emptyPort('hbridge'),
        deployDuty: 20000,
        retractDuty: -20000,
        timeoutMs: 30000,
        motorVoltageMv: 6000,
        guard: defaultGearGuard(),
        doors: [],
        doorMode: 'sync',
        doorDelayMs: 500,
        closePolicy: 'both',
    }
}

/** The canonical undercarriage layout — two mains + one nose/tail leg.  Seeded
 *  when the effect is first enabled (and by the empty-state button) so a fresh
 *  gear effect comes up as the typical 3-strut retract set instead of a single
 *  generic strut.  Names match how a modeller thinks about the legs; ports are
 *  still unassigned (operator picks an H-bridge per leg on the IO tab). */
export const DEFAULT_STRUT_NAMES = ['Main Left', 'Main Right', 'Front/Back'] as const

export function defaultGearStruts(): GearChannelT[] {
    return DEFAULT_STRUT_NAMES.map((name, i) => ({ ...defaultGearChannel(i), name }))
}

export const defaultGearInput = (): GearInputT =>
    ({ name: '', thresholdUs: 1500, hysteresisUs: 50, invert: false })

export const defaultGearSounds = (): GearSoundsT =>
    ({ deploy: '', retract: '', outputMask: 3 })

export const emptyGearConfig = (): GearConfigT => ({
    schemaVersion: 2, enabled: false, coord: 'independent',
    deployOnConnectionLoss: false,
    input: defaultGearInput(), sounds: defaultGearSounds(), gears: [],
})

// ─── Stores ───────────────────────────────────────────────────────────

/** What firmware loaded (last successful LoadGearConfig). */
export const gearConfig = writable<GearConfigT>(emptyGearConfig())
/** What the operator is editing. */
export const gearDraft  = writable<GearConfigT>(emptyGearConfig())

/** True when draft diverges from config. */
export const gearDirty = derived(
    [gearConfig, gearDraft],
    ([cfg, draft]) => JSON.stringify(cfg) !== JSON.stringify(draft),
)

/** Per-channel id → last phase seen, fed by the `gear:phase` event
 *  stream + seeded on mount via `refreshGearStatus()`. */
export const gearPhases = writable<Record<number, GearPhaseT>>({})

// ─── Validation (Rule 35 red) ─────────────────────────────────────────
//
// PURE helpers take the device-model ports explicitly so they're unit-
// testable without a live store (the derived `gearHasErrors` wires them
// to `$deviceModel`).  A channel has errors when:
//   - the motor port doesn't resolve to a BiDcMotor role in the model;
//   - a door servo doesn't resolve to a ServoActuator role;
//   - door_mode === 'delay' but doorDelayMs <= 0;
//   - close_policy === 'first' but fewer than 2 doors (nothing to keep open).
//
// Cross-effect port CONFLICTS are NOT validated here (matching GunFx): the
// picker POOL is fed $effectClaims (merged hard + draft claims) in the panel,
// so a port another effect uses can't be picked.  Keeping validation role-only
// also keeps `gearHasErrors` free of an effect-claims → gear circular import.

/** Does port `ref` resolve to a port in `ports` with `requiredRole` attached? */
function portResolvesTo(
    ports: readonly Port[],
    ref: PortRefT,
    requiredRole: number,
    kindName: 'servo' | 'hbridge',
): boolean {
    if (!ref || !ref.kind) return false
    const p = ports.find(p =>
        p.ref.guid === ref.guid && p.kindName === kindName && p.ref.index === ref.idx)
    if (!p) return false
    return p.roleKind === requiredRole
}

/** Returns the human-readable validation problems for gear channel
 *  `idx`.  `ports` come from the device model.  Cross-effect port conflicts
 *  are guarded by the picker POOL ($effectClaims), not here (see header). */
export function gearItemErrors(
    gears: GearChannelT[],
    idx: number,
    ports: readonly Port[],
): string[] {
    const g = gears[idx]
    const out: string[] = []
    if (!g) return out

    // Motor — required, must be a BiDcMotor.
    if (!g.motor || !g.motor.kind) {
        out.push('No gear motor assigned — the channel can\'t deploy.')
    } else if (!portResolvesTo(ports, g.motor, RoleKind.BiDcMotor, 'hbridge')) {
        out.push('Gear motor port has no BiDcMotor role attached.')
    }

    // Doors — each must resolve to a ServoActuator.
    g.doors.forEach((d, di) => {
        if (!d.port || !d.port.kind) {
            out.push(`Door ${di + 1}: no servo port picked.`)
        } else if (!portResolvesTo(ports, d.port, RoleKind.ServoActuator, 'servo')) {
            out.push(`Door ${di + 1}: servo port has no ServoActuator role attached.`)
        }
    })

    // Door mode / close policy consistency.
    if (g.doorMode === 'delay' && (!g.doorDelayMs || g.doorDelayMs <= 0)) {
        out.push('Door mode = "delay" but the delay is 0 ms — set a positive delay.')
    }
    if (g.closePolicy === 'first' && g.doors.length < 2) {
        out.push('Close policy = "one closes" needs 2 doors — only one is configured.')
    }
    return out
}

/** Effect-level (non-channel) validation problems: the optional sound
 *  paths must be absolute on-device paths when set. */
export function gearEffectErrors(cfg: GearConfigT): string[] {
    const out: string[] = []
    const checkPath = (label: string, p: string) => {
        if (p && !p.startsWith('/'))
            out.push(`${label} sound path must be absolute (start with /).`)
    }
    checkPath('Deploy',  cfg.sounds?.deploy ?? '')
    checkPath('Retract', cfg.sounds?.retract ?? '')
    return out
}

/** Aggregate validation — true when the effect is enabled AND any
 *  channel has at least one error.  Feeds the dirty-registry so the
 *  global Apply is shaded out until resolved (Rule 35 + 45).  Subscribes
 *  to BOTH the draft AND the shared device model (cross-file check). */
export const gearHasErrors: Readable<boolean> = derived(
    [gearDraft, deviceModel],
    ([$draft, $dm]) => {
        if (!$draft || !$draft.enabled) return false
        if (gearEffectErrors($draft).length > 0) return true
        return $draft.gears.some((_, i) =>
            gearItemErrors($draft.gears, i, $dm.ports).length > 0)
    },
)

// ─── DirtySource (Rule 46) ────────────────────────────────────────────

export const gearConfigSource: DirtySource = {
    id:        'gear',
    label:     'Gear / Undercarriage',
    isDirty:   gearDirty,
    hasErrors: gearHasErrors,
    apply:     saveGearConfig,
    refresh:   loadGearConfig,
}

// ─── Loaders / savers ─────────────────────────────────────────────────

/** Download /gearcontrol.yaml, populate `gearConfig`, and (if the draft
 *  wasn't already dirty) seed `gearDraft` with the loaded values. */
export async function loadGearConfig(): Promise<void> {
    const raw = (await LoadGearConfig()) as GearConfigT
    const n = normaliseGearConfig(raw)
    const wasDirty = get(gearDirty)   // SNAPSHOT FIRST (engine-panel lesson)
    gearConfig.set(n)
    if (!wasDirty) gearDraft.set(structuredClone(n))
}

/** Hydrate fields that fresh / hand-edited YAMLs may omit so the UI
 *  doesn't read undefined where it expects a sensible default. */
function normaliseGearConfig(c: GearConfigT | null): GearConfigT {
    if (!c) return emptyGearConfig()
    const out: GearConfigT = structuredClone(c)
    out.schemaVersion = out.schemaVersion ?? 2
    out.enabled       = !!out.enabled
    out.coord         = (out.coord ?? 'independent') as CoordMode
    out.deployOnConnectionLoss = !!out.deployOnConnectionLoss
    out.input = {
        name:         out.input?.name ?? '',
        thresholdUs:  out.input?.thresholdUs || 1500,
        hysteresisUs: out.input?.hysteresisUs || 50,
        // Direction/reverse is set ON THE RADIO, not here — the gear channel is
        // never inverted (switch ON/above threshold = lower (gear down), OFF =
        // raise (gear up); RC-loss lowers).
        invert:       false,
    }
    out.sounds = {
        deploy:     out.sounds?.deploy ?? '',
        retract:    out.sounds?.retract ?? '',
        outputMask: (out.sounds?.outputMask && out.sounds.outputMask <= 3)
                    ? out.sounds.outputMask : 3,
    }
    out.gears         = (out.gears ?? []).map(g => ({
        id:          g.id ?? 0,
        name:        g.name ?? '',
        motor:       g.motor ?? emptyPort('hbridge'),
        deployDuty:  g.deployDuty ?? 20000,
        retractDuty: g.retractDuty ?? -20000,
        timeoutMs:   g.timeoutMs ?? 30000,
        // Absent key (pre-cap file / backend omitempty) = firmware default
        // 6000 mV; explicit 0 = cap off and survives the round-trip.
        motorVoltageMv: g.motorVoltageMv ?? 6000,
        guard:       g.guard ? {
            mode:        g.guard.mode === 'fixed' ? 'fixed' : 'live',
            ratioX100:   g.guard.ratioX100 ?? 250,
            sampleMs:    g.guard.sampleMs ?? 200,
            windowMs:    g.guard.windowMs ?? 80,
            thresholdMa: g.guard.thresholdMa ?? 1000,
            ceilingMa:   g.guard.ceilingMa ?? 0,
        } : defaultGearGuard(),
        doors:       Array.isArray(g.doors) ? g.doors.map(d => ({
            port:  d.port ?? emptyPort('servo'),
        })) : [],
        doorMode:    (g.doorMode ?? 'sync') as DoorMode,
        doorDelayMs: g.doorDelayMs ?? 500,
        closePolicy: (g.closePolicy ?? 'both') as ClosePolicy,
    }))
    return out
}

/** Save the current draft to /gearcontrol.yaml + ask the hub to reload. */
export async function saveGearConfig(): Promise<void> {
    const cfg = get(gearDraft)
    await SaveGearConfig(cfg as any)
    gearConfig.set(structuredClone(cfg))   // optimistic — server reload confirms
}

export async function refreshGearStatus(): Promise<void> {
    const st = (await GearStatus()) as GearStatusEntryT[] | null
    if (!st) return
    gearPhases.update(m => {
        const next = { ...m }
        for (const s of st) {
            next[s.id] = {
                id: s.id, phase: s.phase, phaseName: s.phaseName,
                subPhase: s.subPhase, subPhaseName: s.subPhaseName,
                errReason: s.errReason, errReasonTag: s.errReasonTag,
                doorsOpen: s.doorsOpen, strutState: s.strutState, strutName: s.strutName,
            }
        }
        return next
    })
}

// ─── Runtime control wrappers ─────────────────────────────────────────

export async function gearDeploy(id: number): Promise<void>  { return GearDeploy(id) }
export async function gearRetract(id: number): Promise<void> { return GearRetract(id) }
export async function gearStop(id: number): Promise<void>    { return GearStop(id) }
export async function gearReset(id: number): Promise<void>   { return GearReset(id) }
/** Emergency hold (brake + freeze in place → HELD). */
export async function gearEStop(id: number): Promise<void>   { return GearEStop(id) }
export async function gearEStopAll(): Promise<void>          { return GearEStopAll() }
/** Fleet trigger: 0 = stop/hold, 1 = deploy (down), 2 = retract (up). */
export async function gearAll(action: number): Promise<void> { return GearAll(action) }
/** Single-step a strut ONE leg toward target (0=up, 1=down) then park. */
export async function gearStep(id: number, target: number): Promise<void> { return GearStep(id, target) }

// ── Manual / maintenance: doors + strut independently ─────────────────
// The firmware enforces the interlock (close-doors needs strut up; move-strut
// needs doors open) and NACKs a violation — the panel also gates the buttons.
export async function gearDoors(id: number, open: boolean): Promise<void>  { return GearDoors(id, open) }
export async function gearMoveStrut(id: number, down: boolean): Promise<void> { return GearMoveStrut(id, down) }
export async function gearDoorsAll(open: boolean): Promise<void>           { return GearDoorsAll(open) }
export async function gearMoveStrutAll(down: boolean): Promise<void>       { return GearMoveStrutAll(down) }

export const GearAllStop = 0
export const GearAllDeploy = 1
export const GearAllRetract = 2

export const GearStepUp = 0
export const GearStepDown = 1

// ─── Phase-event listener (idempotent) ────────────────────────────────

let phaseRegistered = false
let cancelPhase: (() => void) | null = null
export function installGearPhaseListener(): void {
    if (phaseRegistered) return
    phaseRegistered = true
    cancelPhase = EventsOn('gear:phase', (ev: GearPhaseT) => {
        gearPhases.update(m => ({ ...m, [ev.id]: ev }))
    })
}
export function uninstallGearPhaseListener(): void {
    if (cancelPhase) cancelPhase()
    EventsOff('gear:phase')
    phaseRegistered = false
    cancelPhase = null
}

// ─── Mutators (draft only — saving pushes to firmware) ────────────────

export function addGearChannel(): void {
    gearDraft.update(c => {
        const used = new Set(c.gears.map(g => g.id))
        let id = 0
        while (used.has(id) && id < 255) id++
        // Suggest the next canonical leg name (Main Left / Main Right /
        // Front-Back) while any remain unused; fall back to a generic name.
        const taken = new Set(c.gears.map(g => g.name))
        const name = DEFAULT_STRUT_NAMES.find(n => !taken.has(n)) ?? `gear${id}`
        return { ...c, gears: [...c.gears, { ...defaultGearChannel(id), name }] }
    })
}

/** Replace the (empty) strut list with the canonical 3-strut undercarriage —
 *  the empty-state "add default struts" action. */
export function seedDefaultGearStruts(): void {
    gearDraft.update(c => c.gears.length === 0 ? { ...c, gears: defaultGearStruts() } : c)
}

export function removeGearChannel(id: number): void {
    gearDraft.update(c => ({ ...c, gears: c.gears.filter(g => g.id !== id) }))
}

/** Move a channel up/down the list.  ORDER IS FUNCTIONAL: the firmware's
 *  `sequenced` coordination runs the full cycle channel-by-channel in
 *  array order (gear[0] first on deploy), so the card order IS the
 *  sequence order. */
export function moveGearChannel(id: number, dir: -1 | 1): void {
    gearDraft.update(c => {
        const i = c.gears.findIndex(g => g.id === id)
        const j = i + dir
        if (i < 0 || j < 0 || j >= c.gears.length) return c
        const gears = [...c.gears]
        ;[gears[i], gears[j]] = [gears[j], gears[i]]
        return { ...c, gears }
    })
}

export function setGearEnabled(on: boolean): void {
    gearDraft.update(c => {
        if (!on) return { ...c, enabled: false }
        // Auto-seed the canonical 3-strut undercarriage on enable (Main Left /
        // Main Right / Front-Back) so a freshly-enabled effect comes up as the
        // typical retract set instead of a single generic strut.
        const gears = c.gears.length === 0 ? defaultGearStruts() : c.gears
        return { ...c, enabled: true, gears }
    })
}

export function setGearCoord(coord: CoordMode): void {
    gearDraft.update(c => ({ ...c, coord }))
}

/** item 6: toggle emergency-deploy-on-input-connection-loss. */
export function setGearDeployOnConnLoss(on: boolean): void {
    gearDraft.update(c => ({ ...c, deployOnConnectionLoss: on }))
}

export function updateGearInput(mutate: (i: GearInputT) => GearInputT): void {
    gearDraft.update(c => ({ ...c, input: mutate(c.input ?? defaultGearInput()) }))
}

export function updateGearSounds(mutate: (s: GearSoundsT) => GearSoundsT): void {
    gearDraft.update(c => ({ ...c, sounds: mutate(c.sounds ?? defaultGearSounds()) }))
}

export function updateGearChannel(id: number, mutate: (g: GearChannelT) => GearChannelT): void {
    gearDraft.update(c => ({
        ...c,
        gears: c.gears.map(g => g.id === id ? mutate(g) : g),
    }))
}

export function addGearDoor(id: number): void {
    updateGearChannel(id, g => g.doors.length >= 2 ? g : ({ ...g, doors: [...g.doors, defaultGearDoor()] }))
}

export function removeGearDoor(id: number, idx: number): void {
    updateGearChannel(id, g => ({ ...g, doors: g.doors.filter((_, i) => i !== idx) }))
}
