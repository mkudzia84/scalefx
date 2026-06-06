import { describe, it, expect } from 'vitest'
import { freePortPool, freePortPoolFiltered } from './port_pool'
import type { Port, Claim } from '../devicemodel'
import { RoleKind } from '../devicemodel'

// Minimal Port factory — freePortPool only reads ref/kindName/direction/roleKind.
function mkPort(guid: string, kindName: string, kind: number, idx: number, roleKind: number): Port {
    return {
        ref: { guid, kind, index: idx },
        kindName,
        direction: 'output',
        roleKind,
    } as unknown as Port
}
const servo = (guid: string, idx: number, role = RoleKind.ServoActuator) =>
    mkPort(guid, 'servo', 1, idx, role)

describe('freePortPool', () => {
    it('includes a HUB-LOCAL servo (canonical empty guid) with the right role', () => {
        // The GUID redesign made hub-local the empty guid; the pool must still
        // surface it (the regression that emptied the dropdowns).
        const ports = [servo('', 0), servo('', 1)]
        const pool = freePortPool(ports, [], 'servo', RoleKind.ServoActuator)
        expect(pool.map(p => p.ref.index)).toEqual([0, 1])
    })

    it('filters out the wrong role kind', () => {
        const ports = [servo('', 0, RoleKind.ServoActuator), servo('', 1, RoleKind.LedAnimator)]
        const pool = freePortPool(ports, [], 'servo', RoleKind.ServoActuator)
        expect(pool.map(p => p.ref.index)).toEqual([0])
    })

    it('filters out the wrong kindName', () => {
        const ports = [servo('', 0), mkPort('', 'pwm', 2, 0, RoleKind.ServoActuator)]
        const pool = freePortPool(ports, [], 'servo', RoleKind.ServoActuator)
        expect(pool).toHaveLength(1)
        expect(pool[0].kindName).toBe('servo')
    })

    it('hides a claimed port but keeps an exempt (currently-selected) one', () => {
        const ports = [servo('', 0), servo('', 1)]
        const claims: Claim[] = [
            { domain: 'gun', slot: 'yaw', port: { guid: '', kind: 1, index: 0 } },
        ]
        // Without exemption: servo 0 is claimed → only servo 1 free.
        expect(freePortPool(ports, claims, 'servo', RoleKind.ServoActuator).map(p => p.ref.index))
            .toEqual([1])
        // With servo 0 exempt (this row owns it): both show.
        const exempt = [{ guid: '', kind: 'servo', idx: 0 }]
        expect(freePortPool(ports, claims, 'servo', RoleKind.ServoActuator, exempt).map(p => p.ref.index))
            .toEqual([0, 1])
    })

    it('exempt keying works for the empty (hub) guid', () => {
        // The exempt key is `${guid}|${kind}|${idx}` → `|servo|0` for a hub port.
        // A non-empty-guid bug here would drop the operator's current pick.
        const ports = [servo('', 0)]
        const claims: Claim[] = [
            { domain: 'gun', slot: 'yaw', port: { guid: '', kind: 1, index: 0 } },
        ]
        const pool = freePortPool(ports, claims, 'servo', RoleKind.ServoActuator,
            [{ guid: '', kind: 'servo', idx: 0 }])
        expect(pool).toHaveLength(1)
    })
})

describe('freePortPoolFiltered', () => {
    it('keeps ports the isExempt callback approves regardless of claims', () => {
        const ports = [servo('', 0), servo('', 1)]
        const claims: Claim[] = [
            { domain: 'gun', slot: 'yaw', port: { guid: '', kind: 1, index: 0 } },
        ]
        const pool = freePortPoolFiltered(ports, claims, 'servo', RoleKind.ServoActuator,
            p => p.ref.index === 0)
        expect(pool.map(p => p.ref.index)).toEqual([0, 1])
    })
})
