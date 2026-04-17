// ScaleFX Studio — GearControl Configuration Verifier
//
// Board-specific verification rules for the GearControl pin mapping, per-gear
// config, door positions, and yaw steering. Mirrors the LightFX verifier
// pattern (see light-verifier.ts) — board UIs build a snapshot of their state
// once per reactive tick, hand it to verify(), and use hasConflict()/severityForPath()
// to drive .verify-error / .verify-warn class bindings.
//
// Rules implemented:
//   • pin-input-dup        — only one gear_input / yaw_input pin allowed
//   • pin-yaw-output-dup   — only one yaw_output pin allowed
//   • pin-door-disabled    — door pin maps to a disabled gear channel
//   • pin-yaw-disabled     — yaw_output pin maps to a disabled gear channel
//   • pin-yaw-orphan       — yaw_input present without yaw_output (or vice versa)
//   • pin-no-input         — direct mode without a gear_input pin
//   • gear-uncalibrated    — enabled gear is not calibrated
//   • gear-no-doors        — enabled gear has no door servo pins assigned
//   • door-range           — door open µs == close µs
//   • door-bounds          — door µs outside servo PWM range [500,2500]
//   • yaw-range            — yaw min ≥ max
//   • yaw-neutral-bounds   — yaw neutral outside [min,max]

import {
    type ConfigVerifier,
    type Severity,
    type VerifyResult,
    type VerifyIssue,
    EMPTY_RESULT,
    ResourceTracker,
    buildResult,
} from './config-verifier'

// ─── Config Shape ───
// Mirrors the in-component state from GearControlTab. The verifier works
// against this interface so it's decoupled from Svelte.

export type PinRole = 'door' | 'gear_input' | 'yaw_input' | 'yaw_output' | 'unused'

export interface GearPinConfig {
    role: PinRole
    /** For role='door': gear channel (0..2). For role='yaw_output': owning gear. */
    channel: number
    gear_id: number
    threshold_us: number
}

export interface GearConfigState {
    enabled: boolean
    calibrated: boolean
    /** Raw flag — board reports it as configFlags & 0x80 over the wire. */
    timeout_ms: number
    /** [open0, close0, open1, close1] in µs. */
    door: { open0_us: number; close0_us: number; open1_us: number; close1_us: number }
    /** Number of door pins assigned to this gear in pinConfigs. */
    doorPinCount: number
}

export interface YawConfigState {
    /** True if any pin has role='yaw_output'. */
    enabled: boolean
    gearId: number
    neutral_us: number
    min_us: number
    max_us: number
}

export interface GearControlConfig {
    pins: GearPinConfig[]
    gears: GearConfigState[]
    yaw: YawConfigState
    /** Connection mode: true → board talks to HubFX; pin inputs are unused. */
    isSlave: boolean
}

// PWM physical bounds for the boards we target. Anything outside is flagged.
const PWM_MIN_US = 500
const PWM_MAX_US = 2500

// ─── Implementation ───

export class GearControlConfigVerifier implements ConfigVerifier<GearControlConfig> {
    private _lastResult: VerifyResult = EMPTY_RESULT
    private _pathIndex: Map<string, VerifyIssue[]> = new Map()

    get lastResult(): VerifyResult { return this._lastResult }

    verify(config: GearControlConfig): VerifyResult {
        const issues: VerifyIssue[] = []

        // ── 1. Pin-role uniqueness ─────────────────────────────────────────
        const inputTracker = new ResourceTracker('Gear input pin')
        const yawInputTracker = new ResourceTracker('Yaw input pin')
        const yawOutputTracker = new ResourceTracker('Yaw output pin')

        for (let i = 0; i < config.pins.length; i++) {
            const p = config.pins[i]
            if (p.role === 'gear_input') {
                inputTracker.claim('gear_input', `pin${i + 1}`, `pins[${i}]`)
            } else if (p.role === 'yaw_input') {
                yawInputTracker.claim('yaw_input', `pin${i + 1}`, `pins[${i}]`)
            } else if (p.role === 'yaw_output') {
                yawOutputTracker.claim('yaw_output', `pin${i + 1}`, `pins[${i}]`)
            }
        }
        issues.push(...inputTracker.conflicts('pin-input-dup'))
        issues.push(...yawInputTracker.conflicts('pin-input-dup'))
        issues.push(...yawOutputTracker.conflicts('pin-yaw-output-dup'))

        // ── 2. Direct mode requires a gear_input pin ───────────────────────
        const hasGearInput = config.pins.some(p => p.role === 'gear_input')
        if (!config.isSlave && !hasGearInput) {
            issues.push({
                id: 'pin-no-input',
                severity: 'warning',
                message: 'Direct mode: no gear_input pin assigned — board cannot read deploy/retract command',
            })
        }

        // ── 3. Yaw input/output orphaning ──────────────────────────────────
        const hasYawIn = config.pins.some(p => p.role === 'yaw_input')
        const hasYawOut = config.pins.some(p => p.role === 'yaw_output')
        if (hasYawIn && !hasYawOut) {
            issues.push({
                id: 'pin-yaw-orphan',
                severity: 'warning',
                message: 'yaw_input pin assigned but no yaw_output — input will be read but never drive a servo',
            })
        }
        if (hasYawOut && !hasYawIn && !config.isSlave) {
            issues.push({
                id: 'pin-yaw-orphan',
                severity: 'warning',
                message: 'yaw_output pin assigned but no yaw_input — yaw will hold its neutral position',
            })
        }

        // ── 4. Pin maps to disabled gear ───────────────────────────────────
        for (let i = 0; i < config.pins.length; i++) {
            const p = config.pins[i]
            if (p.role === 'door') {
                const ch = p.channel
                if (ch < 0 || ch >= config.gears.length || !config.gears[ch].enabled) {
                    issues.push({
                        id: 'pin-door-disabled',
                        severity: 'error',
                        message: `Pin ${i + 1}: door servo bound to disabled gear channel ${ch}`,
                        path: `pins[${i}]`,
                    })
                }
            } else if (p.role === 'yaw_output') {
                const gid = p.gear_id
                if (gid < 0 || gid >= config.gears.length || !config.gears[gid].enabled) {
                    issues.push({
                        id: 'pin-yaw-disabled',
                        severity: 'error',
                        message: `Pin ${i + 1}: yaw_output bound to disabled gear ${gid}`,
                        path: `pins[${i}]`,
                    })
                }
            }
        }

        // ── 5. Per-gear sanity (only enabled gears) ────────────────────────
        for (let gi = 0; gi < config.gears.length; gi++) {
            const g = config.gears[gi]
            if (!g.enabled) continue

            if (!g.calibrated) {
                issues.push({
                    id: 'gear-uncalibrated',
                    severity: 'warning',
                    message: `Gear ${gi} is enabled but not calibrated — deploy/retract is blocked`,
                    path: `gears[${gi}]`,
                })
            }

            if (g.doorPinCount === 0) {
                issues.push({
                    id: 'gear-no-doors',
                    severity: 'info',
                    message: `Gear ${gi} has no door servo pins assigned`,
                    path: `gears[${gi}]`,
                })
            }

            // Door range checks — only the legs that actually have pins.
            if (g.doorPinCount >= 1) {
                if (g.door.open0_us === g.door.close0_us) {
                    issues.push({
                        id: 'door-range',
                        severity: 'error',
                        message: `Gear ${gi} Door A: open µs equals close µs (${g.door.open0_us})`,
                        path: `gears[${gi}].door.0`,
                    })
                }
                for (const v of [g.door.open0_us, g.door.close0_us]) {
                    if (v < PWM_MIN_US || v > PWM_MAX_US) {
                        issues.push({
                            id: 'door-bounds',
                            severity: 'error',
                            message: `Gear ${gi} Door A µs ${v} outside servo range [${PWM_MIN_US},${PWM_MAX_US}]`,
                            path: `gears[${gi}].door.0`,
                        })
                        break
                    }
                }
            }
            if (g.doorPinCount >= 2) {
                if (g.door.open1_us === g.door.close1_us) {
                    issues.push({
                        id: 'door-range',
                        severity: 'error',
                        message: `Gear ${gi} Door B: open µs equals close µs (${g.door.open1_us})`,
                        path: `gears[${gi}].door.1`,
                    })
                }
                for (const v of [g.door.open1_us, g.door.close1_us]) {
                    if (v < PWM_MIN_US || v > PWM_MAX_US) {
                        issues.push({
                            id: 'door-bounds',
                            severity: 'error',
                            message: `Gear ${gi} Door B µs ${v} outside servo range [${PWM_MIN_US},${PWM_MAX_US}]`,
                            path: `gears[${gi}].door.1`,
                        })
                        break
                    }
                }
            }
        }

        // ── 6. Yaw range/neutral ───────────────────────────────────────────
        if (config.yaw.enabled) {
            if (config.yaw.min_us >= config.yaw.max_us) {
                issues.push({
                    id: 'yaw-range',
                    severity: 'error',
                    message: `Yaw min (${config.yaw.min_us}µs) ≥ max (${config.yaw.max_us}µs)`,
                    path: 'yaw',
                })
            } else if (config.yaw.neutral_us < config.yaw.min_us || config.yaw.neutral_us > config.yaw.max_us) {
                issues.push({
                    id: 'yaw-neutral-bounds',
                    severity: 'warning',
                    message: `Yaw neutral (${config.yaw.neutral_us}µs) outside [${config.yaw.min_us},${config.yaw.max_us}]`,
                    path: 'yaw',
                })
            }
        }

        this._lastResult = buildResult(issues)
        this._rebuildPathIndex()
        return this._lastResult
    }

    hasConflict(path: string): boolean {
        return this._pathIndex.has(path)
    }

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
