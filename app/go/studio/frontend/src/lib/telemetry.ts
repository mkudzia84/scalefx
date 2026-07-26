// telemetry.ts — Studio store for the master's live telemetry collection
// (item 4).  There is no firmware broadcast for the collection, so the IO tab
// POLLS GetTelemetry on a slow timer while it's mounted; the panel reads this
// store.  Mirrors protocol/input.TelemetrySnapshot.

import { writable, derived } from 'svelte/store'
import { GetTelemetry } from '../../wailsjs/go/main/App'
import { EventsOn } from '../../wailsjs/runtime/runtime'

export interface TelemetrySensor {
    id: number
    type: number       // SensorKind (0=int, 1=gps, 2=datetime)
    decimals: number
    active: boolean
    value: number      // raw scaled value (divide by 10^decimals for display)
    label: string
    unit: string
}

export interface TelemetryDevice {
    usn: number
    lsn: number
    local: boolean
    active: boolean
    name: string
    sensors: TelemetrySensor[]
}

export interface TelemetrySnapshot {
    pubIntervalMs: number
    respHzX10: number
    activeSensors: number
    devices: TelemetryDevice[]
    ts: number          // wall-clock ms of last successful poll (staleness)
}

export const telemetry = writable<TelemetrySnapshot | null>(null)

/** Downstream (ESC) telemetry is live when the collection holds an ACTIVE
 *  non-local device — i.e. an ESC on IN_2 is replying.  Drives the IN_2
 *  jeti-ex-telemetry port's active / no-signal indicator. */
export const escTelemetryActive = derived(telemetry, $t =>
    !!$t && ($t.devices ?? []).some(d => !d.local && d.active))

// Shared, ref-counted poller so multiple views (the Telemetry sub-tab + the
// IN_2 port indicator) drive ONE 1 Hz GetTelemetry loop, not several.
let pollRefs = 0
let pollTimer: ReturnType<typeof setInterval> | undefined
export function startTelemetryPolling(): void {
    pollRefs++
    if (!pollTimer) {
        pollTelemetry()
        pollTimer = setInterval(() => pollTelemetry(), 1000)
    }
}
export function stopTelemetryPolling(): void {
    pollRefs = Math.max(0, pollRefs - 1)
    if (pollRefs === 0 && pollTimer) { clearInterval(pollTimer); pollTimer = undefined }
}

/** Poll the collection once, merging into the store. Swallows transient wire
 *  errors (a poll racing a reconnect) — the next tick self-heals. */
export async function pollTelemetry(): Promise<void> {
    try {
        const snap = (await GetTelemetry()) as Omit<TelemetrySnapshot, 'ts'>
        // Normalize on ingest: Go nil slices arrive as JSON null (an empty
        // TelemetryHub has Devices == nil).  `$t.devices.some(...)` on that
        // null threw inside the escTelemetryActive derived on every connect
        // with no ESC attached — the 2026-07-26 app-wide store freeze.
        const devices = (Array.isArray(snap?.devices) ? snap.devices : []).map(d => ({
            ...d,
            sensors: Array.isArray(d?.sensors) ? d.sensors : [],
        }))
        telemetry.set({ ...snap, devices, ts: Date.now() })
    } catch {
        /* transient — keep the last snapshot */
    }
}

/** Apply a sensor's implied decimal point for display (value × 10^-decimals). */
export function fmtSensorValue(s: TelemetrySensor): string {
    if (!s.decimals) return String(s.value)
    return (s.value / Math.pow(10, s.decimals)).toFixed(s.decimals)
}

// ─── Generic input connection-loss (item 5) ─────────────────────────

export interface ConnectionEvent {
    portKind: number
    portIdx: number
    guid: string        // "" = hub-local
    state: number       // 0 up, 1 lost, 2 down
    brownouts: number
}

/** Latest connection state per input source, keyed `guid|portKind|portIdx`.
 *  Fed by the firmware's async CONNECTION_EVENT (loss / recovery). */
export const linkStates = writable<Record<string, ConnectionEvent>>({})

export const LINK_STATE_NAMES = ['up', 'signal lost', 'DOWN'] as const

let connCancel: (() => void) | null = null
/** Idempotent — install the `input:connection` listener once. */
export function installConnectionListener(): void {
    if (connCancel) return
    connCancel = EventsOn('input:connection', (ev: ConnectionEvent) => {
        if (!ev) return
        const key = `${ev.guid}|${ev.portKind}|${ev.portIdx}`
        linkStates.update(m => ({ ...m, [key]: ev }))
    })
}
