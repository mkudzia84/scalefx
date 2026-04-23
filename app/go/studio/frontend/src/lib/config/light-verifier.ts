// ScaleFX Studio — LightFX Configuration Verifier
//
// Board-specific verification rules for the LightFX/LightSetup config.
// Also reusable by HubFX when it embeds 6-channel light config.
//
// Rules implemented:
//   • band-overlap       — PWM input bands must not overlap
//   • band-inverted      — band min must be ≤ max
//   • band-narrow        — bands narrower than 50µs are hard to hit
//   • band-gap           — missing coverage in the PWM range
//   • group-servo-dup    — two bindings using the same servo
//   • group-servo-range  — binding servo min_us ≥ max_us
//   • group-gear-no-hub  — "gear" policy used without HubFX/slave connection
//   • group-missing      — channel references a non-existent group
//   • event-empty        — enabled channel in events mode with zero events
//   • event-infinite     — sequence that never loops (single event with duration=0)

import {
    type ConfigVerifier,
    type Severity,
    type VerifyResult,
    type VerifyIssue,
    type Range,
    EMPTY_RESULT,
    ResourceTracker,
    buildResult,
    findRangeOverlaps,
    findNarrowRanges,
    findInvertedRanges,
    findRangeGaps,
} from './config-verifier'

// ─── LightFX Config Shape ───
// Mirrors the in-component state from LightFxTab / LightSetup.
// The verifier works against this interface so it's decoupled from Svelte.

export interface LightSeqEvent {
    id: number
    type: string
    params: Record<string, number>
}

export type GroupPolicy = 'on' | 'off' | 'gear'

export interface LightChannelState {
    enabled: boolean
    mode: 'events' | 'group'
    groupIndex: number  // which group this channel belongs to (when mode is 'group')
    events: LightSeqEvent[]
    /**
     * Which board this channel-def targets. 'lightfx' (default) drives the
     * 8-channel LightFX slave; 'hubfx' drives the 6 local LED channels on the
     * HubFX master. The same /lightfx.yaml is loaded by both boards — each
     * filters out channel-defs that don't belong to it (Rule 26 single
     * canonical light-program file).
     */
    board: 'lightfx' | 'hubfx'
}

export interface LightProgram {
    name: string
    bandMin_us: number
    bandMax_us: number
    channels: LightChannelState[]
    groupPolicies: GroupPolicy[]   // one per group, indexed by group index
}

/** A servo binding within a landing group. */
export interface LightServoBinding {
    servo: number       // servo id (1-3)
    // Servo configuration
    servoMin_us: number
    servoMax_us: number
    servoSpeed: number
    servoAccel: number
    servoDecel: number
    servoReversed: boolean
}

/** A named landing group with its own servo bindings. */
export interface LightGroup {
    name: string
    /** 1-based LightFX channel numbers (1–8) wired to this group's LEDs. */
    memberChannels: number[]
    /** 1-based HubFX-local channel numbers (1–6) wired to this group's LEDs. */
    hubfxMemberChannels: number[]
    /** Which board's servo drives this group's deploy/retract motion. */
    servoBoard: 'lightfx' | 'hubfx'
    servoBindings: LightServoBinding[]
}

/** Battery monitor / soft-cutoff configuration. */
export interface LightBattery {
    /** Disable all LED channels when pack drops below the chemistry low threshold. */
    autoCutoff: boolean
    /** Battery chemistry — must be one of 'lipo' | 'liion' | 'nimh'. */
    chemistry: string
    /** Series cell count (1–6). 0 = auto-detect from voltage on connect. */
    cellCount: number
}

/** The full config snapshot that the verifier operates on. */
export interface LightConfig {
    programs: LightProgram[]
    /** Landing groups, each with its own servo bindings. */
    groups: LightGroup[]
    masterBrightness: number
    /** Battery monitor + soft-cutoff config. */
    battery: LightBattery
    /** Total LED channels on this board (8 for LightFX, 6 for HubFX embedded). */
    channelCount: number
    /** PWM range boundaries [globalMin, globalMax] — typically [1000, 2000]. */
    pwmMin: number
    pwmMax: number
    /** Whether HubFX or slave mode is active (enables "gear" group policy). */
    hubAvailable: boolean
}

const SUPPORTED_CHEMISTRIES = ['lipo', 'liion', 'nimh']

// ─── Implementation ───

export class LightConfigVerifier implements ConfigVerifier<LightConfig> {
    private _lastResult: VerifyResult = EMPTY_RESULT
    private _pathIndex: Map<string, VerifyIssue[]> = new Map()

    get lastResult(): VerifyResult { return this._lastResult }

    verify(config: LightConfig): VerifyResult {
        const issues: VerifyIssue[] = []

        // ── 1. Input band verification ──
        const bands: Range[] = config.programs.map((p, i) => ({
            min: p.bandMin_us,
            max: p.bandMax_us,
            label: p.name || `Program ${i + 1}`,
            path: `programs[${i}].band`,
        }))

        issues.push(...findInvertedRanges('band-inverted', bands, 'Input band'))
        issues.push(...findRangeOverlaps('band-overlap', bands, 'Input band'))
        issues.push(...findNarrowRanges('band-narrow', bands, 50, 'Input band'))
        issues.push(...findRangeGaps('band-gap', bands, config.pwmMin, config.pwmMax, 'Input band'))

        // ── 2. Landing Groups — servo conflicts (across all groups) ──
        const servoTracker = new ResourceTracker('Group servo')

        for (let gi = 0; gi < config.groups.length; gi++) {
            const group = config.groups[gi]
            const groupLabel = group.name || `Group ${gi + 1}`
            for (let bi = 0; bi < group.servoBindings.length; bi++) {
                const b = group.servoBindings[bi]
                const label = `${groupLabel} binding ${bi + 1}`
                servoTracker.claim(b.servo, label, `groups[${gi}].bindings[${bi}].servo`)
            }
        }
        issues.push(...servoTracker.conflicts('group-servo-dup'))

        // ── 3. Landing Groups — servo range verification ──
        for (let gi = 0; gi < config.groups.length; gi++) {
            const group = config.groups[gi]
            const groupLabel = group.name || `Group ${gi + 1}`
            for (let bi = 0; bi < group.servoBindings.length; bi++) {
                const b = group.servoBindings[bi]
                if (b.servoMin_us >= b.servoMax_us) {
                    issues.push({
                        id: 'group-servo-range',
                        severity: 'error',
                        message: `${groupLabel} binding ${bi + 1} servo ${b.servo}: min (${b.servoMin_us}µs) ≥ max (${b.servoMax_us}µs)`,
                        path: `groups[${gi}].bindings[${bi}]`,
                    })
                }
            }
        }

        // ── 4. Landing Groups — "gear" policy without HubFX ──
        for (let pi = 0; pi < config.programs.length; pi++) {
            const prog = config.programs[pi]
            const progLabel = prog.name || `Program ${pi + 1}`
            for (let gi = 0; gi < prog.groupPolicies.length; gi++) {
                if (prog.groupPolicies[gi] === 'gear' && !config.hubAvailable) {
                    const groupLabel = config.groups[gi]?.name || `Group ${gi + 1}`
                    issues.push({
                        id: 'group-gear-no-hub',
                        severity: 'warning',
                        message: `Program "${progLabel}" uses "Gear" policy for "${groupLabel}" but HubFX/slave mode is not active`,
                        path: `programs[${pi}].groupPolicies[${gi}]`,
                    })
                }
            }
        }

        // ── 5. Channel group reference check ──
        for (let pi = 0; pi < config.programs.length; pi++) {
            const prog = config.programs[pi]
            for (let ci = 0; ci < prog.channels.length; ci++) {
                const ch = prog.channels[ci]
                if (!ch.enabled || ch.mode !== 'group') continue
                if (ch.groupIndex < 0 || ch.groupIndex >= config.groups.length) {
                    issues.push({
                        id: 'group-missing',
                        severity: 'error',
                        message: `Channel ${ci + 1} in "${prog.name}" references non-existent group`,
                        path: `programs[${pi}].channels[${ci}]`,
                    })
                }
            }
        }

        // ── 6. Per-program channel verification ──
        for (let pi = 0; pi < config.programs.length; pi++) {
            const prog = config.programs[pi]
            const progLabel = prog.name || `Program ${pi + 1}`

            for (let ci = 0; ci < prog.channels.length; ci++) {
                const ch = prog.channels[ci]
                const chPath = `programs[${pi}].channels[${ci}]`

                if (!ch.enabled) continue

                if (ch.mode === 'events') {
                    // Empty events — nothing will happen
                    if (ch.events.length === 0) {
                        issues.push({
                            id: 'event-empty',
                            severity: 'warning',
                            message: `Channel ${ci + 1} in program "${progLabel}" is enabled but has no events — it will do nothing`,
                            path: chPath,
                        })
                    }

                    // Single non-looping event (duration=0 means infinite)
                    if (ch.events.length === 1 && ch.events[0].params.duration === 0 &&
                        ch.events[0].type !== 'fading' && ch.events[0].type !== 'beacon' &&
                        ch.events[0].type !== 'flash') {
                        const evtType = ch.events[0].type.charAt(0).toUpperCase() + ch.events[0].type.slice(1)
                        issues.push({
                            id: 'event-infinite',
                            severity: 'info',
                            message: `Channel ${ci + 1} in "${progLabel}" has a single ${evtType} event with infinite duration — the sequence will not loop`,
                            path: `${chPath}.events[0]`,
                        })
                    }
                }
            }
        }

        // ── 7. Master brightness bounds ──
        if (config.masterBrightness < 0 || config.masterBrightness > 100) {
            issues.push({
                id: 'brightness-range',
                severity: 'error',
                message: `Master brightness out of range: ${config.masterBrightness} (must be 0–100)`,
                path: 'masterBrightness',
            })
        }

        // ── 8. Battery config ──
        const bat = config.battery
        if (bat) {
            const chem = (bat.chemistry || '').toLowerCase()
            if (!SUPPORTED_CHEMISTRIES.includes(chem)) {
                issues.push({
                    id: 'battery-chemistry',
                    severity: 'error',
                    message: `Battery chemistry "${bat.chemistry}" is not supported (use lipo, liion, or nimh)`,
                    path: 'battery.chemistry',
                })
            }
            if (!Number.isInteger(bat.cellCount) || bat.cellCount < 0 || bat.cellCount > 6) {
                issues.push({
                    id: 'battery-cells',
                    severity: 'error',
                    message: `Battery cell count must be 0 (auto) or 1–6, got ${bat.cellCount}`,
                    path: 'battery.cellCount',
                })
            }
            if (bat.autoCutoff && bat.cellCount === 0) {
                issues.push({
                    id: 'battery-cutoff-auto',
                    severity: 'info',
                    message: 'Auto-cutoff is armed but cell count is auto-detect — cutoff threshold will track the detected pack size on connect',
                    path: 'battery.autoCutoff',
                })
            }
        }

        // Build and cache result
        this._lastResult = buildResult(issues)
        this._rebuildPathIndex()
        return this._lastResult
    }

    hasConflict(path: string): boolean {
        return this._pathIndex.has(path)
    }

    /** Return the highest severity issue affecting this path, or null if clean. */
    severityForPath(path: string): Severity | null {
        const issues = this._pathIndex.get(path)
        if (!issues || issues.length === 0) return null
        if (issues.some(i => i.severity === 'error')) return 'error'
        if (issues.some(i => i.severity === 'warning')) return 'warning'
        return 'info'
    }

    issuesForPath(path: string): VerifyIssue[] {
        return this._pathIndex.get(path) ?? []
    }

    /** Rebuild the path → issues index for quick lookups. */
    private _rebuildPathIndex(): void {
        this._pathIndex.clear()
        for (const issue of this._lastResult.issues) {
            if (issue.path) {
                if (!this._pathIndex.has(issue.path)) this._pathIndex.set(issue.path, [])
                this._pathIndex.get(issue.path)!.push(issue)
            }
            if (issue.conflictPath) {
                if (!this._pathIndex.has(issue.conflictPath)) this._pathIndex.set(issue.conflictPath, [])
                this._pathIndex.get(issue.conflictPath)!.push(issue)
            }
        }
    }
}
