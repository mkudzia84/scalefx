// ScaleFX Studio — YAML Configuration Generators + Parsers
//
// Board-agnostic helpers + per-board round-trip (generate ↔ parse). Each pair
// is symmetric: what the generator emits, the parser must read back into the
// same shape.
//
// Generators:        state  → YAML
// Parsers:    YAML  → state (+errors/warnings)
//
// The YAML dialect emitted here matches the firmware declarative schema in
// controllers/lib/sfx_config/config/*.h and gearcontrol_config.h.

import type {
    LightConfig, LightProgram, LightChannelState, LightSeqEvent,
    LightGroup, LightServoBinding, GroupPolicy,
} from './light-verifier'
import type {
    GearControlConfig, GearPinConfig, GearConfigState, YawConfigState, PinRole,
} from './gearcontrol-verifier'
import {
    parseYaml, asObj, asArr, asStr, asNum, asBool, type YamlValue,
} from './yaml-parser'
import type { ParseResult } from './board-driver'

// ─── Shared helpers ───

function indent(level: number): string {
    return '  '.repeat(level)
}

function yamlString(s: string): string {
    if (/[:#\[\]{}|>&*!?,]/.test(s) || s.trim() !== s || s === '') {
        return `"${s.replace(/"/g, '\\"')}"`
    }
    return s
}

// ─── LightFX YAML Generator ───

/**
 * Generate YAML configuration for a LightFX board.
 * Matches the schema in light_program_config.h.
 *
 * @param config  Current LightFX config state
 * @param nested  If true, wraps output under "light_fx:" (for HubFX embedding)
 */
export function generateLightYaml(config: LightConfig, nested: boolean = false): string {
    const lines: string[] = []
    const base = nested ? 1 : 0

    // Indentation convention (Rule 27 / sfx_config README "YAML Style"):
    //   parent:
    //     - key: value     ← sequence items indented 2 spaces under the key
    //       more: value    ← continuation lines indented 4 spaces
    // Every ScaleFX YAML emitter and reference config uses this form so the
    // round-trip (firmware ↔ Studio ↔ CLI) never needs ambiguity handling.
    const L0 = indent(base)        // parent-key indent
    const L1 = indent(base + 1)    // sequence-item "- " indent
    const L2 = indent(base + 2)    // continuation / nested-item indent
    const L3 = indent(base + 3)    // nested-sequence continuation
    const L4 = indent(base + 4)    // nested-sequence item continuation

    if (nested) lines.push('light_fx:')

    lines.push(`${L0}master_brightness: ${config.masterBrightness}`)
    lines.push('')

    if (config.battery) {
        lines.push(`${L0}battery:`)
        lines.push(`${L1}auto_cutoff: ${config.battery.autoCutoff}`)
        lines.push(`${L1}chemistry: ${config.battery.chemistry}`)
        lines.push(`${L1}cell_count: ${config.battery.cellCount}`)
        lines.push('')
    }

    const activeGroups = config.groups.filter(g =>
        g.memberChannels.length > 0 ||
        (g.hubfxMemberChannels?.length ?? 0) > 0 ||
        g.servoBindings.length > 0
    )
    if (activeGroups.length > 0) {
        lines.push(`${L0}landing_groups:`)
        for (const group of activeGroups) {
            lines.push(`${L1}- name: ${yamlString(group.name)}`)
            // Split channel mask: lightfx-side (8 chans, the slave) vs
            // hubfx-side (6 chans, the master's local LEDs). Each board
            // loads the same /lightfx.yaml and applies only its own bits.
            let lfxMask = 0
            for (const ch of group.memberChannels) lfxMask |= (1 << (ch - 1))
            let hfxMask = 0
            for (const ch of group.hubfxMemberChannels ?? []) hfxMask |= (1 << (ch - 1))
            lines.push(`${L2}lightfx_channels: 0x${lfxMask.toString(16).toUpperCase().padStart(2, '0')}`)
            lines.push(`${L2}hubfx_channels: 0x${hfxMask.toString(16).toUpperCase().padStart(2, '0')}`)
            // servo_board defaults to lightfx — only emit when set to hubfx
            // (keeps round-trip diffs minimal on legacy lightfx-only configs).
            if (group.servoBoard === 'hubfx') {
                lines.push(`${L2}servo_board: hubfx`)
            }
            if (group.servoBindings.length > 0) {
                const b = group.servoBindings[0]
                lines.push(`${L2}servo_id: ${b.servo}`)
                lines.push(`${L2}servo_min_us: ${b.servoMin_us}`)
                lines.push(`${L2}servo_max_us: ${b.servoMax_us}`)
                lines.push(`${L2}servo_speed: ${b.servoSpeed}`)
                if (b.servoReversed) {
                    lines.push(`${L2}servo_reversed: true`)
                }
            } else {
                lines.push(`${L2}servo_id: 0`)
            }
        }
        lines.push('')
    }

    lines.push(`${L0}input:`)
    lines.push(`${L1}channel: 1`)
    const bandsWithPrograms = config.programs
        .map((p, i) => ({ min: p.bandMin_us, max: p.bandMax_us, program: i }))
        .filter(b => b.min <= b.max)
    if (bandsWithPrograms.length > 0) {
        lines.push(`${L1}bands:`)
        for (const b of bandsWithPrograms) {
            lines.push(`${L2}- min_us: ${b.min}`)
            lines.push(`${L3}max_us: ${b.max}`)
            lines.push(`${L3}program: ${b.program}`)
        }
    }
    lines.push('')

    if (config.programs.length > 0) {
        lines.push(`${L0}programs:`)
        for (const prog of config.programs) {
            lines.push(`${L1}- name: ${yamlString(prog.name)}`)
            if (prog.groupPolicies.length > 0) {
                lines.push(`${L2}group_policies: [${prog.groupPolicies.join(', ')}]`)
            }

            const activeChannels = prog.channels
                .map((c, i) => ({ ...c, index: i }))
                .filter(c => c.enabled)

            if (activeChannels.length > 0) {
                lines.push(`${L2}channels:`)
                for (const ch of activeChannels) {
                    lines.push(`${L3}- channel: ${ch.index + 1}`)

                    // board defaults to lightfx — only emit when set to hubfx
                    // so legacy lightfx-only configs round-trip unchanged.
                    if (ch.board === 'hubfx') {
                        lines.push(`${L4}board: hubfx`)
                    }

                    if (ch.mode === 'group') {
                        lines.push(`${L4}group: ${ch.groupIndex}`)
                    } else if (ch.events.length > 0) {
                        lines.push(`${L4}events:`)
                        const L5 = indent(base + 5)
                        const L6 = indent(base + 6)
                        for (const evt of ch.events) {
                            lines.push(`${L5}- type: ${evt.type}`)
                            const p = evt.params
                            switch (evt.type) {
                                case 'on':
                                    if (p.duration)    lines.push(`${L6}duration_ms: ${p.duration}`)
                                    if (p.brightness !== undefined && p.brightness !== 100)
                                                       lines.push(`${L6}brightness: ${p.brightness}`)
                                    if (p.pwmDuty)     lines.push(`${L6}pwm_duty: ${p.pwmDuty}`)
                                    break
                                case 'off':
                                    if (p.duration)    lines.push(`${L6}duration_ms: ${p.duration}`)
                                    break
                                case 'flash':
                                    lines.push(`${L6}cycle_ms: ${p.interval ?? p.cycle ?? 500}`)
                                    if (p.duration)    lines.push(`${L6}duration_ms: ${p.duration}`)
                                    if (p.brightness !== undefined && p.brightness !== 100)
                                                       lines.push(`${L6}brightness: ${p.brightness}`)
                                    if (p.duty !== undefined && p.duty !== 50)
                                                       lines.push(`${L6}duty: ${p.duty}`)
                                    break
                                case 'fadein':
                                case 'fadeout':
                                    lines.push(`${L6}duration_ms: ${p.duration ?? 1000}`)
                                    if (p.brightness !== undefined && p.brightness !== 100)
                                                       lines.push(`${L6}brightness: ${p.brightness}`)
                                    break
                                case 'fading':
                                    lines.push(`${L6}cycle_ms: ${p.cycle ?? 2000}`)
                                    if (p.duration)    lines.push(`${L6}duration_ms: ${p.duration}`)
                                    if (p.min !== undefined) lines.push(`${L6}min_brightness: ${p.min}`)
                                    if (p.max !== undefined) lines.push(`${L6}max_brightness: ${p.max}`)
                                    break
                                case 'beacon':
                                    lines.push(`${L6}cycle_ms: ${p.cycle ?? 1200}`)
                                    if (p.duration)    lines.push(`${L6}duration_ms: ${p.duration}`)
                                    if (p.flashPct !== undefined) lines.push(`${L6}flash_pct: ${p.flashPct}`)
                                    if (p.max !== undefined) lines.push(`${L6}max_brightness: ${p.max}`)
                                    if (p.min !== undefined) lines.push(`${L6}min_brightness: ${p.min}`)
                                    break
                            }
                        }
                    }
                }
            }
            lines.push('')
        }
    }

    return lines.join('\n')
}

// ─── LightFX YAML Parser ───
//
// Reverses generateLightYaml. Tolerant of order / missing keys; uses defaults
// where reasonable. Errors accumulated in ParseResult.errors.

function maskToChannels(mask: number, channelCount: number): number[] {
    const out: number[] = []
    for (let i = 0; i < channelCount; i++) {
        if (mask & (1 << i)) out.push(i + 1)
    }
    return out
}

/**
 * @param yaml         YAML source text (either top-level or nested).
 * @param nested       true when parsing HubFX-style config (reads under "light_fx:").
 * @param channelCount how many LED channels the board exposes (8 for LightFX, 6 for HubFX).
 * @param pwmMin/Max   PWM bounds used by the verifier.
 */
export function parseLightYaml(
    yaml: string, nested: boolean, channelCount: number, pwmMin: number, pwmMax: number,
    hubAvailable: boolean = false,
): ParseResult<LightConfig> {
    const { value, errors: parseErrs } = parseYaml(yaml)
    const errors = parseErrs.map(e => `line ${e.line}: ${e.message}`)
    const warnings: string[] = []

    const root = nested
        ? asObj(asObj(value)['light_fx'])
        : asObj(value)

    if (Object.keys(root).length === 0 && errors.length === 0) {
        warnings.push(nested ? 'No "light_fx:" section found' : 'Configuration is empty')
    }

    const masterBrightness = asNum(root['master_brightness'], 100)

    const battObj = asObj(root['battery'])
    const battery = {
        autoCutoff: asBool(battObj['auto_cutoff'], true),
        chemistry:  asStr(battObj['chemistry'], 'lipo'),
        cellCount:  asNum(battObj['cell_count'], 0),
    }

    // Groups ← landing_groups
    // Supports both the new split keys (lightfx_channels + hubfx_channels)
    // and the legacy single channel_mask, which is treated as lightfx-side.
    const groups: LightGroup[] = []
    const lgArr = asArr(root['landing_groups'])
    for (const item of lgArr) {
        const g = asObj(item)
        const name = asStr(g['name'], `Group ${groups.length + 1}`)
        const lfxMask = g['lightfx_channels'] !== undefined
            ? asNum(g['lightfx_channels'], 0)
            : asNum(g['channel_mask'], 0)   // legacy
        const hfxMask = asNum(g['hubfx_channels'], 0)
        const servoBoardStr = asStr(g['servo_board'], 'lightfx').toLowerCase()
        const servoBoard: 'lightfx' | 'hubfx' = servoBoardStr === 'hubfx' ? 'hubfx' : 'lightfx'
        const servoId = asNum(g['servo_id'], 0)
        const bindings: LightServoBinding[] = servoId > 0 ? [{
            servo: servoId,
            servoMin_us: asNum(g['servo_min_us'], 500),
            servoMax_us: asNum(g['servo_max_us'], 2500),
            servoSpeed: asNum(g['servo_speed'], 4000),
            servoAccel: asNum(g['servo_accel'], 8000),
            servoDecel: asNum(g['servo_decel'], 8000),
            servoReversed: asBool(g['servo_reversed'], false),
        }] : []
        groups.push({
            name,
            // Lightfx side uses the lightfx channel-count for bit→channel
            // expansion (8); hubfx side is always 6.
            memberChannels:      maskToChannels(lfxMask, 8),
            hubfxMemberChannels: maskToChannels(hfxMask, 6),
            servoBoard,
            servoBindings: bindings,
        })
    }

    // Programs ← programs + input.bands for per-program band range
    const bandsByProgram = new Map<number, { min: number, max: number }>()
    const input = asObj(root['input'])
    for (const item of asArr(input['bands'])) {
        const b = asObj(item)
        const pgm = asNum(b['program'], -1)
        if (pgm >= 0) bandsByProgram.set(pgm, {
            min: asNum(b['min_us'], pwmMin),
            max: asNum(b['max_us'], pwmMax),
        })
    }

    const programs: LightProgram[] = []
    const pgArr = asArr(root['programs'])
    for (let pi = 0; pi < pgArr.length; pi++) {
        const p = asObj(pgArr[pi])
        const name = asStr(p['name'], `Program ${pi + 1}`)
        const band = bandsByProgram.get(pi)

        const groupPolicies: GroupPolicy[] = []
        const gpRaw = p['group_policies']
        if (Array.isArray(gpRaw)) {
            for (const v of gpRaw) {
                const n = asNum(v, 0)
                groupPolicies.push(n === 2 ? 'gear' : n === 1 ? 'on' : 'off')
            }
        }

        const channels: LightChannelState[] = Array.from({ length: channelCount }, () => ({
            enabled: false,
            mode: 'events' as const,
            groupIndex: 0,
            events: [],
            board: 'lightfx' as const,
        }))

        for (const chItem of asArr(p['channels'])) {
            const c = asObj(chItem)
            const channel = asNum(c['channel'], 0)
            if (channel < 1 || channel > channelCount) continue
            const state = channels[channel - 1]
            state.enabled = true
            const boardStr = asStr(c['board'], 'lightfx').toLowerCase()
            state.board = boardStr === 'hubfx' ? 'hubfx' : 'lightfx'
            if (c['group'] !== undefined) {
                state.mode = 'group'
                state.groupIndex = asNum(c['group'], 0)
            } else {
                state.mode = 'events'
                for (const evItem of asArr(c['events'])) {
                    const ev = asObj(evItem)
                    const type = asStr(ev['type'], 'off')
                    const params: Record<string, number> = {}
                    if (ev['duration_ms']   !== undefined) params.duration   = asNum(ev['duration_ms'])
                    if (ev['cycle_ms']      !== undefined) params.cycle      = asNum(ev['cycle_ms'])
                    if (ev['brightness']    !== undefined) params.brightness = asNum(ev['brightness'])
                    if (ev['pwm_duty']      !== undefined) params.pwmDuty    = asNum(ev['pwm_duty'])
                    if (ev['duty']          !== undefined) params.duty       = asNum(ev['duty'])
                    if (ev['flash_pct']     !== undefined) params.flashPct   = asNum(ev['flash_pct'])
                    if (ev['min_brightness']!== undefined) params.min        = asNum(ev['min_brightness'])
                    if (ev['max_brightness']!== undefined) params.max        = asNum(ev['max_brightness'])
                    state.events.push({ id: state.events.length + 1, type, params } as LightSeqEvent)
                }
            }
        }

        programs.push({
            name,
            bandMin_us: band?.min ?? pwmMin,
            bandMax_us: band?.max ?? pwmMax,
            channels,
            groupPolicies,
        })
    }

    const state: LightConfig = {
        programs, groups, masterBrightness, battery, channelCount,
        pwmMin, pwmMax, hubAvailable,
    }

    return { state, errors, warnings }
}

// ─── GearControl YAML Generator ───
//
// Emits the YAML format documented in gearcontrol_config.h:
//   retracts: [...]   — per-gear motor + stall settings
//   pins:     [...]   — per-pin role (door/gear_input/yaw_output/...)
//   door_modes: [...] — sequencing per gear (not currently tab-editable; omitted)
//   battery: {...}    — chemistry, cell count, auto_deploy

const pinSlotNames = ['pin1', 'pin2', 'pin3', 'pin4', 'pin5', 'pin6', 'pin7']

export interface GearControlFullConfig extends GearControlConfig {
    /** Extra fields not reflected in the verifier but round-tripped through YAML. */
    retracts?: { channel: number; enabled: boolean; stallCurrent_mA: number; timeout_ms: number }[]
    doorModes?: { channel: number; preDeploy: string; postDeploy: string; delay_ms: number }[]
    battery?:   { autoDeploy: boolean; chemistry: string; cellCount: number }
}

export function generateGearControlYaml(config: GearControlFullConfig, nested: boolean = false): string {
    const lines: string[] = []
    const base = nested ? 1 : 0
    // Indentation (Rule 27 / sfx_config README "YAML Style"): sequence items
    // sit 2 spaces under the parent key; continuation lines 4 spaces under.
    const L0 = indent(base)
    const L1 = indent(base + 1)
    const L2 = indent(base + 2)

    if (nested) lines.push('gear_control:')

    // Retracts
    if (config.retracts && config.retracts.length > 0) {
        lines.push(`${L0}retracts:`)
        for (const r of config.retracts) {
            lines.push(`${L1}- channel: ${r.channel}`)
            lines.push(`${L2}enabled: ${r.enabled}`)
            lines.push(`${L2}stall_current_mA: ${r.stallCurrent_mA}`)
            lines.push(`${L2}timeout_ms: ${r.timeout_ms}`)
        }
        lines.push('')
    }

    // Pins — one entry per configured pin, only role-relevant fields emitted.
    if (config.pins.length > 0) {
        lines.push(`${L0}pins:`)
        for (let i = 0; i < config.pins.length; i++) {
            const p = config.pins[i]
            const slot = pinSlotNames[i] ?? `pin${i + 1}`
            lines.push(`${L1}- slot: ${slot}`)
            lines.push(`${L2}role: ${p.role}`)
            if (p.role === 'door') {
                lines.push(`${L2}channel: ${p.channel}`)
                lines.push(`${L2}door_index: ${p.door_index ?? 0}`)
                lines.push(`${L2}min_us: ${p.min_us}`)
                lines.push(`${L2}max_us: ${p.max_us}`)
                lines.push(`${L2}speed: ${p.speed}`)
                if (p.reversed) lines.push(`${L2}reversed: true`)
            } else if (p.role === 'yaw_input') {
                // Firmware maps [min_us, max_us] input → [yaw.min, yaw.max] output.
                lines.push(`${L2}min_us: ${p.min_us}`)
                lines.push(`${L2}max_us: ${p.max_us}`)
            } else if (p.role === 'yaw_output') {
                lines.push(`${L2}gear_id: ${p.gear_id}`)
                lines.push(`${L2}neutral_us: ${p.neutral_us}`)
                lines.push(`${L2}min_us: ${p.min_us}`)
                lines.push(`${L2}max_us: ${p.max_us}`)
                lines.push(`${L2}speed: ${p.speed}`)
                if (p.reversed) lines.push(`${L2}reversed: true`)
            }
        }
        lines.push('')
    }

    // Door modes
    if (config.doorModes && config.doorModes.length > 0) {
        lines.push(`${L0}door_modes:`)
        for (const dm of config.doorModes) {
            lines.push(`${L1}- channel: ${dm.channel}`)
            lines.push(`${L2}pre_deploy: ${dm.preDeploy}`)
            lines.push(`${L2}post_deploy: ${dm.postDeploy}`)
            lines.push(`${L2}delay_ms: ${dm.delay_ms}`)
        }
        lines.push('')
    }

    // Battery
    if (config.battery) {
        lines.push(`${L0}battery:`)
        lines.push(`${L1}auto_deploy: ${config.battery.autoDeploy}`)
        lines.push(`${L1}chemistry: ${config.battery.chemistry}`)
        lines.push(`${L1}cell_count: ${config.battery.cellCount}`)
        lines.push('')
    }

    // Gear input (fixed pin GP0, RC PWM)
    lines.push(`${L0}gear_input:`)
    lines.push(`${L1}enabled: ${config.gearInput.enabled}`)
    lines.push(`${L1}threshold_us: ${config.gearInput.threshold_us}`)

    return lines.join('\n')
}

export function parseGearControlYaml(yaml: string, nested: boolean = false): ParseResult<GearControlFullConfig> {
    const { value, errors: parseErrs } = parseYaml(yaml)
    const errors = parseErrs.map(e => `line ${e.line}: ${e.message}`)
    const warnings: string[] = []

    const root = nested
        ? asObj(asObj(value)['gear_control'])
        : asObj(value)

    if (Object.keys(root).length === 0 && errors.length === 0) {
        warnings.push(nested ? 'No "gear_control:" section found' : 'Configuration is empty')
    }

    const retracts = asArr(root['retracts']).map((item) => {
        const r = asObj(item)
        return {
            channel: asNum(r['channel'], 0),
            enabled: asBool(r['enabled'], true),
            stallCurrent_mA: asNum(r['stall_current_mA'], 500),
            timeout_ms: asNum(r['timeout_ms'], 60000),
        }
    })

    const validRoles: PinRole[] = ['door', 'yaw_input', 'yaw_output', 'unused']
    const pins: GearPinConfig[] = asArr(root['pins']).map((item): GearPinConfig => {
        const p = asObj(item)
        const roleStr = asStr(p['role'], 'unused')
        const role: PinRole = validRoles.includes(roleStr as PinRole) ? (roleStr as PinRole) : 'unused'
        return {
            role,
            channel: asNum(p['channel'], 0),
            door_index: asNum(p['door_index'], 0),
            gear_id: asNum(p['gear_id'], 0),
            threshold_us: asNum(p['threshold_us'], 1500),
            min_us: asNum(p['min_us'], 500),
            max_us: asNum(p['max_us'], 2500),
            speed: asNum(p['speed'], 4000),
            reversed: asBool(p['reversed'], false),
            neutral_us: asNum(p['neutral_us'], 1500),
        }
    })

    const doorModes = asArr(root['door_modes']).map((item) => {
        const d = asObj(item)
        return {
            channel: asNum(d['channel'], 0),
            preDeploy: asStr(d['pre_deploy'], 'dual_sync'),
            postDeploy: asStr(d['post_deploy'], 'none'),
            delay_ms: asNum(d['delay_ms'], 500),
        }
    })

    const battObj = asObj(root['battery'])
    const battery = Object.keys(battObj).length > 0 ? {
        autoDeploy: asBool(battObj['auto_deploy'], false),
        chemistry: asStr(battObj['chemistry'], 'lipo'),
        cellCount: asNum(battObj['cell_count'], 0),
    } : undefined

    const giObj = asObj(root['gear_input'])
    const gearInput = {
        enabled: asBool(giObj['enabled'], true),
        threshold_us: asNum(giObj['threshold_us'], 1500),
    }

    // Derive verifier fields so the resulting state also satisfies GearControlConfig.
    const gears: GearConfigState[] = [0, 1, 2].map((gi) => {
        const r = retracts.find(x => x.channel === gi)
        const doorPinCount = pins.filter(p => p.role === 'door' && p.channel === gi).length
        return {
            enabled: r?.enabled ?? true,
            calibrated: false,
            timeout_ms: r?.timeout_ms ?? 60000,
            doorPinCount,
        }
    })

    const yawPin = pins.find(p => p.role === 'yaw_output')
    const yaw: YawConfigState = {
        enabled: !!yawPin,
        gearId: yawPin?.gear_id ?? 0,
        neutral_us: yawPin?.neutral_us ?? 1500,
        min_us: yawPin?.min_us ?? 1000,
        max_us: yawPin?.max_us ?? 2000,
    }

    const state: GearControlFullConfig = {
        pins, gears, yaw, gearInput,
        isSlave: false,
        retracts, doorModes, battery,
    }

    return { state, errors, warnings }
}

// ─── HubFX-only settings YAML ───
//
// /hubfx.yaml carries hub-only settings (codec, future input mappings); the
// hub's master copy of the light program lives in /lightfx.yaml on the hub
// (HubLightFxConfigSchema, fanned to the LightFX slave on attach). Per
// Rule 26 / hubfx_config.h, light_fx never appears in /hubfx.yaml.
export interface HubFxSettings {
    codecSupplyVoltage: '12v' | '15v' | '20v' | '24v'
}

export function generateHubFxSettingsYaml(s: HubFxSettings): string {
    return [
        `codec_supply_voltage: ${s.codecSupplyVoltage}`,
        '',
    ].join('\n')
}

export function parseHubFxSettingsYaml(yaml: string): ParseResult<HubFxSettings> {
    const { value, errors: parseErrs } = parseYaml(yaml)
    const errors = parseErrs.map(e => `line ${e.line}: ${e.message}`)
    const root = asObj(value)
    const raw = asStr(root['codec_supply_voltage'], '12v').toLowerCase()
    const allowed: HubFxSettings['codecSupplyVoltage'][] = ['12v', '15v', '20v', '24v']
    const codecSupplyVoltage = (allowed as string[]).includes(raw)
        ? (raw as HubFxSettings['codecSupplyVoltage'])
        : '12v'
    return { state: { codecSupplyVoltage }, errors, warnings: [] }
}
