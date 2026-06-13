// port_keys.ts — the ONE place that builds/parses the `guid|kind|idx` key
// used to match an effect's stored PortRef against a device-model <option>.
//
// These were copy-pasted into every effect panel (LandingPanel, GunFxPanel, …)
// with subtly different guards, and the copies drifted: the `!r.guid` guard
// blanked every HUB-LOCAL port once the GUID redesign (instructions/31) made
// the empty guid the canonical hub form, so loaded configs showed empty
// dropdowns. Shared here so the value-side key (portRefToKey) and the
// option-side key (modelPortKey) can NEVER disagree, and so the rule is
// unit-tested (port_keys.test.ts).

import { formatPortRail, type Port } from '../devicemodel'

export interface PortRefLike { board?: string; guid: string; kind: string; idx: number }

/** Stable key for an effect's stored PortRef. Hub-local is the canonical
 *  EMPTY guid → key `|kind|idx`. Only `kind` is required; an empty guid is
 *  VALID (not "unset"). A null/kind-less ref → '' (the <select>'s "none"). */
export function portRefToKey(r: PortRefLike | null | undefined): string {
    if (!r || !r.kind) return ''
    return `${r.guid}|${r.kind}|${r.idx}`
}

/** The SAME key derived from a device-model Port — what the <option> values
 *  use. MUST stay in lock-step with portRefToKey or a loaded ref won't
 *  resolve to its option. */
export function modelPortKey(p: Port): string {
    return `${p.ref.guid}|${p.kindName}|${p.ref.index}`
}

/** Parse a `guid|kind|idx` key back into a PortRef. Empty key → unset ref
 *  (kind falls back to the caller's expected kind). */
export function parsePortKey(key: string, kindFallback: string): PortRefLike {
    if (!key) return { board: '', guid: '', kind: kindFallback, idx: 0 }
    const [guid, kind, idxStr] = key.split('|')
    return { board: '', guid, kind, idx: Number(idxStr) }
}

/** Operator-facing label for a device-model Port in a picker <option>:
 *  `Board · alias (silkscreen) · rail` — alias (IO-tab name) first when set,
 *  falling back to the silkscreen hardwareName.  Reactive on $deviceModel, so
 *  a rename on the IO tab propagates through every picker.  Was copy-pasted
 *  identically into Gear/GunFx/Landing — hoisted 2026-06-13. */
export function refOptLabel(p: Port): string {
    const rail  = formatPortRail(p.voltageMv)
    const alias = p.name && p.name.trim()
    const head  = alias ? `${alias} (${p.hardwareName})` : p.hardwareName
    return `${p.boardName ?? 'Hub'} · ${head}${rail ? ` · ${rail}` : ''}`
}
