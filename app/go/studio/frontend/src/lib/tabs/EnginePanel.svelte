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

    async function onEnableToggle(e: Event) {
        cfg.enabled = (e.target as HTMLInputElement).checked
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
        const p = await pickFile()
        if (p != null) { cfg.sounds[field] = p; dirty = true }
    }
</script>

<div class="card engine-card" class:disabled={!cfg?.enabled}>
    <div class="card-header">
        <h3>EngineFX</h3>
        <label class="enable-toggle">
            <input type="checkbox" checked={cfg?.enabled} on:change={onEnableToggle} disabled={busy} />
            <span>{cfg?.enabled ? 'Enabled' : 'Disabled'}</span>
        </label>
    </div>

    {#if cfg?.enabled}
        {#if error}<div class="banner err">{error}</div>{/if}

        <!-- Live status + manual control -->
        <div class="status-row">
            <div class="status">
                <span class="status-label">State</span>
                <span class="state-pill state-{$engineStatus.stateName}">{$engineStatus.stateName}</span>
                {#if $engineStatus.engaged}<span class="engaged-pill">RC engaged</span>{/if}
            </div>
            <div class="controls">
                <button class="small" on:click={onStart} disabled={busy}>▶ Start</button>
                <button class="small" on:click={onStop} disabled={busy}>■ Stop</button>
            </div>
        </div>

        <!-- Driver channel + live bar -->
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
        <div class="bar" class:nosignal={!liveUs || !liveUs.valid}>
            {#if liveUs && liveUs.valid}
                <div class="bar-fill" style="width: {usToPct(liveUs.us)}%"></div>
                <span class="bar-val">{liveUs.us}µs · threshold {cfg.toggle.thresholdUs}µs</span>
            {:else}
                <span class="bar-nosignal">{cfg.toggle.input ? 'NO SIGNAL' : 'no channel bound — manual only'}</span>
            {/if}
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
        <div class="section-head">Sounds</div>
        {#each ['starting', 'running', 'stopping'] as f}
            <div class="form-row">
                <span class="field-label" style="width: 72px">{f === 'running' ? 'looping' : f}</span>
                <input class="field-input wide" type="text" placeholder="/sounds/…"
                       value={cfg.sounds[f]} on:input={(e) => { cfg.sounds[f] = inputValue(e); mark() }}
                       disabled={busy} />
                <button class="small" on:click={() => browsePath(f)} disabled={busy} title="Browse device files">…</button>
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

        <!-- Toggle thresholds -->
        <div class="section-head">RC Trigger</div>
        <div class="form-grid cols-2">
            <div class="form-field">
                <span class="field-label">Threshold (µs)</span>
                <input class="field-input narrow" type="number" min="800" max="2200" value={cfg.toggle.thresholdUs}
                       on:change={(e) => { cfg.toggle.thresholdUs = numValue(e); mark() }} />
            </div>
            <div class="form-field">
                <span class="field-label">Hysteresis (µs)</span>
                <input class="field-input narrow" type="number" min="0" max="500" value={cfg.toggle.hysteresisUs}
                       on:change={(e) => { cfg.toggle.hysteresisUs = numValue(e); mark() }} />
            </div>
        </div>

        <div class="save-row">
            <span class="dirty-flag" class:on={$engineDirty}>{$engineDirty ? 'unapplied changes' : 'in sync with firmware'}</span>
            <button class="primary" on:click={onApply} disabled={busy || !$engineDirty}
                    title="Write /enginefx.yaml + reload — settings take effect immediately">Apply</button>
        </div>
    {/if}
</div>

<style>
    .engine-card.disabled { opacity: 0.78; }

    .enable-toggle { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-dim); cursor: pointer; }
    .enable-toggle input { accent-color: var(--accent); }

    .banner.err { background: rgba(255,80,80,0.12); border: 1px solid var(--error); color: var(--error); padding: 7px 10px; border-radius: 4px; margin: 6px 0; font-size: 12px; }

    .status-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin: 8px 0 12px; padding: 8px 10px; background: var(--bg-raised); border-radius: 6px; }
    .status { display: flex; align-items: center; gap: 8px; }
    .status-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .state-pill { font-family: var(--font-mono); font-size: 11px; font-weight: 700; text-transform: uppercase; padding: 2px 10px; border-radius: 3px; background: var(--bg-input); color: var(--text); border: 1px solid var(--border); }
    .state-running { background: rgba(30,158,74,0.25); color: #6ddc94; border-color: rgba(30,158,74,0.6); }
    .state-starting, .state-stopping { background: rgba(255,180,0,0.18); color: var(--warning); border-color: rgba(255,180,0,0.5); }
    .state-stopped { color: var(--text-dim); }
    .engaged-pill { font-size: 9px; padding: 1px 6px; border-radius: 3px; background: var(--accent); color: #fff; }
    .controls { display: flex; gap: 6px; }

    .field-label { font-size: 10px; text-transform: uppercase; letter-spacing: 0.3px; color: var(--text-dim); }

    .bar { position: relative; height: 14px; background: var(--bg-raised); border: 1px solid var(--border); border-radius: 3px; overflow: hidden; margin-bottom: 10px; }
    .bar-fill { height: 100%; background: linear-gradient(90deg, var(--accent), var(--success)); transition: width 0.08s linear; }
    .bar.nosignal { background: repeating-linear-gradient(45deg, var(--bg-raised), var(--bg-raised) 6px, transparent 6px, transparent 12px); }
    .bar-val { position: absolute; right: 6px; top: 0; line-height: 14px; font-family: var(--font-mono); font-size: 9px; color: var(--text-bright); text-shadow: 0 0 3px rgba(0,0,0,0.7); }
    .bar-nosignal { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-family: var(--font-mono); font-size: 9px; letter-spacing: 0.5px; color: var(--text-dim); }

    .section-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-bright); margin: 14px 0 6px; padding-bottom: 4px; border-bottom: 1px solid var(--border); }

    .save-row { display: flex; align-items: center; justify-content: flex-end; gap: 10px; margin-top: 14px; padding-top: 10px; border-top: 1px solid var(--border); }
    .dirty-flag { font-size: 11px; color: var(--text-dim); }
    .dirty-flag.on { color: var(--warning); font-weight: 600; }
</style>
