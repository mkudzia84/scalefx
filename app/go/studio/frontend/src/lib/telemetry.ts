// telemetry.ts — Studio store for the master's live telemetry collection
// (item 4).  There is no firmware broadcast for the collection, so the IO tab
// POLLS GetTelemetry on a slow timer while it's mounted; the panel reads this
// store.  Mirrors protocol/input.TelemetrySnapshot.

import { writable } from 'svelte/store'
import { GetTelemetry } from '../../wailsjs/go/main/App'
import { EventsOn } from '../../wailsjs/runtime/runtime'

export interface TelemetrySensor {
    id: number
    type: number       // Jeti ExDataType (Int6/14/22/30)
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

/** Poll the collection once, merging into the store. Swallows transient wire
 *  errors (a poll racing a reconnect) — the next tick self-heals. */
export async function pollTelemetry(): Promise<void> {
    try {
        const snap = (await GetTelemetry()) as Omit<TelemetrySnapshot, 'ts'>
        telemetry.set({ ...snap, ts: Date.now() })
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
