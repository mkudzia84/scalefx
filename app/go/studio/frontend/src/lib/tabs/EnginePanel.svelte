<!-- ScaleFX Studio — EngineFX panel.
     Enable toggle in the card header; content expands when enabled.
     Content: live status + manual Start/Stop, channel-driver dropdown with
     a live value bar, engine type/output, sound paths (with file picker),
     transitions (offsets + fades) and toggle thresholds, Save. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import {
        engineDraft, engineDirty, engineStatus,
        loadEngineConfig, applyEngineConfig,
        engineStart, engineStop, refreshEngineStatus, checkFiles,
        ENGINE_TYPES, OUTPUT_MODES,
        type EngineConfigT,
    } from '../effects'
    import { deviceModel, liveChannels, liveChannelKey, usToPct } from '../devicemodel'
    import { pickFile } from '../filepicker'

    // The form binds to the persistent `engineDraft` store, so switching
    // tabs no longer wipes in-flight edits.  `mark()` after each mutation
    // re-publishes the draft so derived `engineDirty` recomputes.
    let cfg: EngineConfigT
    let busy = false
    let error = ''

    const unsub = engineDraft.subscribe(c => { cfg = c })
    onMount(() => {
        loadEngineConfig()
        refreshEngineStatus()
        return unsub
    })

    function mark(): void { engineDraft.set(cfg) }

    function selValue(e: Event): string { return (e.target as HTMLSelectElement).value }
    function inputValue(e: Event): string { return (e.target as HTMLInputElement).value }
    function numValue(e: Event): number { return Number((e.target as HTMLInputElement).value) }

    function onEnableToggle(on: boolean) {
        cfg.enabled = on
        mark()
    }

    /** Apply pushes the draft to firmware (YAML write + ReloadPath) — config
     *  takes effect immediately on the hub for dynamic testing AND persists. */
    async function onApply() {
        busy = true; error = ''
        try { await applyEngineConfig() } catch (e) { error = String(e) } finally { busy = false }
    }
    async function onStart() {
        busy = true; error = ''
        try { await engineStart() } catch (e) { error = String(e) } finally { busy = false }
    }
    async function onStop() {
        busy = true; error = ''
        try { await engineStop() } catch (e) { error = String(e) } finally { busy = false }
    }

    // ── Channel driver (toggle.input) ─────────────────────────────────
    // Lists every channel whose function is mapped to something other than
    // "unassigned" — operators can name an input channel and bind it here.
    type ChanOpt = { fnId: string; label: string; portGuid: string; portIdx: number; channel: number }
    $: chanOpts = collectChannels($deviceModel)
    function collectChannels(_dm: typeof $deviceModel): ChanOpt[] {
        const fns = new Map($deviceModel.channelFunctions.map(f => [f.id, f.label] as const))
        const out: ChanOpt[] = []
        for (const inp of $deviceModel.inputs) {
            for (const c of inp.channels) {
                if (c.function === 'unassigned') continue
                out.push({
                    fnId: c.function,
                    label: `CH${c.channel + 1} · ${fns.get(c.function) ?? c.function}`,
                    portGuid: inp.port.guid, portIdx: inp.port.index, channel: c.channel,
                })
            }
        }
        return out
    }
    $: chosenChan = chanOpts.find(o => o.fnId === cfg?.toggle.input)
    $: liveUs = chosenChan ? $liveChannels[liveChannelKey({guid: chosenChan.portGuid, kind: 4, index: chosenChan.portIdx}, chosenChan.channel)] : null

    async function browsePath(field: 'starting' | 'running' | 'stopping') {
        // Sounds always live on SD — open the picker constrained to that
        // backend so the Flash tab is hidden and the dialog opens at /sd:/.
        const p = await pickFile({ targets: 'sd' })
        if (p != null) { cfg.sounds[field] = p; mark(); scheduleValidate() }
    }

    function clearSound(field: 'starting' | 'stopping') {
        cfg.sounds[field] = ''
        mark()
        // Clearing always validates immediately — no path = valid for optional fields.
        soundErrors = { ...soundErrors, [field]: '' }
    }

    // ── Sound-file validation (existence check on SD) ──
    // soundErrors[field] = '' means valid.  Non-empty = error text shown
    // under the row + a red border on the row.  starting / stopping are
    // OPTIONAL (empty path is valid); running is REQUIRED.
    let soundErrors: { starting: string; running: string; stopping: string } =
        { starting: '', running: '', stopping: '' }
    let validateTimer: ReturnType<typeof setTimeout> | null = null
    function scheduleValidate() {
        if (validateTimer) clearTimeout(validateTimer)
        validateTimer = setTimeout(() => { validateTimer = null; void validateSounds() }, 350)
    }
    async function validateSounds() {
        const next = { starting: '', running: '', stopping: '' }
        // Required: running must have a path.
        if (!cfg.sounds.running) next.running = 'required — pick a looping sound'
        const probe = [cfg.sounds.starting, cfg.sounds.running, cfg.sounds.stopping].filter(p => !!p)
        if (probe.length > 0) {
            const exists = await checkFiles(probe)
            for (const k of ['starting','running','stopping'] as const) {
                const p = cfg.sounds[k]
                if (!p) continue
                if (!exists[p]) next[k] = `file not found on SD: ${p}`
            }
        }
        soundErrors = next
    }

    // Re-validate on first paint and whenever the draft swaps in a new path.
    $: void scheduleValidateOn(cfg?.sounds.starting, cfg?.sounds.running, cfg?.sounds.stopping)
    function scheduleValidateOn(..._: unknown[]) {
        if (cfg) scheduleValidate()
    }

    $: soundsHaveErrors = !!(soundErrors.starting || soundErrors.running || soundErrors.stopping)
    // Rule 46: panel doesn't register with the dirty-registry — the
    // domain module (effects.ts) owns `engineConfigSource` and
    // App.svelte registers it once at startup.
</script>

<div class="card engine-card" class:disabled={!cfg?.enabled}>
    <!-- Rule 45 header cluster: [Enable-Button] [Apply] [dirty-flag] ‖ [▶ Start] [■ Stop]
         Always visible — when the effect is disabled the rest of the
         panel hides but the enable button stays so the operator can
         flip it back on without hunting.  Extends Rule 35 by promoting
         the enable affordance into the same row as Apply/Start. -->
    <div class="card-header">
        <h3>EngineFX</h3>
    </div>

    {#if error}<div class="banner err">{error}</div>{/if}

    <div class="status-row">
        <div class="status">
            {#if cfg?.enabled}
                <span class="status-label">State</span>
                <span class="state-pill state-{$engineStatus.stateName}">{$engineStatus.stateName}</span>
                {#if $engineStatus.engaged}<span class="engaged-pill">RC engaged</span>{/if}
            {/if}
        </div>
        <div class="controls">
            <!-- Apply lives in the global ConfigToolbar; this panel
                 only carries the enable-toggle + operational Start/Stop.
                 The toolbar gates Apply on errors anywhere; Start/Stop
                 still gate locally on this effect's dirty/errors so the
                 operator can't trigger a test against a stale config. -->
            <button class="small state-toggle" class:state-on={cfg?.enabled}
                    on:click={() => onEnableToggle(!cfg?.enabled)}
                    disabled={busy}
                    title={cfg?.enabled ? 'Disable EngineFX — press Apply (top bar) to push.' : 'Enable EngineFX — press Apply (top bar) to push.'}>
                {cfg?.enabled ? '✓ Enabled' : '▶ Disabled'}
            </button>
            {#if cfg?.enabled}
                <span class="ctrl-sep" aria-hidden="true"></span>
                <button class="small" on:click={onStart}
                        disabled={busy || $engineDirty || soundsHaveErrors}
                        title={soundsHaveErrors ? 'Resolve validation errors first' : $engineDirty ? 'Apply unsaved changes before starting' : 'Start the engine'}>▶ Start</button>
                <button class="small" on:click={onStop} disabled={busy}>■ Stop</button>
            {/if}
        </div>
    </div>

    {#if cfg?.enabled}
        <!-- Channel-setup cluster (Rule 36):
             [1] channel selector → [2] threshold + hysteresis settings →
             [3] live bar with explicit threshold line + shaded hysteresis
             band → [4] one-line legend.  Settings sit directly above the
             bar so the operator sees the marker move as they edit. -->
        <div class="chan-cluster">
            <div class="form-row">
                <span class="field-label">Driver channel</span>
                <select class="field-input wide" value={cfg.toggle.input}
                        on:change={(e) => { cfg.toggle.input = selValue(e); mark() }} disabled={busy}>
                    <option value="">— manual only —</option>
                    {#each chanOpts as o}
                        <option value={o.fnId}>{o.label}</option>
                    {/each}
                </select>
            </div>

            <div class="form-row trigger-row" title="The engine fires when the channel value rises past threshold; hysteresis is the deadband that prevents jitter from re-triggering.">
                <span class="field-label">Fires when channel ≥</span>
                <input class="field-input narrow" type="number" min="800" max="2200" step="10"
                       value={cfg.toggle.thresholdUs}
                       on:change={(e) => { cfg.toggle.thresholdUs = numValue(e); mark() }} disabled={busy} />
                <span class="unit">µs</span>
                <span class="trigger-pm">±</span>
                <input class="field-input narrow" type="number" min="0" max="500" step="5"
                       value={cfg.toggle.hysteresisUs}
                       on:change={(e) => { cfg.toggle.hysteresisUs = numValue(e); mark() }} disabled={busy} />
                <span class="unit">µs hysteresis</span>
            </div>

            <div class="bar tall" class:nosignal={!liveUs || !liveUs.valid}>
                {#if liveUs && liveUs.valid}
                    <div class="bar-fill" style="width: {usToPct(liveUs.us)}%"></div>
                {/if}
                <!-- Hysteresis band — shaded deadband around the threshold.
                     Visible even with no signal so the operator can dial
                     the trigger before powering the RC link. -->
                <div class="hyst-band"
                     style="left: {usToPct(cfg.toggle.thresholdUs - cfg.toggle.hysteresisUs)}%;
                            width: {Math.max(0.4, usToPct(cfg.toggle.thresholdUs + cfg.toggle.hysteresisUs) - usToPct(cfg.toggle.thresholdUs - cfg.toggle.hysteresisUs))}%"
                     title="hysteresis band — ±{cfg.toggle.hysteresisUs}µs around threshold"></div>
                <div class="threshold-mark" style="left: {usToPct(cfg.toggle.thresholdUs)}%"
                     title="threshold {cfg.toggle.thresholdUs}µs"></div>
                {#if !liveUs || !liveUs.valid}
                    <span class="bar-nosignal">{cfg.toggle.input ? 'NO SIGNAL' : 'no channel bound — manual only'}</span>
                {/if}
            </div>

            <div class="bar-legend">
                <span class="leg-live">
                    {#if liveUs && liveUs.valid}{liveUs.us} µs{:else}—{/if}
                    <span class="leg-label">live</span>
                </span>
                <span class="leg-sep">·</span>
                <span class="leg-mark">{cfg.toggle.thresholdUs} µs <span class="leg-label">threshold</span></span>
                <span class="leg-sep">·</span>
                <span class="leg-band">±{cfg.toggle.hysteresisUs} µs <span class="leg-label">hysteresis</span></span>
                <span class="leg-sep">·</span>
                <span class="leg-range" title="RC PPM/SBUS range — 1000µs is min stick, 2000µs is max stick">1000–2000 µs</span>
            </div>
        </div>

        <!-- Engine type + output -->
        <div class="form-row">
            <span class="field-label">Type</span>
            <select class="field-input wide" value={cfg.type} on:change={(e) => { cfg.type = selValue(e); mark() }} disabled={busy}>
                {#each ENGINE_TYPES as t}
                    <option value={t.id} disabled={t.id !== 'turbine'}>{t.label}</option>
                {/each}
            </select>
        </div>
        <div class="form-row">
            <span class="field-label">Output</span>
            <select class="field-input wide" value={cfg.output} on:change={(e) => { cfg.output = selValue(e); mark() }} disabled={busy}>
                {#each OUTPUT_MODES as o}<option value={o.id}>{o.label}</option>{/each}
            </select>
        </div>

        <!-- Sounds (turbine) -->
        <div class="section-head" class:section-error={soundsHaveErrors}>
            Sounds {#if soundsHaveErrors}<span class="section-err-tag">missing files</span>{/if}
        </div>
        {#each ['starting', 'running', 'stopping'] as f}
            {@const err = soundErrors[f]}
            {@const optional = f !== 'running'}
            <div class="sound-row" class:invalid={!!err}>
                <div class="form-row">
                    <span class="field-label" style="width: 72px">{f === 'running' ? 'looping' : f}{optional ? '' : ' *'}</span>
                    <input class="field-input wide" type="text" placeholder={optional ? '/sounds/…  (optional)' : '/sounds/…  (required)'}
                           value={cfg.sounds[f]} on:input={(e) => { cfg.sounds[f] = inputValue(e); mark(); scheduleValidate() }}
                           disabled={busy} />
                    <!-- Button cluster: browse (…) always on the LEFT, Clear always
                         on the RIGHT. Required rows reserve the Clear slot with a
                         hidden placeholder so the … column aligns across rows. -->
                    <button class="small btn-slot" on:click={() => browsePath(f)} disabled={busy} title="Browse SD card">…</button>
                    {#if optional}
                        <button class="small btn-slot" on:click={() => clearSound(f)} disabled={busy || !cfg.sounds[f]}
                                title="Clear — this sound is optional">Clear</button>
                    {:else}
                        <span class="btn-slot btn-spacer" aria-hidden="true"></span>
                    {/if}
                </div>
                {#if err}<div class="row-err">⚠ {err}</div>{/if}
            </div>
        {/each}

        <!-- Transitions -->
        <div class="section-head">Transitions</div>
        <div class="form-grid cols-2">
            <div class="form-field">
                <span class="field-label">Starting offset (ms)</span>
                <input class="field-input narrow" type="number" min="0" value={cfg.sounds.transitions.startingOffsetMs}
                       on:change={(e) => { cfg.sounds.transitions.startingOffsetMs = numValue(e); mark() }} />
            </div>
            <div class="form-field">
                <span class="field-label">Stopping offset (ms)</span>
                <input class="field-input narrow" type="number" min="0" value={cfg.sounds.transitions.stoppingOffsetMs}
                       on:change={(e) => { cfg.sounds.transitions.stoppingOffsetMs = numValue(e); mark() }} />
            </div>
            <div class="form-field">
                <span class="field-label">Start fade-in (ms)</span>
                <input class="field-input narrow" type="number" min="0" max="10000" value={cfg.sounds.transitions.startFadeInMs}
                       on:change={(e) => { cfg.sounds.transitions.startFadeInMs = numValue(e); mark() }} />
            </div>
            <div class="form-field">
                <span class="field-label">Stop fade-out (ms)</span>
                <input class="field-input narrow" type="number" min="0" max="10000" value={cfg.sounds.transitions.stopFadeOutMs}
                       on:change={(e) => { cfg.sounds.transitions.stopFadeOutMs = numValue(e); mark() }} />
            </div>
        </div>

    {/if}
</div>

<style>
    .engine-card.disabled { opacity: 0.78; }

    .enable-toggle { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-dim); cursor: pointer; }
    .enable-toggle input { accent-color: var(--accent); }

    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); padding: 7px 10px; border-radius: 4px; margin: 6px 0; font-size: 12px; }

    /* .status-row + .status + .state-pill base styles are shared via
       style.css.  Keep only the EngineFx-specific state colour classes
       here (state-running / state-starting / state-stopped). */
    .state-running { background: rgba(30,158,74,0.25); color: #6ddc94; border-color: rgba(30,158,74,0.6); }
    .state-starting, .state-stopping { background: rgba(255,180,0,0.18); color: var(--warning); border-color: rgba(255,180,0,0.5); }
    .state-stopped { color: var(--text-dim); }
    .engaged-pill { font-size: 9px; padding: 1px 6px; border-radius: 3px; background: var(--accent); color: #fff; }
    .controls { display: flex; gap: 6px; }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }

    .bar { position: relative; height: 14px; background: var(--bg-raised); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; margin-bottom: 10px; }
    .bar.tall { height: 18px; margin-bottom: 4px; }
    .bar-fill { height: 100%; background: linear-gradient(90deg, var(--accent), var(--success)); transition: width 0.08s linear; }
    .bar.nosignal { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px); }
    .bar-val { position: absolute; right: 6px; top: 0; line-height: 14px; font-family: var(--font-mono); font-size: 9px; color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); }
    .bar-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.5px; color: var(--text-dim); }

    /* ── Channel-setup cluster (Rule 36) ──
       Threshold marker = solid 2px line; hysteresis band = translucent
       warning-colored fill behind the live bar so the deadband is obvious
       even on a moving signal. */
    .chan-cluster { background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; padding: 8px 10px 6px; margin: 6px 0 14px; }
    .chan-cluster .form-row { margin-bottom: 6px; }
    .trigger-row { display: flex; align-items: center; flex-wrap: wrap; gap: 6px; margin-top: 2px; }
    .trigger-row .field-label { margin-right: 2px; }
    .trigger-row .field-input.narrow { width: 70px; height: 24px; padding: 0 6px; box-sizing: border-box; text-align: right; font-family: var(--font-mono); }
    .unit { font-size: 10px; color: var(--text-dim); font-family: var(--font-mono); }
    .trigger-pm { margin: 0 4px; color: var(--text-dim); font-family: var(--font-mono); font-weight: 700; }

    .hyst-band { position: absolute; top: 0; bottom: 0; background: color-mix(in srgb, var(--warning) 28%, transparent); border-left: 1px dashed color-mix(in srgb, var(--warning) 70%, transparent); border-right: 1px dashed color-mix(in srgb, var(--warning) 70%, transparent); pointer-events: none; z-index: 1; }
    .threshold-mark { position: absolute; top: -2px; bottom: -2px; width: 2px; background: var(--error); box-shadow: 0 0 4px rgba(255,80,80,0.6); pointer-events: none; z-index: 2; transform: translateX(-1px); }

    .bar-legend { display: flex; align-items: center; gap: 4px; flex-wrap: wrap; font-size: 10px; font-family: var(--font-mono); color: var(--text); margin-bottom: 2px; }
    .bar-legend .leg-label { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); margin-left: 2px; }
    .bar-legend .leg-sep { color: var(--text-dim); padding: 0 2px; }
    .bar-legend .leg-live { color: var(--success); font-weight: 700; }
    .bar-legend .leg-mark { color: var(--error); font-weight: 700; }
    .bar-legend .leg-band { color: var(--warning); }
    .bar-legend .leg-range { color: var(--text-dim); margin-left: auto; }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); display: flex; align-items: baseline; gap: 8px; }
    .section-head.section-error { color: var(--error); border-bottom-color: var(--error); }
    .section-err-tag { font-size: 9px; font-weight: 700; color: var(--error); padding: 1px 6px; border: 1px solid var(--error); border-radius: 3px; letter-spacing: 0.5px; }

    /* Per-sound row — red border + error text when the file is missing or
       the required row is empty. */
    .sound-row { border: 1px solid transparent; border-radius: 4px; padding: 3px 4px; margin-bottom: 4px; }
    .sound-row.invalid { border-color: var(--error); background: rgba(255,80,80,0.06); }
    .row-err { font-size: 11px; color: var(--error); margin: 3px 0 0 80px; font-family: var(--font-mono); }

    /* Button cluster after the input: fixed slot width so [browse][clear]
       align across rows, with a visibility-hidden spacer on rows that don't
       expose Clear (Rule 34 — Studio button alignment). */
    .btn-slot { width: 64px; min-width: 64px; box-sizing: border-box; text-align: center; flex-shrink: 0; }
    .btn-spacer { display: inline-block; height: 28px; visibility: hidden; }

    /* Status-row controls: dirty-flag → Apply → divider → Start/Stop. The
       divider is a 1px vertical rule that separates the "commit config"
       action from the "operate engine" actions so they don't look like
       one button group. */
    .dirty-flag { font-size: 11px; color: var(--text-dim); margin-right: 4px; }
    .dirty-flag.on { color: var(--warning); font-weight: 600; }
    .dirty-flag.err { color: var(--error); font-weight: 600; }
    .ctrl-sep { display: inline-block; width: 1px; height: 20px; background: var(--border); margin: 0 4px; }
</style>
