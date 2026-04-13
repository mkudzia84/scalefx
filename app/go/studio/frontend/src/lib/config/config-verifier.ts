// ScaleFX Studio — Configuration Verification Framework
//
// Generic, board-agnostic verification engine. Tracks resource allocation
// (channels, servos, pins, etc.), detects conflicts, and produces a typed
// validation report that the SaveConfigDialog can render.
//
// Board-specific verifiers (LightFX, HubFX, etc.) extend this by calling
// the base helpers and adding their own rules.

// ─── Types ───

/** Severity of a verification finding. */
export type Severity = 'error' | 'warning' | 'info'

/** A single finding from verification. */
export interface VerifyIssue {
    /** Unique rule ID — stable across re-runs (e.g. 'band-overlap', 'channel-dup') */
    id: string
    /** Human-readable summary */
    message: string
    /** error = blocks save, warning = save allowed, info = informational */
    severity: Severity
    /** UI path for highlighting the source (e.g. 'programs[1].channels[3]') */
    path?: string
    /** Optional secondary path (the "other" side of a conflict) */
    conflictPath?: string
}

/** Aggregate verification result. */
export interface VerifyResult {
    /** True if zero errors (warnings are OK). */
    valid: boolean
    /** All findings, ordered by severity: errors first, then warnings, then info. */
    issues: VerifyIssue[]
    /** Counts by severity. */
    counts: Record<Severity, number>
    /** Timestamp of this verification run. */
    timestamp: number
}

// ─── Resource Tracker ───

/**
 * Tracks allocation of a named resource type (e.g. "LED Channel", "Servo ID",
 * "Input Channel"). When the same resource key is claimed by two owners, a
 * conflict issue is generated.
 */
export class ResourceTracker {
    readonly resourceName: string
    private _claims: Map<string, { owner: string; path: string }[]> = new Map()

    constructor(resourceName: string) {
        this.resourceName = resourceName
    }

    /** Claim a resource key. Owner is a human label; path is for UI highlight. */
    claim(key: string | number, owner: string, path: string): void {
        const k = String(key)
        if (!this._claims.has(k)) {
            this._claims.set(k, [])
        }
        this._claims.get(k)!.push({ owner, path })
    }

    /** Reset all claims (call before re-verification). */
    clear(): void {
        this._claims.clear()
    }

    /** Return issues for any resource claimed by more than one owner. */
    conflicts(ruleId: string): VerifyIssue[] {
        const issues: VerifyIssue[] = []
        for (const [key, owners] of this._claims) {
            if (owners.length > 1) {
                const verb = owners.length === 2 ? 'both' : 'all'
                const joinedNames = owners.length === 2
                    ? `${owners[0].owner} and ${owners[1].owner}`
                    : owners.map(o => o.owner).join(', ')
                issues.push({
                    id: ruleId,
                    severity: 'error',
                    message: `${joinedNames} ${verb} use ${this.resourceName.toLowerCase()} ${key} — each must be unique`,
                    path: owners[0].path,
                    conflictPath: owners[1].path,
                })
            }
        }
        return issues
    }
}

// ─── Range Helpers ───

export interface Range {
    min: number
    max: number
    label: string
    path: string
}

/**
 * Detect overlapping ranges.
 * Returns an issue for every pair where [a.min, a.max] overlaps [b.min, b.max].
 */
export function findRangeOverlaps(ruleId: string, ranges: Range[], resourceName: string): VerifyIssue[] {
    const issues: VerifyIssue[] = []
    for (let i = 0; i < ranges.length; i++) {
        for (let j = i + 1; j < ranges.length; j++) {
            const a = ranges[i], b = ranges[j]
            if (a.min <= b.max && a.max >= b.min) {
                issues.push({
                    id: ruleId,
                    severity: 'error',
                    message: `${resourceName} overlap: "${a.label}" [${a.min}–${a.max}] ∩ "${b.label}" [${b.min}–${b.max}]`,
                    path: a.path,
                    conflictPath: b.path,
                })
            }
        }
    }
    return issues
}

/**
 * Detect ranges that are too narrow (< minWidth).
 */
export function findNarrowRanges(ruleId: string, ranges: Range[], minWidth: number, resourceName: string): VerifyIssue[] {
    const issues: VerifyIssue[] = []
    for (const r of ranges) {
        const width = r.max - r.min
        if (width < minWidth) {
            issues.push({
                id: ruleId,
                severity: 'warning',
                message: `${resourceName} "${r.label}" is very narrow (${width}µs < ${minWidth}µs minimum)`,
                path: r.path,
            })
        }
    }
    return issues
}

/**
 * Detect inverted ranges (min > max).
 */
export function findInvertedRanges(ruleId: string, ranges: Range[], resourceName: string): VerifyIssue[] {
    const issues: VerifyIssue[] = []
    for (const r of ranges) {
        if (r.min > r.max) {
            issues.push({
                id: ruleId,
                severity: 'error',
                message: `${resourceName} "${r.label}" has inverted range: min (${r.min}) > max (${r.max})`,
                path: r.path,
            })
        }
    }
    return issues
}

/**
 * Detect gaps in the PWM range that no band covers.
 * Returns a warning for each uncovered interval within [globalMin, globalMax].
 */
export function findRangeGaps(
    ruleId: string, ranges: Range[], globalMin: number, globalMax: number, resourceName: string
): VerifyIssue[] {
    if (ranges.length === 0) return []

    // Sort by min, then max
    const sorted = [...ranges]
        .filter(r => r.min <= r.max) // skip inverted
        .sort((a, b) => a.min - b.min || a.max - b.max)

    const issues: VerifyIssue[] = []
    let covered = globalMin

    for (const r of sorted) {
        if (r.min > covered) {
            issues.push({
                id: ruleId,
                severity: 'info',
                message: `${resourceName} gap: range ${covered}–${r.min}µs is not mapped to any program`,
                path: r.path,
            })
        }
        covered = Math.max(covered, r.max)
    }

    if (covered < globalMax) {
        issues.push({
            id: ruleId,
            severity: 'info',
            message: `${resourceName} gap: range ${covered}–${globalMax}µs is not mapped to any program`,
        })
    }

    return issues
}

// ─── Result Builder ───

/** Collects issues and produces a sorted VerifyResult. */
export function buildResult(issues: VerifyIssue[]): VerifyResult {
    const sevOrder: Record<Severity, number> = { error: 0, warning: 1, info: 2 }
    const sorted = [...issues].sort((a, b) => sevOrder[a.severity] - sevOrder[b.severity])

    const counts: Record<Severity, number> = { error: 0, warning: 0, info: 0 }
    for (const i of sorted) counts[i.severity]++

    return {
        valid: counts.error === 0,
        issues: sorted,
        counts,
        timestamp: Date.now(),
    }
}

// ─── Generic Verifier Interface ───

/**
 * Board-specific verifiers implement this interface.
 * The framework calls `verify()` whenever config state changes, and
 * `hasConflict(path)` to check whether a specific UI element should
 * be highlighted.
 */
export interface ConfigVerifier<T> {
    /** Run full verification against the current config state. */
    verify(config: T): VerifyResult

    /** Quick check: does the latest result have an issue touching this path? */
    hasConflict(path: string): boolean

    /** Get all issues touching a specific path. */
    issuesForPath(path: string): VerifyIssue[]

    /** Most recent result (cached from last verify() call). */
    readonly lastResult: VerifyResult
}

/** Empty "always valid" result. */
export const EMPTY_RESULT: VerifyResult = {
    valid: true,
    issues: [],
    counts: { error: 0, warning: 0, info: 0 },
    timestamp: 0,
}
