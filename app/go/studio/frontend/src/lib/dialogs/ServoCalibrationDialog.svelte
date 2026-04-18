<!-- ScaleFX Studio — Interactive Servo Calibration Dialog -->
<!-- Generic across boards: drives `servo set <id> <us>` live (no debounce)   -->
<!-- for position jogs and debounced `servo.config <id> ...` for param edits. -->
<!-- Save → onApply(cfg); Cancel → restores the pre-open config on the board. -->
<script lang="ts">
    import { SendCommand } from '../../../wailsjs/go/main/App'
    import { onDestroy } from 'svelte'

    export let open: boolean = false

    export let servoId: number = 0
    export let servoName: string = 'Servo'

    export let min_us: number = 500
    export let max_us: number = 2500
    export let speed: number = 4000
    export let accel: number = 0
    export let decel: number = 0
    export let reversed: boolean = false

    /** Show accel/decel rows. Set false for boards that don't expose them (GearControl door/yaw use 0/0). */
    export let supportsAccelDecel: boolean = false

    interface CalibResult {
        min_us: number; max_us: number; speed: number
        accel: number; decel: number; reversed: boolean
    }
    export let onApply: ((cfg: CalibResult) => void) | null = null
    export let onClose: (() => void) | null = null

    // ─── Internal editable state (mirrors props, gets mutated in the dialog) ───
    let _min = min_us
    let _max = max_us
    let _speed = speed
    let _accel = accel
    let _decel = decel
    let _rev = reversed
    let position_us = Math.round((min_us + max_us) / 2)

    // Snapshot of the pre-open config — restored on the board if the user cancels.
    let _origMin = min_us, _origMax = max_us, _origSpeed = speed
    let _origAccel = accel, _origDecel = decel, _origRev = reversed

    // Absolute PWM bounds the board will accept. While the dialog is open we
    // push THESE as the active limits so `servo set` isn't clamped to the
    // user's (possibly narrow) target range — otherwise the user couldn't jog
    // past their existing min/max to probe the real physical end-stops.
    const PWM_MIN = 500
    const PWM_MAX = 2500

    $: if (open) initOnOpen()

    function initOnOpen() {
        _min = min_us; _max = max_us; _speed = speed
        _accel = accel; _decel = decel; _rev = reversed
        _origMin = min_us; _origMax = max_us; _origSpeed = speed
        _origAccel = accel; _origDecel = decel; _origRev = reversed
        position_us = Math.round((min_us + max_us) / 2)
        lastJog = 0
        lastCfgPushed = ''
        cfgStatus = ''
        // Widen the board's active limits so jogs during calibration aren't
        // clamped to the user's existing min/max (srv_control.setTarget clamps
        // to [_minUs, _maxUs]). Save will push the user's chosen bounds back.
        pushWidenedNow()
        jogNow(position_us)
    }

    function pushWidenedNow() {
        if (_speed < 0 || _speed > 65535) { cfgStatus = 'invalid'; return }
        const cmd = `servo.config ${servoId} ${PWM_MIN} ${PWM_MAX} ${_speed} ${_accel} ${_decel}${_rev ? ' rev' : ''}`
        SendCommand(cmd)
        lastCfgPushed = cmd
        cfgStatus = 'sent'
    }

    // ─── Live position jog (throttled, not debounced) ────────────────────
    const JOG_THROTTLE_MS = 30
    let lastJog = 0
    let jogTimer: ReturnType<typeof setTimeout> | null = null

    function jogNow(us: number) {
        const t = Date.now()
        if (t - lastJog >= JOG_THROTTLE_MS) {
            lastJog = t
            SendCommand(`servo set ${servoId} ${Math.round(us)}`)
            if (jogTimer) { clearTimeout(jogTimer); jogTimer = null }
        } else {
            if (jogTimer) clearTimeout(jogTimer)
            jogTimer = setTimeout(() => { lastJog = Date.now(); SendCommand(`servo set ${servoId} ${Math.round(us)}`); jogTimer = null }, JOG_THROTTLE_MS)
        }
    }

    function onPositionInput() { jogNow(position_us) }
    function jogToMin() { position_us = _min; jogNow(_min) }
    function jogToMax() { position_us = _max; jogNow(_max) }
    function jogToCenter() { const c = Math.round((_min + _max) / 2); position_us = c; jogNow(c) }

    function captureMin() { _min = Math.round(position_us); pushConfig() }
    function captureMax() { _max = Math.round(position_us); pushConfig() }

    // ─── Debounced config push ────────────────────────────────────────────
    const CFG_DEBOUNCE_MS = 350
    let cfgTimer: ReturnType<typeof setTimeout> | null = null
    let lastCfgPushed = ''
    let cfgStatus: '' | 'pending' | 'sent' | 'invalid' = ''

    function configError(): string | null {
        if (_min < 300 || _min > 2700) return `min ${_min}µs out of [300,2700]`
        if (_max < 300 || _max > 2700) return `max ${_max}µs out of [300,2700]`
        if (_min >= _max) return `min (${_min}) ≥ max (${_max})`
        if (_speed < 0 || _speed > 65535) return `speed ${_speed} out of [0,65535]`
        if (_accel < 0 || _accel > 65535) return `accel ${_accel} out of [0,65535]`
        if (_decel < 0 || _decel > 65535) return `decel ${_decel} out of [0,65535]`
        return null
    }

    // While the dialog is open, intermediate pushes use the WIDENED PWM range
    // (not _min/_max) so jogs stay unclamped. Only Save commits the narrow range.
    function cfgCommandWidened(): string {
        let cmd = `servo.config ${servoId} ${PWM_MIN} ${PWM_MAX} ${_speed} ${_accel} ${_decel}`
        if (_rev) cmd += ' rev'
        return cmd
    }
    function cfgCommandFinal(): string {
        let cmd = `servo.config ${servoId} ${_min} ${_max} ${_speed} ${_accel} ${_decel}`
        if (_rev) cmd += ' rev'
        return cmd
    }

    function pushConfig() {
        if (configError() !== null) { cfgStatus = 'invalid'; return }
        const cmd = cfgCommandWidened()
        if (cmd === lastCfgPushed) { cfgStatus = 'sent'; return }
        cfgStatus = 'pending'
        if (cfgTimer) { clearTimeout(cfgTimer); cfgTimer = null }
        cfgTimer = setTimeout(() => {
            cfgTimer = null
            if (configError() !== null) { cfgStatus = 'invalid'; return }
            const fresh = cfgCommandWidened()
            if (fresh === lastCfgPushed) { cfgStatus = 'sent'; return }
            SendCommand(fresh)
            lastCfgPushed = fresh
            cfgStatus = 'sent'
        }, CFG_DEBOUNCE_MS)
    }

    // ─── Save / Cancel ────────────────────────────────────────────────────
    function doSave() {
        if (configError() !== null) return
        const cfg: CalibResult = {
            min_us: _min, max_us: _max, speed: _speed,
            accel: _accel, decel: _decel, reversed: _rev,
        }
        if (cfgTimer) { clearTimeout(cfgTimer); cfgTimer = null }
        const finalCmd = cfgCommandFinal()
        SendCommand(finalCmd)
        lastCfgPushed = finalCmd
        cfgStatus = 'sent'
        onApply?.(cfg)
        close()
    }

    function doCancel() {
        // Restore the original config on the board so live edits don't leak out.
        if (cfgTimer) { clearTimeout(cfgTimer); cfgTimer = null }
        SendCommand(`servo.config ${servoId} ${_origMin} ${_origMax} ${_origSpeed} ${_origAccel} ${_origDecel}${_origRev ? ' rev' : ''}`)
        close()
    }

    function close() {
        open = false
        if (jogTimer) { clearTimeout(jogTimer); jogTimer = null }
        if (cfgTimer) { clearTimeout(cfgTimer); cfgTimer = null }
        onClose?.()
    }

    function handleKeydown(e: KeyboardEvent) {
        if (!open) return
        if (e.key === 'Escape') doCancel()
        if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) doSave()
    }

    onDestroy(() => {
        if (jogTimer) clearTimeout(jogTimer)
        if (cfgTimer) clearTimeout(cfgTimer)
    })

    // ─── Visual helpers ───
    $: minPct = Math.max(0, Math.min(100, ((_min - 500) / 2000) * 100))
    $: maxPct = Math.max(0, Math.min(100, ((_max - 500) / 2000) * 100))
    $: cfgErrMsg = configError()
    $: cfgBadge = cfgStatus === 'pending' ? '● syncing…'
                : cfgStatus === 'sent'    ? '✓ synced'
                : cfgStatus === 'invalid' ? '⚠ invalid'
                : ''
</script>

<svelte:window on:keydown={handleKeydown} />

{#if open}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="modal-backdrop calib-backdrop" on:click|self={doCancel}>
        <div class="calib-dialog">

            <div class="calib-header">
                <h2>Calibrate Servo — {servoName} <span class="dim">(id {servoId})</span></h2>
                <button class="close-btn" on:click={doCancel} title="Cancel (Esc)">✕</button>
            </div>

            <!-- Position slider: full servo range with min/max markers overlaid. -->
            <div class="calib-section">
                <div class="row">
                    <span class="section-label">Position</span>
                    <span class="value-pill">{Math.round(position_us)} µs</span>
                </div>
                <div class="slider-wrap">
                    <div class="slider-track-overlay" style="--min-pct: {minPct}%; --max-pct: {maxPct}%"></div>
                    <input type="range" min="500" max="2500" step="1"
                           bind:value={position_us} on:input={onPositionInput}
                           class="pos-slider"
                           title="Live jog — board moves as you drag" />
                </div>
                <div class="jog-row">
                    <button class="small" on:click={jogToMin} title="Jog to current Min">⏮ Min</button>
                    <button class="small" on:click={jogToCenter} title="Jog to center of current [Min,Max]">◼ Center</button>
                    <button class="small" on:click={jogToMax} title="Jog to current Max">⏭ Max</button>
                    <div class="grow"></div>
                    <button class="small accent" on:click={captureMin} title="Capture current position as Min">⤓ Capture Min</button>
                    <button class="small accent" on:click={captureMax} title="Capture current position as Max">⤒ Capture Max</button>
                </div>
            </div>

            <!-- Min/Max numeric edit + slider -->
            <div class="calib-section">
                <div class="row">
                    <label for="min-input" class="section-label">Min</label>
                    <input id="min-input" type="number" min="300" max="2700" step="1"
                           bind:value={_min} on:input={() => pushConfig()} class="num-input" />
                    <label for="max-input" class="section-label">Max</label>
                    <input id="max-input" type="number" min="300" max="2700" step="1"
                           bind:value={_max} on:input={() => pushConfig()} class="num-input" />
                    <span class="dim">Travel: {Math.max(0, _max - _min)} µs</span>
                </div>
            </div>

            <!-- Speed + (optional) accel/decel + reversed -->
            <div class="calib-section">
                <div class="row">
                    <label for="speed-input" class="section-label">Speed</label>
                    <input id="speed-input" type="number" min="0" max="65535" step="100"
                           bind:value={_speed} on:input={() => pushConfig()} class="num-input"
                           title="µs/second (0 = instant). Typical door: 2000-6000" />
                    {#if supportsAccelDecel}
                        <label for="accel-input" class="section-label">Accel</label>
                        <input id="accel-input" type="number" min="0" max="65535" step="100"
                               bind:value={_accel} on:input={() => pushConfig()} class="num-input"
                               title="Acceleration ramp (0 = instant)" />
                        <label for="decel-input" class="section-label">Decel</label>
                        <input id="decel-input" type="number" min="0" max="65535" step="100"
                               bind:value={_decel} on:input={() => pushConfig()} class="num-input"
                               title="Deceleration ramp (0 = instant)" />
                    {/if}
                    <label class="checkbox" title="Swap the effect of min and max">
                        <input type="checkbox" bind:checked={_rev} on:change={() => pushConfig()} />
                        Reversed
                    </label>
                </div>
            </div>

            <div class="calib-status">
                {#if cfgErrMsg}
                    <span class="err-msg">⚠ {cfgErrMsg}</span>
                {:else}
                    <span class="push-badge push-{cfgStatus}">{cfgBadge}</span>
                {/if}
            </div>

            <div class="calib-footer">
                <div class="hint">Drag the position slider to jog the servo. Use <kbd>Capture</kbd> buttons to set Min/Max from the current position.</div>
                <button on:click={doCancel}>Cancel</button>
                <button class="primary" on:click={doSave} disabled={cfgErrMsg !== null}
                        title="Apply config and close (Ctrl+Enter)">💾 Save</button>
            </div>
        </div>
    </div>
{/if}

<style>
    .calib-backdrop { z-index: 160; }

    .calib-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 560px;
        max-width: 92vw;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    .calib-header {
        display: flex; align-items: center; justify-content: space-between;
        padding: 14px 18px 10px;
    }
    .calib-header h2 { font-size: 14px; font-weight: 600; margin: 0; color: var(--text-bright); }
    .calib-header .dim { font-weight: 400; color: var(--text-dim); font-size: 12px; }

    .close-btn {
        background: none; border: none; color: var(--text-dim);
        font-size: 15px; cursor: pointer; padding: 2px 8px; border-radius: 4px;
    }
    .close-btn:hover { color: var(--text); background: var(--bg-raised); }

    .calib-section {
        padding: 10px 18px; border-top: 1px solid var(--border);
        display: flex; flex-direction: column; gap: 8px;
    }
    .row {
        display: flex; align-items: center; gap: 10px; flex-wrap: wrap;
    }
    .section-label { font-size: 11px; color: var(--text-dim); font-weight: 600; min-width: 48px; }
    .value-pill {
        margin-left: auto;
        font-family: var(--font-mono); font-size: 13px; font-weight: 600;
        padding: 2px 10px; border-radius: 10px;
        background: var(--bg-raised); color: var(--accent);
    }
    .grow { flex: 1; }
    .dim { color: var(--text-dim); font-size: 11px; }

    .slider-wrap {
        position: relative;
        padding: 6px 0;
    }
    .slider-track-overlay {
        position: absolute; left: 0; right: 0; top: 50%;
        height: 6px; transform: translateY(-50%);
        background: linear-gradient(to right,
            transparent 0,
            transparent var(--min-pct),
            color-mix(in srgb, var(--accent) 25%, transparent) var(--min-pct),
            color-mix(in srgb, var(--accent) 25%, transparent) var(--max-pct),
            transparent var(--max-pct)
        );
        border-radius: 3px;
        pointer-events: none;
        z-index: 0;
    }
    .pos-slider {
        position: relative; z-index: 1;
        width: 100%; accent-color: var(--accent);
    }

    .jog-row { display: flex; gap: 6px; flex-wrap: wrap; }

    .num-input {
        width: 80px; padding: 4px 8px;
        background: var(--bg-base); color: var(--text);
        border: 1px solid var(--border); border-radius: 4px;
        font-family: var(--font-mono); font-size: 12px;
    }

    .checkbox {
        display: flex; align-items: center; gap: 6px;
        font-size: 12px; color: var(--text); cursor: pointer;
    }

    .calib-status {
        padding: 6px 18px; min-height: 22px;
        display: flex; align-items: center;
    }
    .err-msg { color: var(--error); font-size: 11px; font-family: var(--font-mono); }

    .push-badge {
        font-size: 10px; font-weight: 600;
        padding: 2px 8px; border-radius: 8px;
        font-family: var(--font-mono);
    }
    .push-pending { background: color-mix(in srgb, var(--accent)  20%, transparent); color: var(--accent); }
    .push-sent    { background: color-mix(in srgb, var(--success) 20%, transparent); color: var(--success); }
    .push-invalid { background: color-mix(in srgb, var(--warning) 20%, transparent); color: var(--warning); }

    .calib-footer {
        display: flex; align-items: center; gap: 8px;
        padding: 10px 18px; border-top: 1px solid var(--border);
    }
    .hint { flex: 1; font-size: 11px; color: var(--text-dim); }
    .hint kbd {
        background: var(--bg-raised); border: 1px solid var(--border);
        border-radius: 3px; padding: 1px 6px; font-family: var(--font-mono); font-size: 10px;
    }

    button.accent {
        background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised));
        color: var(--accent); border-color: var(--accent);
    }
    button.primary { background: var(--accent); color: var(--bg-base); border-color: var(--accent); }
    button[disabled] { opacity: 0.5; cursor: not-allowed; }
</style>
