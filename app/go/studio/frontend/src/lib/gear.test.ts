import { describe, it, expect } from 'vitest'
import { gearItemErrors, defaultGearChannel, defaultGearDoor, type GearChannelT } from './gear'
import { RoleKind, type Port, type Claim } from './devicemodel'

// Minimal Port factory — gearItemErrors only reads ref/kindName/roleKind.
function mkPort(guid: string, kindName: string, kind: number, idx: number, roleKind: number): Port {
    return {
        ref: { guid, kind, index: idx },
        kindName,
        direction: 'output',
        roleKind,
    } as unknown as Port
}
const motorPort = (idx: number, role = RoleKind.BiDcMotor) => mkPort('', 'hbridge', 0x40, idx, role)
const servoPort = (idx: number, role = RoleKind.ServoActuator) => mkPort('', 'servo', 1, idx, role)

// A valid single-motor channel wired to hbridge idx 0 (BiDcMotor present).
function validChannel(): GearChannelT {
    const g = defaultGearChannel(0)
    g.motor = { board: '', guid: '', kind: 'hbridge', idx: 0 }
    return g
}

describe('gearItemErrors', () => {
    it('reports no errors for a valid motor-only channel', () => {
        const g = validChannel()
        const ports = [motorPort(0)]
        expect(gearItemErrors([g], 0, ports, [])).toEqual([])
    })

    it('errors when the motor port is unset', () => {
        const g = defaultGearChannel(0) // motor kind is 'hbridge' but… idx 0 with no model port
        g.motor = { board: '', guid: '', kind: '', idx: 0 } // truly unset
        const errs = gearItemErrors([g], 0, [], [])
        expect(errs.some(e => /No gear motor assigned/.test(e))).toBe(true)
    })

    it('errors when the motor port has no BiDcMotor role in the model', () => {
        const g = validChannel()
        // Port exists but is the wrong role (DcMotor, not BiDcMotor).
        const ports = [motorPort(0, RoleKind.DcMotor)]
        const errs = gearItemErrors([g], 0, ports, [])
        expect(errs.some(e => /no BiDcMotor role/.test(e))).toBe(true)
    })

    it('errors when the motor is claimed by another effect', () => {
        const g = validChannel()
        const ports = [motorPort(0)]
        const claims: Claim[] = [
            { domain: 'somethingelse', slot: 'x', port: { guid: '', kind: 0x40, index: 0 } },
        ]
        const errs = gearItemErrors([g], 0, ports, claims)
        expect(errs.some(e => /claimed by another effect/.test(e))).toBe(true)
    })

    it('errors when a door servo has no ServoActuator role', () => {
        const g = validChannel()
        g.doors = [{ port: { board: '', guid: '', kind: 'servo', idx: 0 }, open: 10000, close: 0 }]
        // servo port present but as LedAnimator → wrong role.
        const ports = [motorPort(0), servoPort(0, RoleKind.LedAnimator)]
        const errs = gearItemErrors([g], 0, ports, [])
        expect(errs.some(e => /no ServoActuator role/.test(e))).toBe(true)
    })

    it('accepts a door servo that resolves to ServoActuator', () => {
        const g = validChannel()
        g.doors = [{ port: { board: '', guid: '', kind: 'servo', idx: 0 }, open: 10000, close: 0 }]
        const ports = [motorPort(0), servoPort(0)]
        expect(gearItemErrors([g], 0, ports, [])).toEqual([])
    })

    it('errors for delay door mode with zero delay', () => {
        const g = validChannel()
        g.doors = [
            { port: { board: '', guid: '', kind: 'servo', idx: 0 }, open: 10000, close: 0 },
            { port: { board: '', guid: '', kind: 'servo', idx: 1 }, open: 10000, close: 0 },
        ]
        g.doorMode = 'delay'
        g.doorDelayMs = 0
        const ports = [motorPort(0), servoPort(0), servoPort(1)]
        const errs = gearItemErrors([g], 0, ports, [])
        expect(errs.some(e => /delay is 0 ms/.test(e))).toBe(true)
    })

    it('accepts delay door mode with a positive delay', () => {
        const g = validChannel()
        g.doors = [
            { port: { board: '', guid: '', kind: 'servo', idx: 0 }, open: 10000, close: 0 },
            { port: { board: '', guid: '', kind: 'servo', idx: 1 }, open: 10000, close: 0 },
        ]
        g.doorMode = 'delay'
        g.doorDelayMs = 500
        const ports = [motorPort(0), servoPort(0), servoPort(1)]
        expect(gearItemErrors([g], 0, ports, [])).toEqual([])
    })

    it('errors for close_policy=first with fewer than 2 doors', () => {
        const g = validChannel()
        g.doors = [{ port: { board: '', guid: '', kind: 'servo', idx: 0 }, open: 10000, close: 0 }]
        g.closePolicy = 'first'
        const ports = [motorPort(0), servoPort(0)]
        const errs = gearItemErrors([g], 0, ports, [])
        expect(errs.some(e => /needs 2 doors/.test(e))).toBe(true)
    })

    it('exempts the channel\'s OWN claim (gear domain claiming its own port)', () => {
        const g = validChannel()
        const ports = [motorPort(0)]
        // The gear domain itself claims the motor — that must NOT count as
        // "claimed elsewhere".
        const claims: Claim[] = [
            { domain: 'gear', slot: 'motor', port: { guid: '', kind: 0x40, index: 0 } },
        ]
        expect(gearItemErrors([g], 0, ports, claims)).toEqual([])
    })

    it('uses defaultGearDoor for sane open/close defaults', () => {
        const d = defaultGearDoor()
        expect(d.open).toBe(10000)
        expect(d.close).toBe(0)
        expect(d.port.kind).toBe('servo')
    })
})
