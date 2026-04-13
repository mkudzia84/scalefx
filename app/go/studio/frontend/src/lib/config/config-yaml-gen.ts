// ScaleFX Studio — YAML Configuration Generator
//
// Generates YAML configuration text from the in-memory config state.
// Board-specific generators produce the YAML that matches the C++ schema
// in light_program_config.h.
//
// This is "generated config" — the second tab of the SaveConfigDialog.

import type { LightConfig } from './light-verifier'

// ─── Helpers ───

function indent(level: number): string {
    return '  '.repeat(level)
}

function yamlString(s: string): string {
    // Quote if contains special chars, otherwise bare
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

    if (nested) {
        lines.push('light_fx:')
    }

    // Master brightness
    lines.push(`${indent(base)}master_brightness: ${config.masterBrightness}`)
    lines.push('')

    // Landing Groups (with channel mask and optional servo bindings)
    // Include groups that have member channels OR servo bindings
    const activeGroups = config.groups.filter(g => g.memberChannels.length > 0 || g.servoBindings.length > 0)
    if (activeGroups.length > 0) {
        lines.push(`${indent(base)}landing_groups:`)
        for (const group of activeGroups) {
            lines.push(`${indent(base)}- name: ${yamlString(group.name)}`)
            // channel_mask: bitmask of member channels (bit0=ch1)
            let mask = 0
            for (const ch of group.memberChannels) mask |= (1 << (ch - 1))
            lines.push(`${indent(base)}  channel_mask: 0x${mask.toString(16).toUpperCase().padStart(2, '0')}`)
            // Optional servo binding
            if (group.servoBindings.length > 0) {
                const b = group.servoBindings[0]
                lines.push(`${indent(base)}  servo_id: ${b.servo}`)
                lines.push(`${indent(base)}  servo_min_us: ${b.servoMin_us}`)
                lines.push(`${indent(base)}  servo_max_us: ${b.servoMax_us}`)
                lines.push(`${indent(base)}  servo_speed: ${b.servoSpeed}`)
                if (b.servoReversed) {
                    lines.push(`${indent(base)}  servo_reversed: true`)
                }
            } else {
                lines.push(`${indent(base)}  servo_id: 0`)
            }
        }
        lines.push('')
    }

    // Input
    lines.push(`${indent(base)}input:`)
    lines.push(`${indent(base)}  channel: 1`)
    const bandsWithPrograms = config.programs
        .map((p, i) => ({ min: p.bandMin_us, max: p.bandMax_us, program: i }))
        .filter(b => b.min <= b.max)
    if (bandsWithPrograms.length > 0) {
        lines.push(`${indent(base)}  bands:`)
        for (const b of bandsWithPrograms) {
            lines.push(`${indent(base)}  - min_us: ${b.min}`)
            lines.push(`${indent(base)}    max_us: ${b.max}`)
            lines.push(`${indent(base)}    program: ${b.program}`)
        }
    }
    lines.push('')

    // Programs
    if (config.programs.length > 0) {
        lines.push(`${indent(base)}programs:`)
        for (const prog of config.programs) {
            lines.push(`${indent(base)}- name: ${yamlString(prog.name)}`)
            if (prog.groupPolicies.length > 0) {
                lines.push(`${indent(base)}  group_policies: [${prog.groupPolicies.join(', ')}]`)
            }

            const activeChannels = prog.channels
                .map((c, i) => ({ ...c, index: i }))
                .filter(c => c.enabled)

            if (activeChannels.length > 0) {
                lines.push(`${indent(base)}  channels:`)
                for (const ch of activeChannels) {
                    lines.push(`${indent(base)}  - channel: ${ch.index + 1}`)

                    if (ch.mode === 'group') {
                        lines.push(`${indent(base)}    group: ${ch.groupIndex}`)
                    } else if (ch.events.length > 0) {
                        lines.push(`${indent(base)}    events:`)
                        for (const evt of ch.events) {
                            lines.push(`${indent(base)}    - type: ${evt.type}`)
                            // Emit relevant params per event type
                            const p = evt.params
                            switch (evt.type) {
                                case 'on':
                                    if (p.duration)    lines.push(`${indent(base)}      duration_ms: ${p.duration}`)
                                    if (p.brightness !== undefined && p.brightness !== 100)
                                                       lines.push(`${indent(base)}      brightness: ${p.brightness}`)
                                    if (p.pwmDuty)     lines.push(`${indent(base)}      pwm_duty: ${p.pwmDuty}`)
                                    break
                                case 'off':
                                    if (p.duration)    lines.push(`${indent(base)}      duration_ms: ${p.duration}`)
                                    break
                                case 'flash':
                                    lines.push(`${indent(base)}      cycle_ms: ${p.interval ?? p.cycle ?? 500}`)
                                    if (p.duration)    lines.push(`${indent(base)}      duration_ms: ${p.duration}`)
                                    if (p.brightness !== undefined && p.brightness !== 100)
                                                       lines.push(`${indent(base)}      brightness: ${p.brightness}`)
                                    if (p.duty !== undefined && p.duty !== 50)
                                                       lines.push(`${indent(base)}      duty: ${p.duty}`)
                                    break
                                case 'fadein':
                                case 'fadeout':
                                    lines.push(`${indent(base)}      duration_ms: ${p.duration ?? 1000}`)
                                    if (p.brightness !== undefined && p.brightness !== 100)
                                                       lines.push(`${indent(base)}      brightness: ${p.brightness}`)
                                    break
                                case 'fading':
                                    lines.push(`${indent(base)}      cycle_ms: ${p.cycle ?? 2000}`)
                                    if (p.duration)    lines.push(`${indent(base)}      duration_ms: ${p.duration}`)
                                    if (p.min !== undefined) lines.push(`${indent(base)}      min_brightness: ${p.min}`)
                                    if (p.max !== undefined) lines.push(`${indent(base)}      max_brightness: ${p.max}`)
                                    break
                                case 'beacon':
                                    lines.push(`${indent(base)}      cycle_ms: ${p.cycle ?? 1200}`)
                                    if (p.duration)    lines.push(`${indent(base)}      duration_ms: ${p.duration}`)
                                    if (p.flashPct !== undefined) lines.push(`${indent(base)}      flash_pct: ${p.flashPct}`)
                                    if (p.max !== undefined) lines.push(`${indent(base)}      max_brightness: ${p.max}`)
                                    if (p.min !== undefined) lines.push(`${indent(base)}      min_brightness: ${p.min}`)
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
