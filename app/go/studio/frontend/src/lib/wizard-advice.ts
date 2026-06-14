// wizard-advice.ts — the deterministic "config fitness" engine.
//
// Given the live device model + the effect drafts (via the feature registry),
// returns advice/warnings the wizard surfaces inline and on the Review step:
// does every enabled feature have its RC channel mapped, are channels
// duplicated, is anything obviously broken.  Shared by the wizard steps AND
// (later) the config assistant as grounding + a validate_config tool.

import type { DeviceModelSnapshotT } from './devicemodel'
import { collectChannelOptions } from './channels'
import { WIZARD_FEATURES } from './wizard-features'

export type AdviceLevel = 'info' | 'warn' | 'error'
export interface Advice { level: AdviceLevel; where: string; message: string }

/** The set of channel-function ids currently assigned across all input ports. */
export function mappedFunctions(dm: DeviceModelSnapshotT): Set<string> {
    return new Set(collectChannelOptions(dm).map(o => o.fnId))
}

/** Analyse the current config and return advice ordered error → warn → info. */
export function analyzeWizard(dm: DeviceModelSnapshotT): Advice[] {
    const out: Advice[] = []
    const opts = collectChannelOptions(dm)
    const mapped = new Set(opts.map(o => o.fnId))
    const enabled = WIZARD_FEATURES.filter(f => f.isEnabled())

    if (enabled.length === 0) {
        out.push({ level: 'info', where: 'features', message: 'No effects enabled yet — pick at least one in the Features step.' })
    }

    // Duplicate channel functions (two channels named the same).
    const counts = new Map<string, number>()
    for (const o of opts) counts.set(o.fnId, (counts.get(o.fnId) ?? 0) + 1)
    for (const [fn, n] of counts) {
        if (n > 1) out.push({ level: 'warn', where: 'channels', message: `Channel function "${fn}" is mapped on ${n} channels — usually one is enough.` })
    }

    // Per-feature RC coverage.
    for (const f of enabled) {
        for (const inp of f.inputs) {
            if (inp.required && !mapped.has(inp.fn)) {
                out.push({
                    level: 'warn', where: f.id,
                    message: `${f.label} is on but no channel maps to "${inp.label}". Map one in the Channels step (or drive it manually from Studio).`,
                })
            }
        }
        const bound = f.getInput()
        if (bound && !mapped.has(bound)) {
            out.push({
                level: 'warn', where: f.id,
                message: `${f.label} is bound to channel function "${bound}", but no input channel uses it yet.`,
            })
        }
    }

    const rank: Record<AdviceLevel, number> = { error: 0, warn: 1, info: 2 }
    return out.sort((a, b) => rank[a.level] - rank[b.level])
}

/** Advice scoped to one "where" key (a feature id or a step id). */
export function adviceFor(all: Advice[], where: string): Advice[] {
    return all.filter(a => a.where === where)
}
