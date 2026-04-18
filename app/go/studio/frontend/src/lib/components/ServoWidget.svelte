<!-- ScaleFX Studio — Shared Servo Widget -->
<!-- Reusable servo selector + position control + configuration panel.
     Used by GunFxTab, LightFxTab, GearControlTab. -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'

    /** Servo labels: array of {id, name} for the dropdown. */
    export let servos: { id: number; name: string }[] = []

    /** Whether controls are disabled (e.g. slave mode). */
    export let disabled: boolean = false

    /** Label for the section header. */
    export let label: string = 'Servos'

    /** Show section header. */
    export let showHeader: boolean = true

    // ─── Per-servo state tracked locally ───
    interface ServoState {
        pulse_us: number
        min_us: number
        max_us: number
        speed: number
        accel: number
        decel: number
        reversed: boolean
        status: 'idle' | 'moving' | 'unknown'
    }

    let states: Map<number, ServoState> = new Map()

    function getState(id: number): ServoState {
        if (!states.has(id)) {
            states.set(id, {
                pulse_us: 1500,
                min_us: 500,
                max_us: 2500,
                speed: 4000,
                accel: 8000,
                decel: 8000,
                reversed: false,
                status: 'unknown',
            })
        }
        return states.get(id)!
    }

    // Force reactivity on map edits
    function touch() { states = new Map(states) }

    // ─── Active servo selection ───
    let activeId: number = servos.length > 0 ? servos[0].id : 0
    $: if (servos.length > 0 && !servos.find(s => s.id === activeId)) {
        activeId = servos[0].id
    }
    $: active = getState(activeId)
    // Switching servos discards any pending live push from the old one and
    // resets the deduplication baseline so the next edit always sends.
    $: { void activeId; if (pushTimer) { clearTimeout(pushTimer); pushTimer = null } lastPushed = ''; pushStatus = '' }

    // ─── Live Push (debounced) ───
    // Validates the active servo's config locally; if valid, pushes to the
    // board automatically. Cancels an in-flight timer if the user keeps editing.
    const PUSH_DEBOUNCE_MS = 350
    let pushTimer: ReturnType<typeof setTimeout> | null = null
    let lastPushed: string = ''
    let pushStatus: '' | 'pending' | 'sent' | 'invalid' = ''

    function configError(s: ServoState): string | null {
        if (s.min_us < 300 || s.min_us > 2700) return `min ${s.min_us}µs out of [300,2700]`
        if (s.max_us < 300 || s.max_us > 2700) return `max ${s.max_us}µs out of [300,2700]`
        if (s.min_us >= s.max_us) return `min (${s.min_us}) ≥ max (${s.max_us})`
        if (s.speed < 0 || s.speed > 65535) return `speed ${s.speed} out of [0,65535]`
        if (s.accel < 0 || s.accel > 65535) return `accel ${s.accel} out of [0,65535]`
        if (s.decel < 0 || s.decel > 65535) return `decel ${s.decel} out of [0,65535]`
        return null
    }

    function configCommandFor(id: number, s: ServoState): string {
        let cmd = `servo.config ${id} ${s.min_us} ${s.max_us} ${s.speed} ${s.accel} ${s.decel}`
        if (s.reversed) cmd += ' rev'
        return cmd
    }

    function scheduleLivePush() {
        const id = activeId
        const s = getState(id)
        if (configError(s) !== null) { pushStatus = 'invalid'; return }
        const cmd = configCommandFor(id, s)
        if (cmd === lastPushed) return
        pushStatus = 'pending'
        if (pushTimer) clearTimeout(pushTimer)
        pushTimer = setTimeout(() => {
            pushTimer = null
            // Re-validate at fire time — state may have moved into invalid range.
            if (configError(s) !== null) { pushStatus = 'invalid'; return }
            const fresh = configCommandFor(id, s)
            SendCommand(fresh)
            lastPushed = fresh
            pushStatus = 'sent'
        }, PUSH_DEBOUNCE_MS)
    }

    // ─── Commands ───
    function setPosition() {
        const s = getState(activeId)
        SendCommand(`servo set ${activeId} ${s.pulse_us}`)
    }

    function applyConfig() {
        const s = getState(activeId)
        const cmd = configCommandFor(activeId, s)
        SendCommand(cmd)
        lastPushed = cmd
        pushStatus = 'sent'
    }

    // ─── Behavior summary (human-readable) ───
    function travelMs(s: ServoState): number {
        const range = Math.abs(s.max_us - s.min_us)
        if (s.speed <= 0) return 0
        return Math.round((range / s.speed) * 1000)
    }

    function summary(s: ServoState): string {
        const err = configError(s)
        if (err) return `⚠ Invalid: ${err}`
        const dir = s.reversed ? 'reversed (Open=Min, Close=Max)' : 'normal (Open=Max, Close=Min)'
        const span = `${s.min_us}–${s.max_us}µs (span ${s.max_us - s.min_us}µs)`
        const travel = travelMs(s)
        const profile = s.speed === 0
            ? 'instant motion (no rate limiting)'
            : `~${travel}ms full travel @ ${s.speed}µs/s, accel/decel ${s.accel}/${s.decel}µs/s²`
        return `Travel ${span}, ${dir}. ${profile}.`
    }

    function centerServo() {
        const s = getState(activeId)
        s.pulse_us = Math.round((s.min_us + s.max_us) / 2)
        touch()
        setPosition()
    }

    function minPos() {
        const s = getState(activeId)
        s.pulse_us = s.min_us
        touch()
        setPosition()
    }

    function maxPos() {
        const s = getState(activeId)
        s.pulse_us = s.max_us
        touch()
        setPosition()
    }

    function setMinHere() {
        const s = getState(activeId)
        s.min_us = s.pulse_us
        touch()
        applyConfig()
    }

    function setMaxHere() {
        const s = getState(activeId)
        s.max_us = s.pulse_us
        touch()
        applyConfig()
    }

    function resetDefaults() {
        const s = getState(activeId)
        s.min_us = 500
        s.max_us = 2500
        s.speed = 4000
        s.accel = 8000
        s.decel = 8000
        s.reversed = false
        touch()
        applyConfig()
    }

    // ─── Status color ───
    function statusColor(status: string): string {
        switch (status) {
            case 'idle': return 'var(--ok, #4ec9b0)'
            case 'moving': return 'var(--accent, #569cd6)'
            default: return 'var(--text-dim)'
        }
    }

    // Show/hide config panel — expanded by default
    let showConfig = true

    import { onDestroy } from 'svelte'
    onDestroy(() => { if (pushTimer) clearTimeout(pushTimer) })
</script>

<section class="servo-widget" class:disabled>
    {#if showHeader}
        <div class="widget-header">
            <h3>{label}</h3>
        </div>
    {/if}

    <!-- Servo selector + status -->
    <div class="form-row">
        <span class="flbl">Servo</span>
        <select bind:value={activeId} class="field-input" style="min-width: 130px" {disabled}>
            {#each servos as sv}
                <option value={sv.id}>{sv.id} — {sv.name}</option>
            {/each}
        </select>
        <span class="status-dot" style="background: {statusColor(active.status)}"
              title={active.status}></span>
        <span class="status-text">{active.status}</span>
    </div>

    <!-- Position slider -->
    <div class="slider-row">
        <span class="flbl" title="Live servo target. Drag the slider or type a µs value to move the servo.">Position</span>
        <input type="range" bind:value={active.pulse_us}
               on:change={() => touch()}
               min={active.min_us} max={active.max_us} step="1"
               class="slider wide" {disabled}
               title="Drag to position the servo. Range is constrained to [min, max] µs." />
        <input type="number" bind:value={active.pulse_us}
               on:change={() => touch()}
               class="field-input narrow" min="500" max="2500" step="10" {disabled}
               title="Pulse width in microseconds. 1500µs is typically center." />
        <span class="unit">µs</span>
        <button class="small primary" on:click={setPosition} {disabled}
                title="Send `servo set` to drive servo to current position">Set</button>
    </div>

    <!-- Quick positions -->
    <div class="quick-row">
        <button class="tiny" on:click={minPos} {disabled} title="Go to min">Min</button>
        <button class="tiny" on:click={centerServo} {disabled} title="Go to center">Center</button>
        <button class="tiny" on:click={maxPos} {disabled} title="Go to max">Max</button>
        <span class="spacer"></span>
        <button class="tiny" on:click={() => showConfig = !showConfig}
                class:toggled={showConfig}>
            {showConfig ? '▾ Config' : '▸ Config'}
        </button>
    </div>

    <!-- Config panel (expanded by default) -->
    {#if showConfig}
        <div class="config-panel">
            <div class="config-section">
                <span class="cfg-section-title"
                      title="Mechanical end-stops. The servo will refuse pulses outside [min, max] to prevent stalling against linkage limits.">Limits</span>
                <div class="config-grid">
                    <div class="cfg-field">
                        <span class="cfg-label"
                              title="Lower pulse-width limit (µs). Defines one mechanical end-stop. Direction toggle decides whether this is the open or closed position.">Min µs</span>
                        <input type="number" bind:value={active.min_us}
                               on:change={() => { touch(); scheduleLivePush() }}
                               class="field-input" min="300" max="2700" step="10" {disabled}
                               title="Minimum allowed servo pulse in microseconds (300–2700). Movement is clamped to this on the lower side." />
                    </div>
                    <div class="cfg-field">
                        <span class="cfg-label"
                              title="Upper pulse-width limit (µs). Defines the opposite mechanical end-stop. Must be greater than min.">Max µs</span>
                        <input type="number" bind:value={active.max_us}
                               on:change={() => { touch(); scheduleLivePush() }}
                               class="field-input" min="300" max="2700" step="10" {disabled}
                               title="Maximum allowed servo pulse in microseconds (300–2700). Must be > min." />
                    </div>
                    <div class="cfg-field cfg-actions-inline">
                        <button class="tiny set-btn" on:click={setMinHere} {disabled}
                                title="Capture the current Position value into Min — convenient for calibrating end-stops by hand.">↓ Set Min Here</button>
                        <button class="tiny set-btn" on:click={setMaxHere} {disabled}
                                title="Capture the current Position value into Max — convenient for calibrating end-stops by hand.">↑ Set Max Here</button>
                    </div>
                </div>
            </div>

            <div class="config-section">
                <span class="cfg-section-title"
                      title="Which physical pulse value corresponds to 'open' vs 'closed'. Flip if your linkage is mounted upside-down or fights you on deploy.">Direction</span>
                <div class="config-grid">
                    <div class="cfg-field">
                        <label class="cfg-toggle"
                               title="When reversed, Open commands drive to Min instead of Max. Use this to swap mechanical sense without re-wiring.">
                            <input type="checkbox" bind:checked={active.reversed}
                                   on:change={() => { touch(); scheduleLivePush() }} {disabled} />
                            <span class="cfg-label">Reversed</span>
                        </label>
                        <span class="cfg-hint">
                            {active.reversed ? 'Open = Min, Close = Max' : 'Open = Max, Close = Min'}
                        </span>
                    </div>
                </div>
            </div>

            <div class="config-section">
                <span class="cfg-section-title"
                      title="Trapezoidal motion profile. The servo ramps up at Accel, cruises at Speed, then ramps down at Decel. 0 means instant motion (no rate limiting).">Motion Profile</span>
                <div class="config-grid">
                    <div class="cfg-field">
                        <span class="cfg-label"
                              title="Cruise speed in microseconds per second. Higher = faster movement. 0 disables the profile and snaps instantly.">Speed <span class="cfg-unit">µs/sec</span></span>
                        <input type="number" bind:value={active.speed}
                               on:change={() => { touch(); scheduleLivePush() }}
                               class="field-input" min="0" max="65535" step="100" {disabled}
                               title="Cruise speed (µs/s). 4000 ≈ 0.5s travel across full 2000µs range. 0 = instant." />
                    </div>
                    <div class="cfg-field">
                        <span class="cfg-label"
                              title="How fast the servo ramps from rest to cruise speed (µs/s²). Higher = snappier start but more strain on linkage.">Accel <span class="cfg-unit">µs/sec²</span></span>
                        <input type="number" bind:value={active.accel}
                               on:change={() => { touch(); scheduleLivePush() }}
                               class="field-input" min="0" max="65535" step="100" {disabled}
                               title="Acceleration (µs/s²) — how quickly the servo reaches cruise speed. 0 = instant." />
                    </div>
                    <div class="cfg-field">
                        <span class="cfg-label"
                              title="How fast the servo ramps down from cruise speed to a stop (µs/s²). Higher = harder stop, less overshoot.">Decel <span class="cfg-unit">µs/sec²</span></span>
                        <input type="number" bind:value={active.decel}
                               on:change={() => { touch(); scheduleLivePush() }}
                               class="field-input" min="0" max="65535" step="100" {disabled}
                               title="Deceleration (µs/s²) — how quickly the servo slows to a stop. 0 = instant." />
                    </div>
                </div>
            </div>

            <!-- Behavior summary — describes the configured behavior in plain English. -->
            <div class="behavior-summary"
                 class:summary-invalid={pushStatus === 'invalid'}
                 title="Plain-English summary of how this servo is currently configured.">
                <span class="summary-label">Behavior</span>
                <span class="summary-text">{summary(active)}</span>
            </div>

            <div class="config-actions">
                <button class="small primary" on:click={applyConfig} {disabled}
                        title="Send `servo.config` now (forces a push even if values match the last live update)">Apply Config</button>
                <button class="small" on:click={resetDefaults} {disabled}
                        title="Reset to firmware defaults: 500–2500µs, speed 4000, accel/decel 8000, not reversed">Defaults</button>
                <span class="spacer"></span>
                <span class="push-status push-{pushStatus}"
                      title="Live-push status: edits are validated and sent automatically (~{PUSH_DEBOUNCE_MS}ms after last change).">
                    {#if pushStatus === 'pending'}● syncing…{:else if pushStatus === 'sent'}✓ synced{:else if pushStatus === 'invalid'}⚠ invalid — not sent{:else}—{/if}
                </span>
            </div>
        </div>
    {/if}
</section>

<style>
    .servo-widget {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 6px;
        padding: 12px 14px;
    }

    .servo-widget.disabled {
        opacity: 0.5;
        pointer-events: none;
    }

    .widget-header {
        margin-bottom: 10px;
        padding-bottom: 6px;
        border-bottom: 1px solid var(--border);
    }

    .widget-header h3 {
        font-size: 13px;
        font-weight: 600;
        color: var(--text-bright);
        text-transform: uppercase;
        letter-spacing: 0.5px;
    }

    /* ─── Rows ─── */
    .form-row, .slider-row, .quick-row {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 6px;
    }

    .quick-row { margin-bottom: 0; }

    .spacer { flex: 1; }

    /* ─── Status indicator ─── */
    .status-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        display: inline-block;
        flex-shrink: 0;
    }

    .status-text {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        text-transform: uppercase;
    }

    /* ─── Fields ─── */
    .flbl {
        font-size: 10px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
        flex-shrink: 0;
    }

    .field-input {
        background: var(--bg-input);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        font-family: var(--font-mono);
        font-size: 11px;
        padding: 3px 6px;
        width: 72px;
    }

    .field-input.narrow { width: 58px; }
    .field-input:focus { border-color: var(--border-focus); outline: none; }

    .unit {
        font-size: 10px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .slider { accent-color: var(--accent); }
    .slider.wide { flex: 1; min-width: 80px; }

    /* ─── Buttons ─── */
    button {
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-radius: 3px;
        color: var(--text);
        cursor: pointer;
        transition: background 0.1s, border-color 0.1s;
    }

    button:hover {
        background: color-mix(in srgb, var(--accent) 10%, var(--bg-raised));
        border-color: var(--accent);
    }

    .small { font-size: 11px; padding: 3px 10px; }
    .tiny  { font-size: 10px; padding: 2px 8px; }

    .primary {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
        border-color: var(--accent);
        color: var(--accent);
    }

    .toggled { color: var(--accent); border-color: var(--accent); }

    /* ─── Config panel ─── */
    .config-panel {
        margin-top: 8px;
        padding: 10px;
        background: var(--bg-raised);
        border: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
        border-radius: 4px;
    }

    .config-section {
        margin-bottom: 10px;
    }

    .config-section:last-of-type {
        margin-bottom: 0;
    }

    .cfg-section-title {
        display: block;
        font-size: 9px;
        font-weight: 700;
        text-transform: uppercase;
        letter-spacing: 0.5px;
        color: var(--text-dim);
        margin-bottom: 6px;
        padding-bottom: 3px;
        border-bottom: 1px solid color-mix(in srgb, var(--border) 30%, transparent);
    }

    .config-grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(100px, 1fr));
        gap: 8px;
    }

    .cfg-field {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .cfg-actions-inline {
        display: flex;
        flex-direction: column;
        gap: 4px;
        justify-content: flex-end;
    }

    .cfg-field-row {
        flex-direction: row;
        align-items: center;
        gap: 6px;
    }

    .toggle-label {
        display: flex;
        align-items: center;
        gap: 6px;
        cursor: pointer;
    }

    .toggle-label input[type="checkbox"] {
        accent-color: var(--accent);
        width: 14px;
        height: 14px;
        cursor: pointer;
    }

    .set-btn {
        font-size: 9px;
        padding: 2px 6px;
        color: var(--accent);
        border-color: color-mix(in srgb, var(--accent) 30%, transparent);
    }

    .cfg-label {
        font-size: 9px;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.3px;
    }

    .cfg-unit {
        font-weight: 400;
        font-size: 8px;
        text-transform: none;
        letter-spacing: 0;
        opacity: 0.7;
    }

    .config-actions {
        margin-top: 8px;
        display: flex;
        gap: 6px;
    }

    .cfg-toggle {
        display: flex;
        align-items: center;
        gap: 6px;
        cursor: pointer;
    }

    .cfg-toggle input[type="checkbox"] {
        accent-color: var(--accent);
        width: 14px;
        height: 14px;
        cursor: pointer;
    }

    .cfg-hint {
        font-size: 11px;
        color: var(--text-dim);
        font-style: italic;
        margin-top: 2px;
    }

    /* ─── Behavior summary ─── */
    .behavior-summary {
        margin-top: 10px;
        padding: 8px 10px;
        background: color-mix(in srgb, var(--accent) 7%, var(--bg-input));
        border: 1px solid color-mix(in srgb, var(--accent) 25%, transparent);
        border-left: 3px solid var(--accent);
        border-radius: 4px;
        display: flex;
        gap: 8px;
        align-items: baseline;
        font-size: 11px;
        line-height: 1.4;
    }
    .behavior-summary.summary-invalid {
        background: color-mix(in srgb, var(--warning) 10%, var(--bg-input));
        border-color: color-mix(in srgb, var(--warning) 40%, transparent);
        border-left-color: var(--warning);
        color: var(--warning);
    }
    .summary-label {
        font-size: 9px;
        font-weight: 700;
        text-transform: uppercase;
        letter-spacing: 0.4px;
        color: var(--text-dim);
        flex-shrink: 0;
    }
    .summary-text {
        color: var(--text);
        font-family: var(--font-mono);
    }

    /* ─── Live-push status ─── */
    .push-status {
        font-size: 10px;
        font-family: var(--font-mono);
        color: var(--text-dim);
        margin-left: auto;
        align-self: center;
    }
    .push-status.push-pending { color: var(--accent); }
    .push-status.push-sent    { color: var(--success); }
    .push-status.push-invalid { color: var(--warning); }
</style>
