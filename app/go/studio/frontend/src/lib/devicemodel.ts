// ScaleFX Studio — device-model client layer.
//
// Mirrors the Go `scalefx/devicemodel` DTOs and wraps the Wails bindings
// that expose them.  The whole model (ports, attached roles, domain
// claims, validation issues, domain/role/preset catalogs) lives in the Go
// backend; this module is the typed conduit + a reactive store the tabs
// subscribe to.  No model logic lives here — claim/validate/preset all
// round-trip to Go so there is exactly one source of truth.

import { writable, derived, get } from 'svelte/store'
import { connectionInfo } from './stores'
import {
    RefreshDeviceModel, DeviceModelSnapshot,
    ClaimPort, UnclaimPort, CandidatePorts,
    AttachRole, DetachRole, SetPortName,
    SetInputProtocol, SetInputChannelCount, SetChannelFunction, ApplyDefaults,
    LoadHubConfig, SaveHubConfig, RemoveExpanderConfig,
} from '../../wailsjs/go/main/App'
import { EventsOn } from '../../wailsjs/runtime/runtime'

// ─── Wire enums (mirror protocol/ports + protocol/roles) ──────────────

export const PortKind = { Servo: 1, Pwm: 2, HBridge: 3, Input: 4 } as const

export const portKindName: Record<number, string> = {
    1: 'servo', 2: 'pwm', 3: 'hbridge', 4: 'input',
}

export const RoleKind = {
    None: 0x00, ServoActuator: 0x01, RcPwmInput: 0x02, SbusInput: 0x03,
    JetiExInput: 0x04, JetiExTelemetry: 0x05, LedAnimator: 0x10, DcMotor: 0x11,
    Heater: 0x12, BiDcMotor: 0x20,
} as const

export const roleKindName: Record<number, string> = {
    0x00: 'none', 0x01: 'servo-actuator', 0x02: 'rc-pwm-input',
    0x03: 'sbus-input', 0x04: 'jeti-ex-input', 0x05: 'jeti-ex-telemetry',
    0x10: 'led-animator', 0x11: 'dc-motor', 0x12: 'heater', 0x20: 'bi-dc-motor',
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
    /** Rail voltage in millivolts (Phase 0 of GunFX rollout —
     *  instructions/22). 0 = unknown / unconstrained. Effects use this
     *  to compute voltage-scaled PWM duty for sub-rail elements. */
    voltageMv: number
    caps: string[]
    roleKind: number
    roleName: string
    hardwareName: string
    allowedRoles: RoleOption[]
    name: string
    /** Servo motion profile (Rule 42/44) — present only on servo ports
     *  that have an authored profile in /hubfx.yaml.  Mirrors the Go
     *  `devicemodel.ServoMotionProfile` json shape. Overlaid by the
     *  backend from the studio's portProfiles map. */
    profile?: {
        minUs: number; maxUs: number; centerUs: number; reversed: boolean
        maxSpeedUsPerSec: number; maxAccelUsPerSec2: number; maxJerkUsPerSec3: number
    }
    /** offline = a "ghost" port reconstructed from /hubfx.yaml for an
     *  expander board that's configured but NOT currently connected. The UI
     *  dims it, warns, and offers removal; its role can't be edited until the
     *  board reconnects. Live ports omit this (undefined ⇒ online). */
    offline?: boolean
}

/** formatPortRail renders a port's voltageMv as a human label for
 *  picker option text — "8 V", "3.3 V", or "" when unknown (0). */
export function formatPortRail(voltageMv: number): string {
    if (!voltageMv) return ''
    if (voltageMv % 1000 === 0) return `${voltageMv / 1000} V`
    return `${(voltageMv / 1000).toFixed(1)} V`
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
    inputs: InputPortConfig[]
    channelFunctions: ChannelFunctionDef[]
    inputProtocols: InputProtocolDef[]
}

const empty: DeviceModelSnapshotT = {
    ports: [], claims: [], domains: [], issues: [], roleCatalog: [],
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

export type TabKind = 'io' | 'engine' | 'gun' | 'lighting' | 'gear' | 'domain' | 'firmware' | 'gear-diagnostics'
export interface StudioTab { key: string; label: string; kind: TabKind; domain?: Domain }

// Engine + gun each get their own dedicated tab (EnginePanel / GunFxPanel)
// instead of a generic DomainTab — these provide config + runtime control
// + claiming.  Split out of the former combined "Effects" tab (2026-05-29)
// so GunFX gets full width for its two-column gun-core | smoke layout.
const SUPERSEDED_BY_EFFECTS  = new Set(['engine', 'gun'])
// The Lighting tab hosts both LED surfaces as sub-tabs (2026-06-05): the LightFX
// program editor AND the Landing-light groups (a landing group is a servo-
// deployed LED searchlight that attaches to a program via landing_bindings).
// Both `lighting` and `landing-lights` are hub-advertised → the tab always shows;
// neither gets its own generic domain tab.
const SUPERSEDED_BY_LIGHTING = new Set(['lighting', 'landing-lights'])
// Gear tab — the backend domain catalog names the undercarriage domain
// `landing-gear` (Domain.ID in app/go/devicemodel/types.go); `gearcontrol`
// is kept for compat.  BOTH route to the dedicated Gear tab (GearPanel) —
// without this set matching the real id, the domain fell through to the
// GENERIC DomainTab (the bare "Gear units"/"Gear switch" slot view) and the
// purpose-built panel was unreachable (found 2026-06-13).
const SUPERSEDED_BY_GEAR = new Set(['gearcontrol', 'landing-gear'])

export const studioTabs = derived([deviceModel, connectionInfo], ([$dm, $conn]): StudioTab[] => {
    const firmwareTab: StudioTab = { key: 'firmware', label: 'Firmware', kind: 'firmware' }
    // Tab gating by connected board: the full effect/config/IO surface is
    // HubFX-only (the master owns every effect + the port/role model). When a
    // NON-HubFX board (an expander) is connected on its own, it gets only its
    // own surfaces — a board-specific Diagnostics tab (low-level role attach /
    // drive / inspect) where one exists, plus Firmware (flash / version). The
    // Input & Ports tab and every effect tab stay HubFX-only.
    // Pre-connect (controllerType unset) keeps the full capability-gated set so
    // the operator can review surfaces before plugging in a hub.
    const ct = $conn.controllerType
    if ($conn.connected && ct && ct !== 'hubfx') {
        const tabs: StudioTab[] = []
        if (ct === 'gearcontrol') {
            tabs.push({ key: 'gear-diag', label: 'Diagnostics', kind: 'gear-diagnostics' })
        }
        tabs.push(firmwareTab)
        return tabs
    }
    // Order: Input & Ports → Effects → Lighting → other domain tabs →
    // Firmware.  Effects + Lighting both sit near the front so the
    // operator's most common edit surfaces (engine / gun config,
    // strobe / landing-light groups) are one click away.
    const tabs: StudioTab[] = [
        { key: 'io', label: 'Input & Ports', kind: 'io' },
    ]
    const domains = $dm.domains ?? []
    const showLighting = domains.some(d => SUPERSEDED_BY_LIGHTING.has(d.id))
    const gearDomain = domains.find(d => SUPERSEDED_BY_GEAR.has(d.id))
    const showGear = !!gearDomain
    if (domains.some(d => d.id === 'engine')) {
        tabs.push({ key: 'engine', label: 'EngineFX', kind: 'engine' })
    }
    if (domains.some(d => d.id === 'gun')) {
        tabs.push({ key: 'gun', label: 'GunFX', kind: 'gun' })
    }
    if (showLighting) {
        tabs.push({ key: 'lighting', label: 'Lighting', kind: 'lighting' })
    }
    if (showGear) {
        tabs.push({ key: 'gear', label: 'Gear', kind: 'gear', domain: gearDomain })
    }
    for (const d of domains) {
        if (SUPERSEDED_BY_EFFECTS.has(d.id) || SUPERSEDED_BY_LIGHTING.has(d.id)
            || SUPERSEDED_BY_GEAR.has(d.id)) continue
        tabs.push({ key: 'dom:' + d.id, label: d.label, kind: 'domain', domain: d })
    }
    tabs.push(firmwareTab)
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

/** hydrateFromHubYaml pulls /hubfx.yaml off the device and merges its
 *  inputs[]/ports[]/expanders[] into the studio overlay (channel
 *  functions, port names).  Called on connect; missing file is a no-op. */
export async function hydrateFromHubYaml(): Promise<void> {
    await LoadHubConfig()
    clearHubDirty()       // freshly loaded → nothing to apply
}

/** applyHubConfig writes /hubfx.yaml from the current overlay (channel
 *  functions + port names + role attachments) and tells the firmware to
 *  reload — changes take effect immediately on the hub. */
export async function applyHubConfig(): Promise<void> {
    await SaveHubConfig()
    clearHubDirty()       // just saved → in sync with the device
}

// ─── Rule 46 — modular config source ────────────────────────────────
//
// `hubConfigSource` exposes the IO/Ports tab dirty state to the global
// ConfigToolbar.  Signal-based: every mutator that persists into
// /hubfx.yaml (attachRole / detachRole / setPortName /
// setInputProtocol / setInputChannelCount / setChannelFunction +
// SetPortProfile called from the gun panel) raises the flag;
// applyHubConfig + hydrateFromHubYaml clear it.

import type { DirtySource } from './dirty-registry'

// ─── Signal-based dirty flag (2026-05-24) ────────────────────────────
//
// Replaces the previous fingerprint-compare approach.  Two broken
// iterations preceded this one:
//   (1) imperative `deviceModel.subscribe(snap => compareToBaseline)`
//       captured a closed-over baseline variable, raced with the
//       async `devicemodel:changed` event on connect (sometimes
//       baselined the PRE-hydrate model, sometimes the post-hydrate).
//   (2) derived store over `[deviceModel, _hubBaseline]` — sound on
//       paper, broke at runtime because `get` wasn't imported from
//       `svelte/store`, so rebaseline threw silently → baseline
//       stayed empty → derived always returned false → dirty NEVER
//       fired no matter what the operator did.
//
// The signal-based model is what the operator's mental model already
// is: any persisted edit RAISES the flag, save/load LOWERS it.  No
// snapshot diff, no closed-over state, no race.  Mutators that touch
// /hubfx.yaml call `markHubDirty()` after the device model is
// updated; `clearHubDirty()` runs from `applyHubConfig` (after a
// successful save) and `hydrateFromHubYaml` (after on-device state
// is mirrored back into the working model).  Mutators that DON'T
// persist (claim/unclaim — those are studio-overlay state today)
// don't touch the flag.  This keeps the contract explicit: if you
// added a new mutator that writes /hubfx.yaml, call markHubDirty.
const _hubDirty  = writable<boolean>(false)
const _hubErrors = derived(validationCounts, ($vc) => $vc.errors > 0)

/** Exported so panels that bypass the local mutators (e.g. GunFxPanel
 *  calling Wails `SetPortProfile` directly to update a servo profile)
 *  can raise the flag explicitly.  Direct-Wails-call sites without
 *  this would silently leave the IO config out-of-sync with the GUI. */
export function markHubDirty():  void { _hubDirty.set(true) }
function          clearHubDirty(): void { _hubDirty.set(false) }

export const hubConfigSource: DirtySource = {
    id:        'hubconfig',
    label:     'IO & Ports',
    isDirty:   _hubDirty,
    hasErrors: _hubErrors,
    apply:     applyHubConfig,
    refresh:   refresh,
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
    markHubDirty()    // role attachment persists into /hubfx.yaml ports[]
}

export async function detachRole(p: PortRef): Promise<void> {
    const snap = await DetachRole(p.guid, p.kind, p.index)
    deviceModel.set(normalize(snap))
    markHubDirty()    // role detach persists into /hubfx.yaml ports[]
}

export async function setPortName(p: PortRef, name: string): Promise<void> {
    const snap = await SetPortName(p.guid, p.kind, p.index, name)
    deviceModel.set(normalize(snap))
    markHubDirty()    // name persists into /hubfx.yaml ports[].label
}

/** removeAbandonedBoard drops a configured-but-disconnected expander's entry
 *  (ports/roles/names/profiles) from the in-memory config. The change is
 *  marked dirty so the global toolbar's Apply persists it to /hubfx.yaml. */
export async function removeAbandonedBoard(guid: string): Promise<void> {
    const snap = await RemoveExpanderConfig(guid)
    deviceModel.set(normalize(snap))
    markHubDirty()    // removal persists on next Apply (rewrites /hubfx.yaml)
}

// (Removed 2026-05-23) applyPreset → will be reintroduced as a Setup
// Wizard surface; the underlying devicemodel.Presets() catalog is still
// in the Go package, just no longer exposed through Wails.

// ─── Input config ─────────────────────────────────────────────────────

export async function setInputProtocol(p: PortRef, protocol: string): Promise<void> {
    const snap = await SetInputProtocol(p.guid, p.kind, p.index, protocol)
    deviceModel.set(normalize(snap))
    markHubDirty()    // protocol persists into /hubfx.yaml inputs[].protocol
}

export async function setInputChannelCount(p: PortRef, count: number): Promise<void> {
    const snap = await SetInputChannelCount(p.guid, p.kind, p.index, count)
    deviceModel.set(normalize(snap))
    markHubDirty()    // channelCount persists into /hubfx.yaml inputs[].channelCount
}

export async function setChannelFunction(p: PortRef, channel: number, fn: string): Promise<void> {
    const snap = await SetChannelFunction(p.guid, p.kind, p.index, channel, fn)
    deviceModel.set(normalize(snap))
    markHubDirty()    // channel function persists into /hubfx.yaml inputs[].channels[]
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
// Channel-count autodetect dedup: last count we auto-expanded a port to, so
// we push SetInputChannelCount only when the detected width actually changes
// (the stream fires at ~10 Hz — we must not command on every frame).
const lastAutoCount: Record<string, number> = {}
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
        autoExpandChannels(v)
    })
}

// Full auto-expand (operator decision 2026-05-29): grow the declared channel
// count to whatever the live stream actually carries, and let a row render
// per detected channel.  The radio emits a fixed N-slot frame — including the
// static padding channels (PPM/SBUS/Jeti all confirmed to pad to 16/24 on the
// bench), so the operator sees every slot and names only the ones they use
// (Rule 43).  Capped at the protocol ceiling; deduped per port.
function autoExpandChannels(v: { guid: string; portIdx: number; channels: { channel: number }[] }) {
    let detected = 0
    for (const c of v.channels) if (c.channel + 1 > detected) detected = c.channel + 1
    if (detected <= 0) return
    const pkey = `${v.guid}|${v.portIdx}`
    if (lastAutoCount[pkey] === detected) return
    lastAutoCount[pkey] = detected

    const dm = get(deviceModel)
    const cfg = dm.inputs.find(c => c.port.guid === v.guid && c.port.index === v.portIdx)
    if (!cfg) return
    const proto = dm.inputProtocols.find(p => p.id === cfg.protocol)
    const target = Math.min(detected, proto?.maxChannels ?? detected)
    if (cfg.channelCount === target) return
    // setInputChannelCount updates the model (rows appear), pushes to the hub,
    // and marks /hubfx.yaml dirty so the detected width can be persisted.
    setInputChannelCount(cfg.port, target).catch(() => {})
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

// Device-name prefixes that don't equal the kind string (the firmware uses
// "GearCtrl" but the kind is "gearcontrol").  Mirrors Go
// devicemodel.BoardKindFromName — keep the two in lock-step.
const KIND_ALIAS: Record<string, string> = { gearctrl: 'gearcontrol' }

export function boardKindOf(name: string): string {
    const n = (name || '').toLowerCase()
    for (const k of Object.keys(KIND_LABEL)) {
        if (n.startsWith(k)) return k
    }
    for (const [prefix, k] of Object.entries(KIND_ALIAS)) {
        if (n.startsWith(prefix)) return k
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
