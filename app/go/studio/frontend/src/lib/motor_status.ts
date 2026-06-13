// motor_status.ts — live BiDcMotor (gear-motor / H-bridge) telemetry, keyed by
// PortRef (guid + hbridge index).  Unlike the servo stream (a firmware-pushed
// batched broadcast), there's no motor live-view broadcast — the panel POLLS
// GearMotorStatus per configured motor on a slow cadence, and the calibration
// dialog polls faster while open.  This is the store both feed.
//
// The headline value is `currentMa` — the gear panel surfaces it next to each
// strut so the operator sees stall current build during a deploy ("read out of
// the mA hooked into status").

import { writable, get } from 'svelte/store'
import { GearMotorStatus } from '../../wailsjs/go/main/App'

export interface MotorLive {
    signedDuty:  number
    voltageMv:   number
    currentMa:   number
    stalled:     boolean
    position:    number      // 0 unknown / 1 A / 2 B
    positionName: string
    guardName:   string
    ts:          number       // wall-clock ms of last update (staleness)
}

// Keyed `${guid}|hbridge|${idx}` — same composite shape the device model uses.
export const motorStatus = writable<Record<string, MotorLive>>({})

export function motorStatusKey(guid: string, idx: number): string {
    return `${guid}|hbridge|${idx}`
}

/** Poll one motor's status and merge it into the store.  Swallows transient
 *  wire errors (a poll racing a reconnect) — the next tick self-heals. */
export async function pollMotorStatus(guid: string, idx: number): Promise<void> {
    try {
        const st = await GearMotorStatus(guid, idx) as {
            signedDuty: number; voltageMv: number; currentMa: number
            stalled: boolean; position: number; positionName: string; guardName: string
        }
        motorStatus.update(m => ({
            ...m,
            [motorStatusKey(guid, idx)]: {
                signedDuty: st.signedDuty, voltageMv: st.voltageMv, currentMa: st.currentMa,
                stalled: st.stalled, position: st.position, positionName: st.positionName,
                guardName: st.guardName, ts: Date.now(),
            },
        }))
    } catch {
        /* transient — leave the prior sample (staleness shows via ts) */
    }
}

/** Poll a batch of motors (the gear panel's configured struts) sequentially —
 *  one wire round-trip each, so a 2–3 motor set stays well under the poll
 *  interval.  Refs with no kind/guid are skipped. */
export async function pollMotors(
    refs: ReadonlyArray<{ guid: string; idx: number; kind?: string }>,
): Promise<void> {
    for (const r of refs) {
        if (!r || (r.kind !== undefined && r.kind !== 'hbridge')) continue
        await pollMotorStatus(r.guid, r.idx)
    }
}

/** Read the cached live sample for a motor ref (or null). */
export function motorLiveFor(guid: string, idx: number): MotorLive | null {
    return get(motorStatus)[motorStatusKey(guid, idx)] ?? null
}
