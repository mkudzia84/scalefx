// port_pool.ts — shared "free port pool" helper for every panel's
// output-port picker (Rule 49).
//
// Every dropdown that picks an output port (landing-light LEDs +
// servos, GunFx muzzle/recoil/smoke/turret, LightFx program channels,
// future expander effects) MUST filter by both:
//   1. the right ROLE attached to the port (e.g. `LedAnimator` for
//      LED channels, `ServoActuator` for servos, `Heater` for
//      smoke heaters, `DcMotor` for fans)
//   2. UNCLAIMED state — the port isn't already grabbed by some
//      other effect / sibling group.  An exemption list lets the
//      operator's currently-selected port stay in the picker for
//      editing.
//
// When the resulting pool is empty AND the field can't function
// without a pick (LEDs in a landing group, channels in a program),
// the picker MUST render a YELLOW WARNING — the operator's mental
// model is "I added a thing but it's broken because there's nothing
// to attach to", and a silent empty dropdown hides the cause.
// The warning is non-blocking (Rule 39 yellow vs Rule 35 red) — the
// operator can still save the YAML and fix it later.

import type { Port, Claim } from './../devicemodel'
import { claimsForPort } from './../devicemodel'

/** Build the pool for one picker call.
 *  @param ports        every port from $deviceModel.ports
 *  @param claims       every claim from $deviceModel.claims
 *  @param kindName     'pwm' | 'servo' | 'hbridge' | 'input'
 *  @param requiredRole the role kind byte (devicemodel.RoleKind.*)
 *  @param exempt       port refs already picked by THIS row — keep them
 *                       in the pool so the picker shows the current value */
export function freePortPool(
    ports:        readonly Port[],
    claims:       readonly Claim[],
    kindName:     'pwm' | 'servo' | 'hbridge' | 'input',
    requiredRole: number,
    exempt:       readonly { guid: string; kind?: string; idx: number }[] = [],
): Port[] {
    const exemptKeys = new Set(exempt
        .filter(r => r && r.kind)
        .map(r => `${r.guid}|${r.kind}|${r.idx}`))
    return ports.filter(p => {
        if (p.kindName !== kindName) return false
        if (p.direction !== 'output') return false
        if (p.roleKind !== requiredRole) return false
        // Exempt → keep regardless of claims.
        const key = `${p.ref.guid}|${p.kindName}|${p.ref.index}`
        if (exemptKeys.has(key)) return true
        // Unclaimed → keep.  Anything claimed by anyone is hidden.
        return claimsForPort(claims, p.ref).length === 0
    })
}

/** Same as freePortPool, but the `exempt` callback is more general —
 *  used by panels that compute exemption via "every port currently in
 *  THIS group's list" (LandingPanel) or "the port this row is bound to
 *  PLUS every port the SIBLING ROWS are bound to" (more complex). */
export function freePortPoolFiltered(
    ports:        readonly Port[],
    claims:       readonly Claim[],
    kindName:     'pwm' | 'servo' | 'hbridge' | 'input',
    requiredRole: number,
    isExempt:     (p: Port) => boolean,
): Port[] {
    return ports.filter(p => {
        if (p.kindName !== kindName) return false
        if (p.direction !== 'output') return false
        if (p.roleKind !== requiredRole) return false
        if (isExempt(p)) return true
        return claimsForPort(claims, p.ref).length === 0
    })
}

