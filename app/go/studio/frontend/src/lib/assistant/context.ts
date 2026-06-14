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
    const fx: string[] = []

    // Engine
    const eng = get(engineDraft)
    if (eng && eng.enabled) {
        const t = [`type \`${eng.type}\``, `speakers \`${eng.output}\``, `on/off via ${chanRef(eng.toggle && eng.toggle.input, dm)}`]
        if (eng.toggle && eng.toggle.failsafe) t.push(`on signal loss: \`${eng.toggle.failsafe}\``)
        if (eng.sounds) {
            const s: string[] = []
            if (eng.sounds.running) s.push(`running \`${eng.sounds.running}\``)
            if (eng.sounds.starting) s.push(`start \`${eng.sounds.starting}\``)
            if (eng.sounds.stopping) s.push(`stop \`${eng.sounds.stopping}\``)
            if (s.length) t.push(`sounds: ${s.join(', ')}`)
        }
        fx.push(`Engine sound: ON — ${t.join('; ')}.`)
    } else fx.push('Engine sound: off.')

    // Guns (incl. rate of fire)
    const gun = get(gunfxDraft)
    if (gun && gun.enabled && gun.guns && gun.guns.length) {
        for (const g of gun.guns) {
            const parts: string[] = [`fire on ${chanRef(g.trigger && g.trigger.input, dm)}`]
            const items = (g.rof && g.rof.items) || []
            if (items.length === 1) parts.push(`rate of fire \`${items[0].rpm} rpm\``)
            else if (items.length > 1) {
                const bands = items.map(it => `\`${it.name || 'band'}\` ${it.rpm} rpm (${it.bandLoUs}-${it.bandHiUs}µs)`)
                parts.push(`rate of fire selected by ${chanRef(g.rof && (g.rof as any).input, dm)}: ${bands.join(', ')}`)
            }
            if (g.muzzleFlash) parts.push(`muzzle \`${refName(g.muzzleFlash.port, pidx)}\` ${g.muzzleFlash.durationMs}ms @ ${g.muzzleFlash.brightness}%`)
            if (g.recoil && g.recoil.enabled) parts.push(`recoil kick \`${g.recoil.jerkUs}µs\``)
            if (g.smoke && g.smoke.heater && refAssigned(g.smoke.heater.port, pidx)) {
                parts.push(`smoke: heater \`${refName(g.smoke.heater.port, pidx)}\` @ ${g.smoke.heater.elementMv}mV, fan \`${refName(g.smoke.fan && g.smoke.fan.port, pidx)}\``)
            }
            if (g.yaw && g.yaw.enabled) parts.push(`yaw servo \`${refName(g.yaw.servoPort, pidx)}\` on ${chanRef(g.yaw.input, dm)}`)
            if (g.pitch && g.pitch.enabled) parts.push(`pitch servo \`${refName(g.pitch.servoPort, pidx)}\` on ${chanRef(g.pitch.input, dm)}`)
            fx.push(`Gun "\`${g.name || 'gun'}\`": ${parts.join('; ')}.`)
        }
    } else fx.push('Guns: off.')

    // Gear (per leg)
    const gear = get(gearDraft)
    if (gear && gear.enabled && gear.gears && gear.gears.length) {
        const head = [`coordination \`${gear.coord}\``, `switch ${chanRef(gear.input && gear.input.name, dm)} (ON = down)`]
        if (gear.deployOnConnectionLoss) head.push('deploys on signal loss')
        fx.push(`Retractable gear: ON — ${head.join('; ')}.`)
        for (const g of gear.gears) {
            const fwd = (g.deployDuty ?? 0) >= 0
            const lp: string[] = [`motor \`${refName(g.motor, pidx)}\``, `deploy drives \`${fwd ? 'forward' : 'reverse'}\``, `timeout \`${g.timeoutMs}ms\``]
            if (g.doors && g.doors.length) lp.push(`${g.doors.length} door(s) [\`${g.doorMode}\`, after deploy: \`${g.closePolicy}\`]: ${g.doors.map((d: any) => `\`${refName(d.port, pidx)}\``).join(', ')}`)
            fx.push(`  • Leg "\`${g.name || 'leg'}\`": ${lp.join('; ')}.`)
        }
    } else fx.push('Retractable gear: off.')

    // Lighting (incl. programs + selector)
    const light = get(lightfxDraft)
    if (light && light.enabled) {
        const lp: string[] = [`master \`${light.masterBrightnessPct}%\``]
        const ch = (light.channels || []).map((c: any) => `\`${c.name}\` -> \`${refName(c.port, pidx)}\` @ ${c.defaultBrightnessPct ?? 100}%`)
        lp.push(ch.length ? `${ch.length} LED channel(s): ${ch.join(', ')}` : 'no LED channels yet')
        const progs = (light.activePrograms || []).map((p: any) => `\`${p.name}\``)
        lp.push(progs.length ? `programs: ${progs.join(', ')}` : 'no programs yet')
        const sel = light.programSelector
        if (sel && sel.enabled && sel.input) {
            const ranges = (sel.ranges || []).map((r: any) => `\`${r.program}\``)
            lp.push(`program selector on ${chanRef(sel.input, dm)}${ranges.length ? ` -> ${ranges.join(', ')}` : ''}`)
        }
        fx.push(`Lighting: ON — ${lp.join('; ')}.`)
    } else fx.push('Lighting: off.')

    // Landing lights
    const land = get(landingDraft)
    if (land && land.lights && land.lights.length) {
        for (const l of land.lights) {
            const servos = (l.servos || []).map((s: any) => `\`${refName(s.port, pidx)}\``)
            const leds = (l.leds || []).map((d: any) => `\`${refName(d.port, pidx)}\``)
            const act = l.activation || {}
            let when = '`manual`'
            if (act.mode === 'input_channel') when = `on ${chanRef(act.input, dm)}`
            else if (act.mode === 'program') when = `with program \`${act.program}\``
            fx.push(`Landing light "\`${l.name || 'light'}\`": servos ${servos.join(', ') || '(none)'}, LEDs ${leds.join(', ') || '(none)'}, fade-in \`${l.fadeInMs}ms\`, activates ${when}.`)
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
