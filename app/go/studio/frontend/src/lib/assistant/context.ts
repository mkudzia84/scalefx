// assistant/context — assembles the LIVE-STATE block the assistant is grounded
// in, from what the operator actually sees: the device model + the effect
// drafts.  Uses HUMAN-READABLE names throughout — port labels (not hbridge[0])
// and channel-function LABELS + the channel number they're mapped to (not the
// internal id like "landing_gear").

import { get } from 'svelte/store'
import { connectionInfo } from '../stores'
import { deviceModel, type Port, type DeviceModelSnapshotT } from '../devicemodel'
import { modelPortKey, portRefToKey } from '../components/port_keys'
import { engineDraft } from '../effects'
import { gunfxDraft } from '../gunfx'
import { gearDraft } from '../gear'
import { lightfxDraft } from '../lightfx'
import { landingDraft } from '../landing'

function cap(s: string): string { return s ? s[0].toUpperCase() + s.slice(1) : s }
function titleCase(s: string): string { return s.split('_').map(cap).join(' ') }

// ── Ports ──
// Resolution uses the canonical `guid|kind|index` key (the same the panels use),
// so a hub-local servo (empty guid) and an expander servo with the SAME index
// never collide — and we always know which board each port is on.
function portBoard(p: Port): string {
    return p.ref.guid ? `expander ${p.boardName || p.ref.guid}` : 'main controller'
}
function portFriendly(p: Port): string {
    const base = (p.name && p.name.trim()) || (p.hardwareName && p.hardwareName.trim()) || `${cap(p.kindName)} ${p.ref.index + 1}`
    const v = p.voltageMv > 0 ? `, ${Math.round(p.voltageMv / 1000)}V` : ''
    return `${base} [${portBoard(p)}${v}]`
}
function buildPortIndex(ports: Port[]): Map<string, Port> {
    const m = new Map<string, Port>()
    for (const p of ports) m.set(modelPortKey(p), p)
    return m
}
function resolvePort(ref: any, idx: Map<string, Port>): Port | undefined {
    const key = portRefToKey(ref)
    return key ? idx.get(key) : undefined
}
function refName(ref: any, idx: Map<string, Port>): string {
    const p = resolvePort(ref, idx)
    if (p) return portFriendly(p)
    if (ref && ref.kind) return `${cap(String(ref.kind))} ${(ref.idx ?? 0) + 1} (unassigned)`
    return '(unassigned)'
}
// An optional part is "assigned" when the port it points at has a role attached
// (picking a port auto-attaches its role).  emptyRef uses idx:0, so this is the
// reliable signal that an optional output (smoke heater, etc.) is really set.
function refAssigned(ref: any, idx: Map<string, Port>): boolean {
    const p = resolvePort(ref, idx)
    return !!(p && p.roleKind)
}

// ── Channels — by LABEL + the channel number they map to ──
function fnLabel(fnId: string, dm: DeviceModelSnapshotT): string {
    if (!fnId) return ''
    const def = (dm.channelFunctions || []).find(f => f.id === fnId)
    return (def && def.label) ? def.label : titleCase(fnId)
}
function fnChannelNum(fnId: string, dm: DeviceModelSnapshotT): number | null {
    for (const inp of dm.inputs || []) {
        for (const ch of inp.channels || []) {
            if (ch.function === fnId) return ch.channel
        }
    }
    return null
}
// "Landing Gear (channel 5)" / "Engine On/Off (not mapped to a channel)".
function chanRef(fnId: string, dm: DeviceModelSnapshotT): string {
    if (!fnId) return 'manual (no radio channel)'
    const label = fnLabel(fnId, dm)
    const num = fnChannelNum(fnId, dm)
    return num != null ? `\`${label}\` (\`CH${num}\`)` : `\`${label}\` (not mapped to a channel)`
}

// Speaker-routing bitmask: 1 = left, 2 = right, 3 = both.
function maskName(m: number): string { return m === 1 ? 'left' : m === 2 ? 'right' : 'stereo' }

// A threshold + hysteresis gate, compactly.
function gate(thresholdUs: number, hysteresisUs: number): string {
    return `above \`${thresholdUs}µs\` (±\`${hysteresisUs}µs\`)`
}

// One light-program event: its kind + the timing/levels that apply to that kind.
function evDesc(e: any): string {
    const bits: string[] = [e.kind]
    if (e.durationMs) bits.push(`${e.durationMs}ms`)
    if (e.cycleMs) bits.push(`cycle ${e.cycleMs}ms`)
    if (e.kind === 'on' || e.kind === 'flash' || e.kind === 'fade_in' || e.kind === 'fade_out') bits.push(`${e.brightnessPct}%`)
    if (e.kind === 'fading' || e.kind === 'beacon') bits.push(`${e.minPct}-${e.maxPct}%`)
    if (e.kind === 'beacon' || e.kind === 'flash') bits.push(`flash ${e.flashPct}%`)
    return bits.join(' ')
}

export function buildAssistantContext(): string {
    const conn = get(connectionInfo)
    if (!conn || !conn.connected) {
        return 'No board is connected. Tell the operator to connect the model to get setup-specific advice.'
    }
    const dm = get(deviceModel)
    const pidx = buildPortIndex(dm.ports)
    const out: string[] = []

    out.push(`Board: ${conn.controllerType || 'unknown'}${conn.controllerName ? ` (${conn.controllerName})` : ''}${conn.firmwareVersion ? `, firmware ${conn.firmwareVersion}` : ''}.`)
    out.push('(In your answers, refer to ports, roles, channels, and values using these human-readable names, and wrap each in backticks so it renders highlighted.)')

    // Radio channels (label + number)
    out.push('')
    out.push('RADIO CHANNELS (mapped functions):')
    const chans: string[] = []
    for (const inp of dm.inputs || []) {
        for (const ch of inp.channels || []) {
            if (ch.function) chans.push(`  - \`CH${ch.channel}\` -> \`${fnLabel(ch.function, dm)}\` [${inp.protocol}]`)
        }
    }
    out.push(chans.length ? chans.join('\n') : '  - (no channels mapped yet)')

    // Effects — read each draft's real enabled state + key settings.
    out.push('')
    out.push('CONFIGURED EFFECTS:')
    out.push('(Each value is labelled with its exact Studio setting name — the same name used in the Parameter reference. Interpret each ONLY by its Parameter-reference definition, never from the wording of the label. E.g. a sound "offset" is a SEEK into the file (skip the intro), not a delay before it.)')
    const fx: string[] = []

    // Engine — every set field (intentionally exhaustive so the assistant can
    // answer about any engine setting, including transitions/fade in-out).
    const eng: any = get(engineDraft)
    if (eng && eng.enabled) {
        const t: string[] = [`Type \`${eng.type}\``, `Speaker output \`${eng.output}\``]
        const tg = eng.toggle || {}
        t.push(`On/off channel ${chanRef(tg.input, dm)} (Threshold \`${tg.thresholdUs}µs\`, Hysteresis \`${tg.hysteresisUs}µs\`)`)
        if (tg.failsafe) t.push(`Failsafe \`${tg.failsafe}\``)
        const s = eng.sounds || {}
        const snd: string[] = []
        if (s.running) snd.push(`Running \`${s.running}\``)
        if (s.starting) snd.push(`Starting \`${s.starting}\``)
        if (s.stopping) snd.push(`Stopping \`${s.stopping}\``)
        t.push(`Sounds: ${snd.length ? snd.join(', ') : 'none set'}`)
        const tr = s.transitions || {}
        // Offsets are a SEEK into the file (skip the intro), NOT a delay — spell it out inline so the model can't misread.
        t.push(`Starting offset \`${tr.startingOffsetMs}ms\` (seek INTO the start sound, not a delay), Stopping offset \`${tr.stoppingOffsetMs}ms\` (seek INTO the stop sound, not a delay), Start fade-in \`${tr.startFadeInMs}ms\`, Stop fade-out \`${tr.stopFadeOutMs}ms\``)
        fx.push(`Engine sound: ON — ${t.join('; ')}.`)
    } else fx.push('Engine sound: off.')

    // Guns (incl. rate of fire, recoil, smoke heater/fan, turret axes)
    const gun: any = get(gunfxDraft)
    if (gun && gun.enabled && gun.guns && gun.guns.length) {
        for (const g of gun.guns) {
            const parts: string[] = []
            const tr = g.trigger || {}
            parts.push(`fire on ${chanRef(tr.input, dm)} (${gate(tr.thresholdUs, tr.hysteresisUs)})`)
            const items = (g.rof && g.rof.items) || []
            const rofItem = (it: any) => `${it.rpm}rpm${it.soundPath ? `, sound \`${it.soundPath}\` [${maskName(it.outputMask)}]` : ''}`
            if (items.length === 1) parts.push(`rate of fire \`${rofItem(items[0])}\``)
            else if (items.length > 1) {
                const bands = items.map((it: any) => `\`${it.name || 'band'}\` ${it.bandLoUs}-${it.bandHiUs}µs: ${rofItem(it)}`)
                parts.push(`rate of fire selected by ${chanRef(g.rof.input, dm)}: ${bands.join(', ')}`)
            }
            if (g.muzzleFlash && refAssigned(g.muzzleFlash.port, pidx)) parts.push(`muzzle \`${refName(g.muzzleFlash.port, pidx)}\` ${g.muzzleFlash.durationMs}ms @ ${g.muzzleFlash.brightness}%`)
            if (g.recoil) parts.push(g.recoil.enabled ? `recoil kick \`${g.recoil.jerkUs}µs\` hold \`${g.recoil.holdMs}ms\`` : 'recoil off')
            if (g.smoke && g.smoke.heater && refAssigned(g.smoke.heater.port, pidx)) {
                const h = g.smoke.heater, f = g.smoke.fan || {}
                let hs = `heater \`${refName(h.port, pidx)}\` @ ${h.elementMv}mV, mode \`${h.mode}\``
                if (h.mode === 'cycle') hs += ` (on \`${h.cycleOnMs}ms\` / off \`${h.cycleOffMs}ms\`)`
                if (h.activation && h.activation.input) hs += `, gated by ${chanRef(h.activation.input, dm)} (${gate(h.activation.thresholdUs, h.activation.hysteresisUs)})`
                let fs = `fan \`${refName(f.port, pidx)}\` @ ${f.elementMv}mV, mode \`${f.mode}\``
                if (f.mode === 'pulse') fs += ` (pulse \`${f.pulseDurationMs}ms\`)`
                parts.push(`smoke: ${hs}; ${fs}`)
            }
            if (g.yaw && g.yaw.enabled) parts.push(`yaw servo \`${refName(g.yaw.servoPort, pidx)}\` on ${chanRef(g.yaw.input, dm)} (neutral \`${g.yaw.neutralUs}µs\`)`)
            if (g.pitch && g.pitch.enabled) parts.push(`pitch servo \`${refName(g.pitch.servoPort, pidx)}\` on ${chanRef(g.pitch.input, dm)} (neutral \`${g.pitch.neutralUs}µs\`)`)
            fx.push(`Gun "\`${g.name || 'gun'}\`": ${parts.join('; ')}.`)
        }
    } else fx.push('Guns: off.')

    // Gear (master + per leg: motor duty, stall guard, doors)
    const gear: any = get(gearDraft)
    if (gear && gear.enabled && gear.gears && gear.gears.length) {
        const gi = gear.input || {}
        const head = [`coordination \`${gear.coord}\``, `switch ${chanRef(gi.name, dm)} (down ${gate(gi.thresholdUs, gi.hysteresisUs)})`]
        if (gear.deployOnConnectionLoss) head.push('deploys on signal loss')
        const gs = gear.sounds || {}
        if (gs.deploy || gs.retract) head.push(`sounds: ${[gs.deploy && `deploy \`${gs.deploy}\``, gs.retract && `retract \`${gs.retract}\``].filter(Boolean).join(', ')} [${maskName(gs.outputMask)}]`)
        fx.push(`Retractable gear: ON — ${head.join('; ')}.`)
        for (const g of gear.gears) {
            const lp: string[] = []
            lp.push(`motor \`${refName(g.motor, pidx)}\` (deploy duty \`${g.deployDuty}\`, retract duty \`${g.retractDuty}\`, timeout \`${g.timeoutMs}ms\`)`)
            const gd = g.guard || {}
            lp.push(`stall guard \`${gd.mode}\` (ratio \`${(gd.ratioX100 / 100).toFixed(2)}×\`, threshold \`${gd.thresholdMa}mA\`${gd.ceilingMa ? `, ceiling \`${gd.ceilingMa}mA\`` : ''})`)
            if (g.doors && g.doors.length) {
                let ds = `${g.doors.length} door(s) \`${g.doorMode}\``
                if (g.doorMode === 'delay') ds += ` (delay \`${g.doorDelayMs}ms\`)`
                ds += `, after deploy \`${g.closePolicy}\`: ${g.doors.map((d: any) => `\`${refName(d.port, pidx)}\``).join(', ')}`
                lp.push(ds)
            }
            fx.push(`  • Leg "\`${g.name || 'leg'}\`": ${lp.join('; ')}.`)
        }
    } else fx.push('Retractable gear: off.')

    // Lighting (master, LED channels, programs + their tracks/events, selector)
    const light: any = get(lightfxDraft)
    if (light && light.enabled) {
        const lp: string[] = [`master \`${light.masterBrightnessPct}%\``]
        const ch = (light.channels || []).map((c: any) => `\`${c.name}\` -> \`${refName(c.port, pidx)}\` @ ${c.defaultBrightnessPct ?? 0}%`)
        lp.push(ch.length ? `${ch.length} LED channel(s): ${ch.join(', ')}` : 'no LED channels yet')
        const progLines = (light.activePrograms || []).map((ap: any) => {
            const tracks = (ap.program && ap.program.tracks) || []
            const tdesc = tracks.map((t: any) => {
                const evs = (t.events || []).map(evDesc).join(' → ')
                return `\`${t.channel}\`${t.loop ? ' (loop)' : ''}: ${evs || '(no events)'}`
            })
            return `\`${ap.name}\`${tdesc.length ? ` [${tdesc.join(' | ')}]` : ''}`
        })
        lp.push(progLines.length ? `programs: ${progLines.join('; ')}` : 'no programs yet')
        const sel = light.programSelector
        if (sel && sel.enabled && sel.input) {
            const ranges = (sel.ranges || []).map((r: any) => `\`${r.program}\` @ ${r.fromUs}-${r.toUs}µs`)
            lp.push(`program selector on ${chanRef(sel.input, dm)} (±\`${sel.hysteresisUs}µs\`)${ranges.length ? ` -> ${ranges.join(', ')}` : ''}`)
        }
        fx.push(`Lighting: ON — ${lp.join('; ')}.`)
    } else fx.push('Lighting: off.')

    // Landing lights (servo open/close, per-LED brightness, fade-in, activation)
    const land: any = get(landingDraft)
    if (land && land.lights && land.lights.length) {
        for (const l of land.lights) {
            const servos = (l.servos || []).map((s: any) => `\`${refName(s.port, pidx)}\``)
            const leds = (l.leds || []).map((d: any) => `\`${refName(d.port, pidx)}\` @ ${d.brightnessPct}%`)
            const act = l.activation || {}
            let when = '`manual`'
            if (act.mode === 'input_channel') when = `on ${chanRef(act.input, dm)} (deploy ${gate(act.thresholdUs, act.hysteresisUs)})`
            else if (act.mode === 'program') when = `with program \`${act.program}\` when \`${act.whenProgram}\``
            fx.push(`Landing light "\`${l.name || 'light'}\`": servos ${servos.join(', ') || '(none)'} (open \`${l.openUs}µs\` / close \`${l.closeUs}µs\`), LEDs ${leds.join(', ') || '(none)'}, fade-in \`${l.fadeInMs}ms\`, activates ${when}.`)
        }
    } else fx.push('Landing lights: none.')

    out.push(fx.map(f => f.startsWith('  •') ? f : `  - ${f}`).join('\n'))

    // Ports — friendly name -> role.
    out.push('')
    out.push('PORTS (human-readable name -> attached role):')
    const inUse: string[] = []
    const free: string[] = []
    for (const p of dm.ports) {
        if (p.kindName === 'input') continue
        if (p.offline) { inUse.push(`\`${portFriendly(p)}\` - offline (board not connected)`); continue }
        if (p.roleKind && p.roleName) inUse.push(`\`${portFriendly(p)}\` -> \`${p.roleName}\``)
        else free.push(`\`${portFriendly(p)}\``)
    }
    out.push(inUse.length ? inUse.map(s => `  - ${s}`).join('\n') : '  - (no roles attached)')
    out.push(`FREE PORTS (available to assign): ${free.length ? free.join(', ') : 'none'}.`)

    return out.join('\n')
}
