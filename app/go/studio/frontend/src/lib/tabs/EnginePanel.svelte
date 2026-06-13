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
        engineStart, engineStop, refreshEngineStatus,
        ENGINE_TYPES, OUTPUT_MODES,
        type EngineConfigT,
    } from '../effects'
    import { deviceModel, liveChannels, liveChannelKey, usToPct } from '../devicemodel'
    import { pickFile } from '../filepicker'
    import { collectChannelOptions } from '../channels'
    import { validateSoundFiles } from '../sound_validation'
    import SoundRow from '../components/SoundRow.svelte'
    import ChannelToggleCluster from '../components/ChannelToggleCluster.svelte'

    // Engine shares ONE output-mask across all three sounds (start /
    // running / stop are never audible simultaneously) — the YAML
    // persists it as the string `output: both|left|right`.  Mapping
    // helpers keep the SoundRow speaker button + the legacy string
    // field in lock-step; click on ANY sound row's speaker cycles the
    // shared mask, all three buttons update together.
    function maskFromOutput(out: string): number {
        if (out === 'left')  return 0x01
        if (out === 'right') return 0x02
        return 0x03
    }
    function outputFromMask(mask: number): string {
        if (mask === 0x01) return 'left'
        if (mask === 0x02) return 'right'
        return 'both'
    }
    function setEngineOutputMask(mask: number) {
        cfg.output = outputFromMask(mask); mark()
    }

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
    // Engine sound is "on" whenever it isn't fully stopped (starting /
    // running / stopping all count as on).  Drives the single on/off
    // toggle below — same single-button behaviour as the RC/manual
    // toggle: click flips, ON→OFF always allowed (cutoff), OFF→ON gated.
    $: engineOn = $engineStatus.stateName !== 'stopped'
    function onEngineToggle() { engineOn ? onStop() : onStart() }

    // ── Channel driver (toggle.input) ─────────────────────────────────
    // Lists every channel whose function is mapped to something other than
    // "unassigned" — operators can name an input channel and bind it here.
    $: chanOpts = collectChannelOptions($deviceModel)
    $: chosenChan = chanOpts.find(o => o.fnId === cfg?.toggle.input)
    $: liveUs = chosenChan ? $liveChannels[liveChannelKey({guid: chosenChan.portGuid, kind: 4, index: chosenChan.portIdx}, chosenChan.channel)] : null

    // The three sound slots — a typed tuple so the {#each} alias narrows to
    // the union (a bare array literal yields `string`, which then can't index
    // cfg.sounds / call the typed handlers — the 2 baseline type errors).
    const SOUND_FIELDS = ['starting', 'running', 'stopping'] as const
    type SoundField = typeof SOUND_FIELDS[number]

    async function browsePath(field: SoundField) {
        // Sounds always live on SD — open the picker constrained to that
        // backend so the Flash tab is hidden and the dialog opens at /sd:/.
        const p = await pickFile({ targets: 'sd' })
        if (p != null) { cfg.sounds[field] = p; mark(); scheduleValidate() }
    }

    function clearSound(field: SoundField) {
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
        // running is REQUIRED (looping sound); starting / stopping optional.
        const errs = await validateSoundFiles([
            { key: 'starting', path: cfg.sounds.starting ?? '' },
            { key: 'running',  path: cfg.sounds.running  ?? '', required: true },
            { key: 'stopping', path: cfg.sounds.stopping ?? '' },
        ])
        soundErrors = {
            starting: errs.starting ?? '',
            running:  errs.running  ?? '',
            stopping: errs.stopping ?? '',
        }
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
                <!-- Engine sound as a single on/off toggle — same
                     behaviour as the RC/manual toggle (Rule 48 single-
                     toggle variant): one button, label = state, click
                     flips.  ON→OFF is always enabled (emergency cutoff);
                     OFF→ON gates on this effect's dirty/errors (Rule 35)
                     so a stale config can't be started. -->
                <!-- Action toggle: label is what the click DOES (the
                     status pill beside it shows the live state, so the
                     button must not duplicate it).  Red when running =
                     "click to stop".  ON→OFF always enabled (cutoff);
                     OFF→ON gated (Rule 35/48). -->
                <button class="small state-toggle" class:danger={engineOn}
                        on:click={onEngineToggle}
                        disabled={engineOn ? busy : (busy || $engineDirty || soundsHaveErrors)}
                        title={engineOn ? 'Stop the engine sound (always available — emergency cutoff)'
                             : soundsHaveErrors ? 'Resolve validation errors before starting'
                             : $engineDirty ? 'Apply unsaved changes before starting'
                             : 'Start the engine sound'}>
                    {engineOn ? 'Engine Stop' : 'Engine Start'}
                </button>
            {/if}
        </div>
    </div>

    {#if cfg?.enabled}
        <!-- Shared channel-toggle cluster (Rule 36).  Renamed mapping:
             cfg.toggle.input/thresholdUs/hysteresisUs ↔ widget's
             inputId/thresholdUs/hysteresisUs.  The widget owns its own
             card chrome + legend; this panel just feeds + listens. -->
        <ChannelToggleCluster
            channelLabel="Driver channel"
            emptyOption="— manual only —"
            options={chanOpts.map(o => ({ id: o.fnId, label: o.label }))}
            inputId={cfg.toggle.input}
            thresholdUs={cfg.toggle.thresholdUs}
            hysteresisUs={cfg.toggle.hysteresisUs}
            liveUs={liveUs?.us ?? null}
            liveValid={liveUs?.valid ?? false}
            busy={busy}
            actionVerb="Fires"
            onChange={(n) => {
                cfg.toggle.input = n.inputId
                cfg.toggle.thresholdUs = n.thresholdUs
                cfg.toggle.hysteresisUs = n.hysteresisUs
                mark()
            }} />

        <!-- Engine type + output -->
        <div class="form-row">
            <span class="field-label">Type</span>
            <select class="field-input wide" value={cfg.type} on:change={(e) => { cfg.type = selValue(e); mark() }} disabled={busy}>
                {#each ENGINE_TYPES as t}
                    <option value={t.id} disabled={t.id !== 'turbine'}>{t.label}</option>
                {/each}
            </select>
        </div>
        <!-- Sounds (turbine).  Speaker-routing button on each row binds
             to the engine-level `cfg.output` field — engine plays one
             sound at a time, so a per-row mask would be redundant; the
             dedicated "Output" dropdown that used to sit above this
             section was removed (single source of truth — the speaker
             button IS the dropdown, just visually integrated into the
             sound row, matching GunFx). -->
        <div class="section-head" class:section-error={soundsHaveErrors}>
            Sounds {#if soundsHaveErrors}<span class="section-err-tag">missing files</span>{/if}
        </div>
        {#each SOUND_FIELDS as f}
            {@const err = soundErrors[f]}
            {@const optional = f !== 'running'}
            <SoundRow
                label={f === 'running' ? 'looping' : f}
                placeholder={optional ? '/sounds/…  (optional)' : '/sounds/…  (required)'}
                value={cfg.sounds[f]}
                outputMask={maskFromOutput(cfg.output)}
                busy={busy}
                required={!optional}
                error={err}
                onPathChange={(v) => { cfg.sounds[f] = v; mark(); scheduleValidate() }}
                onMaskChange={setEngineOutputMask}
                onBrowse={() => browsePath(f)}
                onClear={() => clearSound(f)} />
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

    /* .btn-slot + .btn-spacer moved to style.css (2026-05-24) so
       shared components like SoundRow inherit the width — local
       Svelte-scoped styling broke alignment when the slot lived only
       in this file. */

    /* Status-row controls: dirty-flag → Apply → divider → Start/Stop. The
       divider is a 1px vertical rule that separates the "commit config"
       action from the "operate engine" actions so they don't look like
       one button group. */
    .dirty-flag { font-size: 11px; color: var(--text-dim); margin-right: 4px; }
    .dirty-flag.on { color: var(--warning); font-weight: 600; }
    .dirty-flag.err { color: var(--error); font-weight: 600; }
    .ctrl-sep { display: inline-block; width: 1px; height: 20px; background: var(--border); margin: 0 4px; }
</style>
