<!-- ScaleFX Studio — Servo Profile Editor (Rule 42).
     Reusable, layout-only servo motion-profile editor.  Replaces the
     cramped 4-column grid that used to live inline inside
     PortRoleConfig.svelte for `RoleKind.ServoActuator`.

     Cribs the section/grid layout from the legacy ServoWidget in
     _archive/ — three labelled sections (Limits / Direction / Motion
     Profile), each with an auto-fit grid so fields breathe at any panel
     width — plus a plain-English behaviour summary at the bottom.

     Pure UI: the parent owns the live-push timer, GET/SET Wails calls
     and any draft vs. config bookkeeping.  This component:
       • two-way binds the profile prop,
       • fires `on:change` whenever a field changes,
       • surfaces a `pushStatus` chip provided by the parent.

     Field set matches the current ServoMotionProfile DTO (jerk replaces
     the legacy `decel` — `MotionProfile1D` uses symmetric accel + jerk
     for the S-curve shape).  See controllers/lib/sfx_peripherals/.
-->
<script lang="ts">
    import { createEventDispatcher } from 'svelte'

    export interface ServoProfile {
        minUs: number
        maxUs: number
        centerUs: number
        reversed: boolean
        maxSpeedUsPerSec: number
        maxAccelUsPerSec2: number
        maxJerkUsPerSec3: number
    }

    export let profile: ServoProfile
    export let disabled: boolean = false
    /** Optional live-push status chip — '' hides it. */
    export let pushStatus: '' | 'pending' | 'sent' | 'invalid' = ''
    /** Optional label above the editor; '' = no header. */
    export let label: string = ''

    const dispatch = createEventDispatcher<{ change: ServoProfile }>()

    // Bubble up the changed profile so the parent can debounce-push.
    function emit() { dispatch('change', profile) }

    // ── Validation (same shape as legacy widget; warns not blocks) ──
    function validate(p: ServoProfile): string | null {
        if (p.minUs < 300  || p.minUs > 2700) return `min ${p.minUs}µs out of [300,2700]`
        if (p.maxUs < 300  || p.maxUs > 2700) return `max ${p.maxUs}µs out of [300,2700]`
        if (p.minUs >= p.maxUs)               return `min (${p.minUs}) ≥ max (${p.maxUs})`
        if (p.centerUs < p.minUs || p.centerUs > p.maxUs)
            return `center ${p.centerUs}µs outside [${p.minUs},${p.maxUs}]`
        if (p.maxSpeedUsPerSec  < 0) return `speed must be ≥ 0`
        if (p.maxAccelUsPerSec2 < 0) return `accel must be ≥ 0`
        if (p.maxJerkUsPerSec3  < 0) return `jerk must be ≥ 0`
        return null
    }
    $: invalid = validate(profile)

    function travelMs(p: ServoProfile): number {
        const range = Math.abs(p.maxUs - p.minUs)
        if (p.maxSpeedUsPerSec <= 0) return 0
        return Math.round((range / p.maxSpeedUsPerSec) * 1000)
    }

    function summary(p: ServoProfile): string {
        const err = validate(p)
        if (err) return `⚠ Invalid: ${err}`
        const dir = p.reversed
            ? 'reversed (Open=Min, Close=Max)'
            : 'normal (Open=Max, Close=Min)'
        const span = `${p.minUs}–${p.maxUs}µs (center ${p.centerUs}µs, span ${p.maxUs - p.minUs}µs)`
        if (p.maxSpeedUsPerSec === 0) return `Travel ${span}, ${dir}. Instant motion (no rate limiting).`
        const travel = travelMs(p)
        const shape = p.maxJerkUsPerSec3 > 0 ? 'S-curve' : 'trapezoidal'
        return `Travel ${span}, ${dir}. ~${travel}ms full travel @ ${p.maxSpeedUsPerSec}µs/s (${shape}, accel ${p.maxAccelUsPerSec2}µs/s²${p.maxJerkUsPerSec3 > 0 ? `, jerk ${p.maxJerkUsPerSec3}µs/s³` : ''}).`
    }

    function centerHere(): void {
        profile.centerUs = Math.round((profile.minUs + profile.maxUs) / 2)
        emit()
    }
    function resetDefaults(): void {
        profile = {
            minUs: 1000, maxUs: 2000, centerUs: 1500, reversed: false,
            maxSpeedUsPerSec: 800, maxAccelUsPerSec2: 1600, maxJerkUsPerSec3: 0,
        }
        emit()
    }
</script>

<section class="servo-profile-editor" class:disabled>
    {#if label}
        <div class="editor-header">
            <span class="editor-label">{label}</span>
            <span class="rule-tag" title="Rule 42 — motion shape lives on the role, not the effect">Rule 42</span>
        </div>
    {/if}

    <div class="config-section">
        <span class="cfg-section-title"
              title="Mechanical end-stops + neutral. The servo refuses pulses outside [min, max]; center is where it goes on idle / RC failsafe.">Limits</span>
        <div class="config-grid">
            <div class="cfg-field">
                <span class="cfg-label"
                      title="Lower pulse-width limit (µs). One mechanical end-stop. Direction toggle decides whether this is the open or closed position.">Min <span class="cfg-unit">µs</span></span>
                <input type="number" class="field-input"
                       min="300" max="2700" step="10"
                       bind:value={profile.minUs}
                       on:change={emit} {disabled}
                       title="Minimum allowed servo pulse (300–2700µs)." />
            </div>
            <div class="cfg-field">
                <span class="cfg-label"
                      title="Upper pulse-width limit (µs). Opposite end-stop. Must be greater than min.">Max <span class="cfg-unit">µs</span></span>
                <input type="number" class="field-input"
                       min="300" max="2700" step="10"
                       bind:value={profile.maxUs}
                       on:change={emit} {disabled}
                       title="Maximum allowed servo pulse (300–2700µs). Must be > min." />
            </div>
            <div class="cfg-field">
                <span class="cfg-label"
                      title="Neutral / center pulse-width (µs). Where the servo rests on idle or RC failsafe.">Center <span class="cfg-unit">µs</span></span>
                <input type="number" class="field-input"
                       min="300" max="2700" step="10"
                       bind:value={profile.centerUs}
                       on:change={emit} {disabled}
                       title="Center pulse (must be within [min, max])." />
            </div>
            <div class="cfg-field cfg-actions-inline">
                <button class="small" type="button" on:click={centerHere} {disabled}
                        title="Set Center to (min + max) / 2.">↕ Mid</button>
            </div>
        </div>
    </div>

    <div class="config-section">
        <span class="cfg-section-title"
              title="Which physical pulse value corresponds to 'open' vs 'closed'. Flip if your linkage is mounted upside-down.">Direction</span>
        <div class="config-grid">
            <div class="cfg-field">
                <label class="cfg-toggle"
                       title="When reversed, Open commands drive to Min instead of Max. Swap sense without re-wiring.">
                    <input type="checkbox" bind:checked={profile.reversed}
                           on:change={emit} {disabled} />
                    <span class="cfg-label">Reversed</span>
                </label>
                <span class="cfg-hint">
                    {profile.reversed ? 'Open = Min, Close = Max' : 'Open = Max, Close = Min'}
                </span>
            </div>
        </div>
    </div>

    <div class="config-section">
        <span class="cfg-section-title"
              title="MotionProfile1D shape. Speed = cruise rate, Accel = ramp up/down rate. Set Jerk > 0 for a smooth S-curve; leave 0 for trapezoidal.">Motion Profile</span>
        <div class="config-grid">
            <div class="cfg-field">
                <span class="cfg-label"
                      title="Cruise speed in microseconds per second. 0 = instant motion (no rate limiting).">Max Speed <span class="cfg-unit">µs/s</span></span>
                <input type="number" class="field-input"
                       min="0" max="10000" step="50"
                       bind:value={profile.maxSpeedUsPerSec}
                       on:change={emit} {disabled}
                       title="Cruise speed (µs/s). 800 ≈ 1.25s for a 1000µs swing. 0 = instant." />
            </div>
            <div class="cfg-field">
                <span class="cfg-label"
                      title="How fast the servo ramps to / from cruise speed (µs/s²). Higher = snappier start, more strain.">Max Accel <span class="cfg-unit">µs/s²</span></span>
                <input type="number" class="field-input"
                       min="0" max="50000" step="100"
                       bind:value={profile.maxAccelUsPerSec2}
                       on:change={emit} {disabled}
                       title="Acceleration (µs/s²) — symmetric ramp up + down. 0 = instant." />
            </div>
            <div class="cfg-field">
                <span class="cfg-label"
                      title="Jerk = rate of change of acceleration (µs/s³). 0 keeps the profile trapezoidal; > 0 turns it into a smooth S-curve.">Max Jerk <span class="cfg-unit">µs/s³</span></span>
                <input type="number" class="field-input"
                       min="0" max="100000" step="100"
                       bind:value={profile.maxJerkUsPerSec3}
                       on:change={emit} {disabled}
                       title="Jerk (µs/s³). 0 = trapezoidal. > 0 = S-curve (smoother starts/stops)." />
                <span class="cfg-hint">{profile.maxJerkUsPerSec3 > 0 ? 'S-curve' : 'trapezoidal'}</span>
            </div>
        </div>
    </div>

    <!-- Plain-English summary — explains the configured behaviour at a
         glance.  Tinted yellow when validation fails. -->
    <div class="behavior-summary" class:summary-invalid={invalid !== null}
         title="Plain-English summary of how this servo is currently configured.">
        <span class="summary-label">Behavior</span>
        <span class="summary-text">{summary(profile)}</span>
    </div>

    <div class="editor-footer">
        <button class="small" type="button" on:click={resetDefaults} {disabled}
                title="Reset to firmware defaults: 1000–2000µs, center 1500, speed 800, accel 1600, jerk 0, not reversed.">Defaults</button>
        <span class="spacer"></span>
        {#if pushStatus}
            <span class="push-status push-{pushStatus}"
                  title="Live-push status: edits are validated and pushed automatically (~350ms after last change).">
                {#if pushStatus === 'pending'}● syncing…{:else if pushStatus === 'sent'}✓ synced{:else if pushStatus === 'invalid'}⚠ invalid — not sent{:else}—{/if}
            </span>
        {/if}
    </div>
</section>

<style>
    .servo-profile-editor { display: block; }
    .servo-profile-editor.disabled { opacity: 0.5; pointer-events: none; }

    .editor-header {
        display: flex; align-items: baseline; gap: 8px;
        margin-bottom: 8px; padding-bottom: 4px;
        border-bottom: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
    }
    .editor-label {
        font-size: 11px; font-weight: 600; color: var(--text-bright);
        text-transform: uppercase; letter-spacing: 0.4px;
    }
    .rule-tag {
        font-size: 9px; color: var(--text-dim); font-style: italic;
        margin-left: auto;
    }

    .config-section { margin-bottom: 10px; }
    .config-section:last-of-type { margin-bottom: 0; }

    .cfg-section-title {
        display: block;
        font-size: 9px; font-weight: 700;
        text-transform: uppercase; letter-spacing: 0.5px;
        color: var(--text-dim);
        margin-bottom: 6px; padding-bottom: 3px;
        border-bottom: 1px solid color-mix(in srgb, var(--border) 30%, transparent);
    }

    .config-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(110px, 1fr));
        gap: 8px;
    }

    .cfg-field { display: flex; flex-direction: column; gap: 2px; }
    .cfg-field .field-input { width: 100%; box-sizing: border-box; height: 24px; }

    .cfg-actions-inline { justify-content: flex-end; }
    .cfg-actions-inline .small { height: 24px; padding: 0 8px; font-size: 11px; }

    .cfg-label {
        font-size: 9px; color: var(--text-dim);
        text-transform: uppercase; letter-spacing: 0.3px;
    }
    .cfg-unit {
        font-weight: 400; font-size: 8px;
        text-transform: none; letter-spacing: 0;
        opacity: 0.7;
    }
    .cfg-hint {
        font-size: 10px; font-style: italic;
        color: var(--text-dim); margin-top: 1px;
    }

    .cfg-toggle { display: flex; align-items: center; gap: 6px; cursor: pointer; }
    .cfg-toggle input[type="checkbox"] {
        accent-color: var(--accent); width: 14px; height: 14px; cursor: pointer;
    }

    .behavior-summary {
        margin-top: 10px; padding: 8px 10px;
        background: color-mix(in srgb, var(--accent) 7%, var(--bg-input));
        border: 1px solid color-mix(in srgb, var(--accent) 25%, transparent);
        border-left: 3px solid var(--accent);
        border-radius: 4px;
        display: flex; gap: 8px; align-items: baseline;
        font-size: 11px; line-height: 1.4;
    }
    .behavior-summary.summary-invalid {
        background: color-mix(in srgb, var(--warning) 10%, var(--bg-input));
        border-color: color-mix(in srgb, var(--warning) 40%, transparent);
        border-left-color: var(--warning);
        color: var(--warning);
    }
    .summary-label {
        font-size: 9px; font-weight: 700;
        text-transform: uppercase; letter-spacing: 0.4px;
        color: var(--text-dim); flex-shrink: 0;
    }
    .summary-text { color: var(--text); font-family: var(--font-mono); }

    .editor-footer {
        margin-top: 8px; display: flex; align-items: center; gap: 8px;
    }
    .spacer { flex: 1; }
    .push-status {
        font-size: 10px; font-family: var(--font-mono);
        color: var(--text-dim);
    }
    .push-status.push-pending { color: var(--accent); }
    .push-status.push-sent    { color: var(--success, #4ec9b0); }
    .push-status.push-invalid { color: var(--warning); }
</style>
