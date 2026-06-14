// wizard-ports.ts — port assignment for the wizard.
//
// Unlike the panels (which pick from ports that ALREADY have the role attached
// on the IO tab), the wizard picks from any compatible free port and attaches
// the required role on pick — so the operator never has to think about roles.

import type { Port, Claim, PortRef } from './devicemodel'
import { claimsForPort, attachRole } from './devicemodel'

/** PortRefT as stored in effect configs (board + string kind + idx). */
export interface PortRefT { board: string; guid: string; kind: string; idx: number }

const KIND_NUM: Record<string, number> = { servo: 1, pwm: 2, hbridge: 3, input: 4 }

/** Ports of `kind` that THIS effect can use: output, online, and either already
 *  carrying `role`, or able to (allowedRoles) and not claimed by another effect.
 *  The currently-picked ref stays in the pool (exempt). */
export function assignablePool(
    ports: readonly Port[],
    claims: readonly Claim[],
    kind: 'servo' | 'pwm' | 'hbridge',
    role: number,
    exempt?: PortRefT | null,
): Port[] {
    const exemptKey = exempt && exempt.kind ? `${exempt.guid}|${exempt.kind}|${exempt.idx}` : ''
    return ports.filter(p => {
        if (p.kindName !== kind) return false
        if (p.direction !== 'output') return false
        if (p.offline) return false
        const canRole = p.roleKind === role || (p.allowedRoles ?? []).some(r => r.kind === role)
        if (!canRole) return false
        const key = `${p.ref.guid}|${p.kindName}|${p.ref.index}`
        if (key === exemptKey) return true
        return claimsForPort(claims, p.ref).length === 0
    })
}

/** Build the effect PortRefT for a device-model port. */
export function portToRef(p: Port): PortRefT {
    return { board: '', guid: p.ref.guid, kind: p.kindName, idx: p.ref.index }
}
export function emptyRef(kind: string): PortRefT {
    return { board: '', guid: '', kind, idx: 0 }
}
export function refKey(r: PortRefT | null | undefined): string {
    return r && r.kind ? `${r.guid}|${r.kind}|${r.idx}` : ''
}
export function portKey(p: Port): string {
    return `${p.ref.guid}|${p.kindName}|${p.ref.index}`
}

/** Attach `role` to the port if it isn't already carrying it. */
export async function ensureRole(p: Port, role: number): Promise<void> {
    if (p.roleKind !== role) {
        try { await attachRole(p.ref as PortRef, role) } catch { /* surfaced via device model */ }
    }
}
