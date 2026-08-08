// effect-claims.ts — cross-effect port-claim synthesis.
//
// Feature effects (LightFx channels, GunFx muzzle / smoke / turret, Landing
// servos + LEDs, Gear motors + door servos) own ports by referencing them in
// their config.  There is no separate "hard claim" registry — the old
// ApplyPreset/Domains hard-claim path was retired (the device model no longer
// carries a `claims` array).  This file derives the claim list purely from each
// effect's DRAFT store and exports `effectClaims`: the unified Claim[] every
// port picker filters against.  Result: if GunFx 1's muzzle flash uses PWM 5,
// the LightFx LED picker no longer offers PWM 5.
//
// Auto-recomputes when any contributing draft changes, so an edit is visible in
// sibling pickers on the next render tick.
//
// Conventions:
//   - `domain` = the effect's name (`lightfx`, `gunfx`, `landing`, `gear`)
//   - `slot`   = a human-readable description of where in the effect config the
//                port is referenced (shown in tooltips when a sibling effect
//                tries to grab the port).
//
// A disabled effect contributes no claims (its ports return to the pool).

import { derived, type Readable } from 'svelte/store'
import { type Claim, PortKind, portRefKey } from './devicemodel'
import { lightfxDraft } from './lightfx'
import { gunfxDraft } from './gunfx'
import { landingDraft } from './landing'
import { gearDraft } from './gear'

/** A wire-format PortRefT from any effect store — narrow shape that all
 *  three effects share (we don't depend on the per-effect symbol so we
 *  can collect ports without a type-import cycle). */
interface EffectPortRef { guid: string; kind: string; idx: number }

/** Translate the effect-draft string kind ('pwm' / 'servo' / 'hbridge' /
 *  'input') to the numeric byte the device-model PortRef uses.  Returns
 *  -1 for empty / unknown so callers can drop the claim — was the source
 *  of a 2026-05-24 bug where synthesized claims keyed `"|pwm|5"` while
 *  real device ports keyed `"|2|5"` (PortKind.Pwm = 2), so the filter
 *  silently never matched and GunFx ports stayed selectable in LightFx. */
function kindByteFromName(name: string): number {
    switch (name) {
        case 'servo':   return PortKind.Servo
        case 'pwm':     return PortKind.Pwm
        case 'hbridge': return PortKind.HBridge
        case 'input':   return PortKind.Input
        default:        return -1
    }
}

/** Push one synthesized claim to `out`, but only if the ref is fully
 *  populated.  Empty refs ({guid:'', kind:'', idx:0}) are the
 *  "operator hasn't picked yet" sentinel — skipping them stops a
 *  blank slot from claiming and accidentally poisoning the picker for
 *  the first servo on every board.  Unknown kind names also skip. */
function pushIfBound(out: Claim[], domain: string, slot: string, p: EffectPortRef): void {
    if (!p || !p.kind) return
    const kindByte = kindByteFromName(p.kind)
    if (kindByte < 0) return
    // Note: guid === '' is legitimate (= hub-local).  Only kind decides
    // whether the operator has picked.
    out.push({ domain, slot, port: { guid: p.guid, kind: kindByte, index: p.idx } })
}

/** effectClaims — unified [hard + soft] claim list used by every port
 *  picker.  Auto-recomputes whenever any contributing draft store
 *  changes, so the operator's edits are visible in sibling pickers
 *  on the next render tick (no manual refresh). */
export const effectClaims: Readable<Claim[]> = derived(
    [lightfxDraft, gunfxDraft, landingDraft, gearDraft],
    ([$lfx, $gfx, $lnd, $gear]) => {
        const out: Claim[] = []
        const seen = new Set(out.map(c => `${c.domain}|${c.slot}|${portRefKey(c.port)}`))
        const add = (domain: string, slot: string, p: EffectPortRef) => {
            if (!p || !p.kind) return
            const kindByte = kindByteFromName(p.kind)
            if (kindByte < 0) return
            // Key on the numeric byte so the dedup matches the
            // device-model PortRef shape (same fix as pushIfBound).
            const key = `${domain}|${slot}|${p.guid}|${kindByte}|${p.idx}`
            if (seen.has(key)) return
            seen.add(key)
            pushIfBound(out, domain, slot, p)
        }

        // A DISABLED effect releases its ports back to the unclaimed pool
        // (the firmware doesn't attach its roles when disabled, so a sibling
        // effect is free to grab them).  Each effect's whole claim block is
        // gated on its top-level `enabled` flag — toggle an effect off and its
        // servos/LEDs reappear in every other picker on the next tick.

        // ── LightFx channel pool (Phase 1, 2026-05-27) ─────────────
        // Each named LED channel binds a physical led-animator port.
        // Synthesize a claim per bound channel so OTHER effects' pickers
        // (GunFx muzzle, Landing LEDs) exclude ports LightFx is using —
        // bidirectional with LightFx's own picker, which filters out the
        // `lightfx` domain and handles siblings via its draft.
        if ($lfx?.enabled) {
            for (const ch of $lfx.channels ?? []) {
                if (ch.port) add('lightfx', `channel / ${ch.name || '?'}`, ch.port)
            }
        }

        // ── GunFx per-gun outputs (muzzle / smoke heater + fan / turret) ──
        if ($gfx?.enabled) {
            for (const g of $gfx.guns ?? []) {
                const label = g.name && g.name.trim() ? g.name : `gun${g.id}`
                add('gunfx', `${label} / muzzle`,       g.muzzleFlash.port)
                add('gunfx', `${label} / smoke heater`, g.smoke.heater.port)
                add('gunfx', `${label} / smoke fan`,    g.smoke.fan.port)
                if (g.yaw.enabled)   add('gunfx', `${label} / yaw servo`,   g.yaw.servoPort)
                if (g.pitch.enabled) add('gunfx', `${label} / pitch servo`, g.pitch.servoPort)
            }
        }

        // ── Landing per-light servos + LEDs ────────────────────────
        for (const l of $lnd?.lights ?? []) {
            const label = l.name && l.name.trim() ? l.name : `landing${l.id}`
            for (let i = 0; i < (l.servos ?? []).length; i++) {
                add('landing', `${label} / servo${i + 1}`, l.servos[i].port)
            }
            for (let i = 0; i < (l.leds ?? []).length; i++) {
                add('landing', `${label} / led${i + 1}`, l.leds[i].port)
            }
        }

        // ── Gear per-strut motor + door servos (2026-06-13) ────────
        // Without this block the gear's H-bridges + door servos stayed
        // selectable in every sibling effect's picker (and vice versa —
        // GearPanel filters against this same merged list).
        if ($gear?.enabled) {
            const mode = $gear.strutMode ?? 'hbridge'
            for (const g of $gear.gears ?? []) {
                const label = g.name && g.name.trim() ? g.name : `strut${g.id}`
                if (mode === 'hbridge')     add('gear', `${label} / motor`, g.motor)
                else if (mode === 'servo')  add('gear', `${label} / strut`, g.strutServo)
                for (let i = 0; i < (g.doors ?? []).length; i++) {
                    add('gear', `${label} / door${i + 1}`, g.doors[i].port)
                }
            }
            // servo_shared: the ONE channel for the whole set (2.45.0).
            if (mode === 'servo_shared' && $gear.strutShared?.port) {
                add('gear', 'gear / shared strut', $gear.strutShared.port)
            }
        }

        return out
    },
)
