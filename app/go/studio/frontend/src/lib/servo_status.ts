// Generic, port-keyed live servo telemetry (Rule 42). The firmware broadcasts a
// batched SERVO_MOTION_UPDATE (all active servos) while subscribed via
// SetServoLiveView; the Go side re-emits it as `servo:motion`. ANY panel that
// uses a servo consumes this store — it's decoupled from the effect domain that
// owns the servo. Upload-safe: the firmware gates the emit on hostVerboseActive
// and skips it entirely during an upload (no client-side poller).

import { writable } from 'svelte/store'
import { EventsOn } from '../../wailsjs/runtime/runtime'
import { SetServoLiveView } from '../../wailsjs/go/main/App'

export interface ServoLive {
    posUs: number       // live position (motion-profile output, incl. recoil)
    targetUs: number    // commanded target the profile is slewing to
    velUsPerS: number
    ts: number          // wall-clock ms of last update (staleness check)
}

// Keyed by `${guid}|servo|${portIdx}` — same composite key shape the device
// model + profileForPort use, so a consumer keys off its servo PortRef directly.
export const servoStatus = writable<Record<string, ServoLive>>({})

/** Composite key for a servo PortRef (guid + index; kind is always 'servo'). */
export function servoStatusKey(guid: string, idx: number): string {
    return `${guid}|servo|${idx}`
}

let cancel: (() => void) | null = null

/** Idempotent — install the `servo:motion` listener once. */
export function installServoStatusListener(): void {
    if (cancel) return
    cancel = EventsOn('servo:motion', (ev: {
        guid: string
        servos: Array<{ portIdx: number; posUs: number; targetUs: number; velUsPerS: number }>
    }) => {
        if (!ev || !ev.servos) return
        const now = Date.now()
        servoStatus.update(m => {
            const next = { ...m }
            for (const s of ev.servos) {
                next[servoStatusKey(ev.guid, s.portIdx)] = {
                    posUs: s.posUs, targetUs: s.targetUs, velUsPerS: s.velUsPerS, ts: now,
                }
            }
            return next
        })
    })
}

export function uninstallServoStatusListener(): void {
    if (cancel) { cancel(); cancel = null }
}

/** Subscribe-on-view: enable/disable the wire telemetry while a panel that
 *  shows servo status is mounted. */
export function setServoLiveView(on: boolean): Promise<void> {
    return SetServoLiveView(on)
}
