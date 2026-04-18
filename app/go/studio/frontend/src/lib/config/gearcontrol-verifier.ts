// ScaleFX Studio — GearControl Configuration Verifier
//
// Board-specific verification rules for the GearControl pin mapping, per-gear
// config, door positions, and yaw steering. Mirrors the LightFX verifier
// pattern (see light-verifier.ts) — board UIs build a snapshot of their state
// once per reactive tick, hand it to verify(), and use hasConflict()/severityForPath()
// to drive .verify-error / .verify-warn class bindings.
//
// Rules implemented:
//   • pin-input-dup        — only one yaw_input pin allowed
//   • pin-yaw-output-dup   — only one yaw_output pin allowed
//   • pin-door-disabled    — door pin maps to a disabled gear channel
//   • pin-yaw-disabled     — yaw_output pin maps to a disabled gear channel
//   • pin-yaw-orphan       — yaw_input present without yaw_output (or vice versa)
//   • gear-uncalibrated    — enabled gear is not calibrated
//   • gear-no-doors        — enabled gear has no door servo pins assigned
//   • servo-range          — door/yaw min µs ≥ max µs
//   • servo-bounds         — servo µs outside PWM range [500,2500]
//   • yaw-neutral-bounds   — yaw neutral outside [min,max]
//
// gear_input is a fixed-function pin (GP0) — its enable / threshold live in
// GearInputConfig and are not validated as pin roles.

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

export type PinRole = 'door' | 'yaw_input' | 'yaw_output' | 'unused'

export interface GearPinConfig {
    role: PinRole
    /** For role='door': gear channel (0..2). For role='yaw_output': owning gear. */
    channel: number
    gear_id: number
    threshold_us: number
    /** Servo fields — used for role='door' and 'yaw_output'. */
    min_us: number
    max_us: number
    speed: number
    reversed: boolean
    /** Yaw output only. */
    neutral_us: number
}

export interface GearConfigState {
    enabled: boolean
    calibrated: boolean
    /** Raw flag — board reports it as configFlags & 0x80 over the wire. */
    timeout_ms: number
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

export interface GearInputConfig {
    enabled: boolean
    threshold_us: number
}

export interface GearControlConfig {
    pins: GearPinConfig[]
    gears: GearConfigState[]
    yaw: YawConfigState
    gearInput: GearInputConfig
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
        const yawInputTracker = new ResourceTracker('Yaw input pin')
        const yawOutputTracker = new ResourceTracker('Yaw output pin')

        for (let i = 0; i < config.pins.length; i++) {
            const p = config.pins[i]
            if (p.role === 'yaw_input') {
                yawInputTracker.claim('yaw_input', `pin${i + 1}`, `pins[${i}]`)
            } else if (p.role === 'yaw_output') {
                yawOutputTracker.claim('yaw_output', `pin${i + 1}`, `pins[${i}]`)
            }
        }
        issues.push(...yawInputTracker.conflicts('pin-input-dup'))
        issues.push(...yawOutputTracker.conflicts('pin-yaw-output-dup'))

        // ── 2. Gear input (fixed GP0) — threshold sanity ───────────────────
        if (config.gearInput.enabled &&
            (config.gearInput.threshold_us < 800 || config.gearInput.threshold_us > 2200)) {
            issues.push({
                id: 'gear-input-threshold',
                severity: 'warning',
                message: `Gear input threshold (${config.gearInput.threshold_us}µs) outside [800,2200]`,
                path: 'gearInput',
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
                        message: `SRV${i + 1}: door servo bound to disabled gear channel ${ch}`,
                        path: `pins[${i}]`,
                    })
                }
            } else if (p.role === 'yaw_output') {
                const gid = p.gear_id
                if (gid !== 0) {
                    issues.push({
                        id: 'pin-yaw-not-nose',
                        severity: 'error',
                        message: `SRV${i + 1}: yaw_output must bind to nose gear (gear 0), got gear ${gid}`,
                        path: `pins[${i}]`,
                    })
                } else if (!config.gears[0].enabled) {
                    issues.push({
                        id: 'pin-yaw-disabled',
                        severity: 'error',
                        message: `SRV${i + 1}: yaw_output bound to disabled nose gear`,
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
        }

        // ── 5b. Per-pin servo range/bounds (doors + yaw_output) ─────────────
        for (let i = 0; i < config.pins.length; i++) {
            const p = config.pins[i]
            if (p.role !== 'door' && p.role !== 'yaw_output') continue
            if (p.min_us >= p.max_us) {
                issues.push({
                    id: 'servo-range',
                    severity: 'error',
                    message: `SRV${i + 1}: servo min (${p.min_us}µs) ≥ max (${p.max_us}µs)`,
                    path: `pins[${i}]`,
                })
            }
            if (p.min_us < PWM_MIN_US || p.max_us > PWM_MAX_US) {
                issues.push({
                    id: 'servo-bounds',
                    severity: 'error',
                    message: `SRV${i + 1}: servo range [${p.min_us},${p.max_us}] outside [${PWM_MIN_US},${PWM_MAX_US}]`,
                    path: `pins[${i}]`,
                })
            }
        }

        // ── 6. Yaw neutral bounds (min/max checked per-pin above) ──────────
        if (config.yaw.enabled && config.yaw.min_us < config.yaw.max_us) {
            if (config.yaw.neutral_us < config.yaw.min_us || config.yaw.neutral_us > config.yaw.max_us) {
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
