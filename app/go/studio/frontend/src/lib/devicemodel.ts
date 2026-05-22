// ScaleFX Studio — device-model client layer.
//
// Mirrors the Go `scalefx/devicemodel` DTOs and wraps the Wails bindings
// that expose them.  The whole model (ports, attached roles, domain
// claims, validation issues, domain/role/preset catalogs) lives in the Go
// backend; this module is the typed conduit + a reactive store the tabs
// subscribe to.  No model logic lives here — claim/validate/preset all
// round-trip to Go so there is exactly one source of truth.

import { writable, derived } from 'svelte/store'
import {
    RefreshDeviceModel, DeviceModelSnapshot,
    ClaimPort, UnclaimPort, CandidatePorts,
    AttachRole, DetachRole, ApplyPreset, SetPortName,
    SetInputProtocol, SetInputChannelCount, SetChannelFunction, ApplyDefaults,
} from '../../wailsjs/go/main/App'
import { EventsOn } from '../../wailsjs/runtime/runtime'

// ─── Wire enums (mirror protocol/ports + protocol/roles) ──────────────

export const PortKind = { Servo: 1, Pwm: 2, HBridge: 3, Input: 4 } as const

export const portKindName: Record<number, string> = {
    1: 'servo', 2: 'pwm', 3: 'hbridge', 4: 'input',
}

export const RoleKind = {
    None: 0x00, ServoActuator: 0x01, RcPwmInput: 0x02, SbusInput: 0x03,
    JetiExInput: 0x04, LedAnimator: 0x10, DcMotor: 0x11, Heater: 0x12,
    BiDcMotor: 0x20,
} as const

export const roleKindName: Record<number, string> = {
    0x00: 'none', 0x01: 'servo-actuator', 0x02: 'rc-pwm-input',
    0x03: 'sbus-input', 0x04: 'jeti-ex-input', 0x10: 'led-animator',
    0x11: 'dc-motor', 0x12: 'heater', 0x20: 'bi-dc-motor',
}

// ─── DTOs (mirror devicemodel/*.go json tags) ─────────────────────────

export interface PortRef { guid: string; kind: number; index: number }

export interface RoleOption { kind: number; label: string }

export interface Port {
    ref: PortRef
    boardName: string
    kindName: string
    direction: 'output' | 'input'
    flags: number
    caps: string[]
    roleKind: number
    roleName: string
    hardwareName: string
    allowedRoles: RoleOption[]
    name: string
}

export interface Claim { domain: string; slot: string; port: PortRef }

export interface Slot {
    key: string
    label: string
    roleKinds: number[]
    direction: 'output' | 'input'
    min: number
    max: number
    optional: boolean
    shared: boolean
}

export interface Domain { id: string; label: string; cap: number; slots: Slot[] }

export interface Issue {
    severity: 'error' | 'warn'
    domain?: string
    slot?: string
    port?: PortRef
    message: string
}

export interface RoleKindInfo { kind: number; name: string }

export interface Preset { name: string; description: string }

export interface ChannelMap { channel: number; function: string }
export interface InputPortConfig {
    port: PortRef
    protocol: string
    channelCount: number
    channels: ChannelMap[]
}
export interface ChannelFunctionDef { id: string; label: string; group: string }
export interface InputProtocolDef {
    id: string; label: string; roleKind: number; implemented: boolean; maxChannels: number
}

export interface DeviceModelSnapshotT {
    ports: Port[]
    claims: Claim[]
    domains: Domain[]
    issues: Issue[]
    roleCatalog: RoleKindInfo[]
    presets: Preset[]
    inputs: InputPortConfig[]
    channelFunctions: ChannelFunctionDef[]
    inputProtocols: InputProtocolDef[]
}

const empty: DeviceModelSnapshotT = {
    ports: [], claims: [], domains: [], issues: [], roleCatalog: [], presets: [],
    inputs: [], channelFunctions: [], inputProtocols: [],
}

/** normalize guarantees every array field is non-null.  Go marshals an
 *  empty nil slice as JSON `null`; iterating that (`for..of null`) throws
 *  and would break Svelte reactivity, so we coerce on every ingest. */
function normalize(snap: unknown): DeviceModelSnapshotT {
    const s = (snap ?? {}) as Partial<DeviceModelSnapshotT>
    return {
        ports: s.ports ?? [],
        claims: s.claims ?? [],
        domains: s.domains ?? [],
        issues: s.issues ?? [],
        roleCatalog: s.roleCatalog ?? [],
        presets: s.presets ?? [],
        inputs: s.inputs ?? [],
        channelFunctions: s.channelFunctions ?? [],
        inputProtocols: s.inputProtocols ?? [],
    }
}

// ─── Reactive store ───────────────────────────────────────────────────

export const deviceModel = writable<DeviceModelSnapshotT>(empty)

// ─── Tab model ────────────────────────────────────────────────────────
//
// The Studio tab strip is derived from the model: two fixed setup tabs
// (Ports & Roles, Inputs), then one tab per capability-available domain,
// then Firmware.  TabBar and MainLayout both read this so they never drift.

export type TabKind = 'io' | 'domain' | 'firmware'
export interface StudioTab { key: string; label: string; kind: TabKind; domain?: Domain }

export const studioTabs = derived(deviceModel, ($dm): StudioTab[] => {
    const tabs: StudioTab[] = [
        { key: 'io', label: 'Input & Ports', kind: 'io' },
    ]
    for (const d of $dm.domains ?? []) {
        tabs.push({ key: 'dom:' + d.id, label: d.label, kind: 'domain', domain: d })
    }
    tabs.push({ key: 'firmware', label: 'Firmware', kind: 'firmware' })
    return tabs
})

/** errorCount / warnCount summarize validation severity for the status bar. */
export const validationCounts = derived(deviceModel, ($dm) => {
    let errors = 0, warnings = 0
    for (const i of $dm.issues ?? []) { if (i.severity === 'error') errors++; else warnings++ }
    return { errors, warnings }
})

let bridged = false

/** installDeviceModelBridge subscribes to backend devicemodel:changed
 *  events once.  Call from App.svelte onMount. */
export function installDeviceModelBridge() {
    if (bridged) return
    bridged = true
    EventsOn('devicemodel:changed', (snap: DeviceModelSnapshotT) => {
        deviceModel.set(normalize(snap))
    })
}

/** refresh re-fetches the topology from the hub and rebuilds the model. */
export async function refresh(): Promise<void> {
    const snap = await RefreshDeviceModel()
    deviceModel.set(normalize(snap))
}

/** loadCatalogs pulls the cached snapshot (domain/role/preset catalogs are
 *  populated even before a topology is loaded). */
export async function loadCatalogs(): Promise<void> {
    const snap = await DeviceModelSnapshot()
    deviceModel.set(normalize(snap))
}

export function reset() {
    deviceModel.set(empty)
}

// ─── Mutations (round-trip to Go, store updated from the returned snap) ─

export async function claim(domain: string, slot: string, p: PortRef): Promise<void> {
    const snap = await ClaimPort(domain, slot, p.guid, p.kind, p.index)
    deviceModel.set(normalize(snap))
}

export async function unclaim(domain: string, slot: string, p: PortRef): Promise<void> {
    const snap = await UnclaimPort(domain, slot, p.guid, p.kind, p.index)
    deviceModel.set(normalize(snap))
}

export async function candidates(domain: string, slot: string): Promise<Port[]> {
    return (await CandidatePorts(domain, slot)) as Port[]
}

export async function attachRole(p: PortRef, roleKind: number): Promise<void> {
    const snap = await AttachRole(p.guid, p.kind, p.index, roleKind)
    deviceModel.set(normalize(snap))
}

export async function detachRole(p: PortRef): Promise<void> {
    const snap = await DetachRole(p.guid, p.kind, p.index)
    deviceModel.set(normalize(snap))
}

export async function setPortName(p: PortRef, name: string): Promise<void> {
    const snap = await SetPortName(p.guid, p.kind, p.index, name)
    deviceModel.set(normalize(snap))
}

export async function applyPreset(name: string): Promise<string[]> {
    const res = await ApplyPreset(name) as any
    // Wails returns multi-value (snapshot, warnings) as an array.
    if (Array.isArray(res)) {
        deviceModel.set(normalize(res[0]))
        return (res[1] as string[]) ?? []
    }
    deviceModel.set(normalize(res))
    return []
}

// ─── Input config ─────────────────────────────────────────────────────

export async function setInputProtocol(p: PortRef, protocol: string): Promise<void> {
    const snap = await SetInputProtocol(p.guid, p.kind, p.index, protocol)
    deviceModel.set(normalize(snap))
}

export async function setInputChannelCount(p: PortRef, count: number): Promise<void> {
    const snap = await SetInputChannelCount(p.guid, p.kind, p.index, count)
    deviceModel.set(normalize(snap))
}

export async function setChannelFunction(p: PortRef, channel: number, fn: string): Promise<void> {
    const snap = await SetChannelFunction(p.guid, p.kind, p.index, channel, fn)
    deviceModel.set(normalize(snap))
}

export async function applyDefaults(): Promise<string[]> {
    const res = await ApplyDefaults() as any
    if (Array.isArray(res)) {
        deviceModel.set(normalize(res[0]))
        return (res[1] as string[]) ?? []
    }
    deviceModel.set(normalize(res))
    return []
}

// ─── Live channel values ──────────────────────────────────────────────
//
// The hub streams RC frames as `input:values` events.  We keep the latest
// per (portRef → channel) so the bars can read O(1).  Keyed
// `${guid}|${portIdx}|${channel}`.

export interface LiveChannel { us: number; valid: boolean }
export const liveChannels = writable<Record<string, LiveChannel>>({})

let inputBridged = false
export function installInputValuesBridge() {
    if (inputBridged) return
    inputBridged = true
    EventsOn('input:values', (v: { guid: string; portIdx: number; channels: { channel: number; us: number; valid: boolean }[] }) => {
        if (!v || !v.channels) return
        liveChannels.update(cur => {
            const next = { ...cur }
            for (const c of v.channels) {
                next[`${v.guid}|${v.portIdx}|${c.channel}`] = { us: c.us, valid: c.valid }
            }
            return next
        })
    })
}

export function liveChannelKey(p: PortRef, channel: number): string {
    return `${p.guid}|${p.index}|${channel}`
}

/** usToPct maps a 1000–2000 µs RC pulse to 0–100% for the value bar. */
export function usToPct(us: number): number {
    const pct = ((us - 1000) / 1000) * 100
    return Math.max(0, Math.min(100, pct))
}

// ─── Helpers ──────────────────────────────────────────────────────────

export function portRefKey(p: PortRef): string {
    return `${p.guid}|${p.kind}|${p.index}`
}

export function boardLabel(guid: string): string {
    return guid === '' ? 'Hub' : guid
}

// ─── Board display names ──────────────────────────────────────────────
//
// GUIDs are an internal addressing detail; the UI shows friendly board
// names (kind + index).  When two expanders of the same kind are present
// they're indexed ("LightFX #1", "LightFX #2"); a lone board of a kind has
// no index.  Returns a guid → display-name map computed from the ports.

const KIND_LABEL: Record<string, string> = {
    hubfx: 'HubFX', lightfx: 'LightFX', gunfx: 'GunFX',
    gearcontrol: 'GearControl', noop: 'NoOp',
}

export function boardKindOf(name: string): string {
    const n = (name || '').toLowerCase()
    for (const k of Object.keys(KIND_LABEL)) {
        if (n.startsWith(k)) return k
    }
    return ''
}

export function boardDisplayNames(ports: Port[]): Record<string, string> {
    // Distinct boards in stable order (hub "" first, then by guid).
    const seen = new Map<string, string>() // guid → boardName
    for (const p of ports) {
        if (!seen.has(p.ref.guid)) seen.set(p.ref.guid, p.boardName)
    }
    const guids = [...seen.keys()].sort((a, b) => (a === '' ? -1 : b === '' ? 1 : a < b ? -1 : 1))
    // Count per kind to decide whether to index.
    const kindOf: Record<string, string> = {}
    const counts: Record<string, number> = {}
    for (const g of guids) {
        const k = boardKindOf(seen.get(g) || '')
        kindOf[g] = k
        counts[k] = (counts[k] || 0) + 1
    }
    const out: Record<string, string> = {}
    const idx: Record<string, number> = {}
    for (const g of guids) {
        const k = kindOf[g]
        const label = KIND_LABEL[k] || (seen.get(g) || boardLabel(g))
        if ((counts[k] || 0) > 1) {
            idx[k] = (idx[k] || 0) + 1
            out[g] = `${label} #${idx[k]}`
        } else {
            out[g] = label
        }
    }
    return out
}

/** issuesFor returns the validation issues scoped to a domain (and
 *  optionally a slot). */
export function issuesFor(issues: Issue[], domain: string, slot?: string): Issue[] {
    return issues.filter(i => i.domain === domain && (slot === undefined || i.slot === slot))
}

/** claimsForSlot returns the claims in a (domain, slot). */
export function claimsForSlot(claims: Claim[], domain: string, slot: string): Claim[] {
    return claims.filter(c => c.domain === domain && c.slot === slot)
}

/** domainsForPort lists the (domain, slot) labels a port is claimed by —
 *  used by the Inputs tab to show fan-out. */
export function claimsForPort(claims: Claim[], p: PortRef): Claim[] {
    const k = portRefKey(p)
    return claims.filter(c => portRefKey(c.port) === k)
}
