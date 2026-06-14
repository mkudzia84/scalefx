<!-- WizardEffectEngine — full engine setup: type · speakers · RC on/off +
     failsafe · the three sounds.  Engine output is mixer-fixed (no ports). -->
<script lang="ts">
    import { engineDraft, ENGINE_TYPES, OUTPUT_MODES, FAILSAFE_MODES } from '../effects'
    import { pickFile } from '../filepicker'
    import WizardChannelSelect from './WizardChannelSelect.svelte'

    const set       = (p: any) => engineDraft.update(c => ({ ...c, ...p }))
    const setToggle  = (p: any) => engineDraft.update(c => ({ ...c, toggle: { ...c.toggle, ...p } }))
    const setSounds  = (p: any) => engineDraft.update(c => ({ ...c, sounds: { ...c.sounds, ...p } }))
    const numValue   = (e: Event) => Number((e.target as HTMLInputElement).value)
    const selValue   = (e: Event) => (e.target as HTMLSelectElement).value
    const strVal     = (e: Event) => (e.target as HTMLInputElement).value

    async function browse(field: 'starting' | 'running' | 'stopping') {
        const p = await pickFile({ targets: 'sd' })
        if (p != null) setSounds({ [field]: p })
    }
</script>

<div class="fx-sec">
    <div class="fx-sec-head">Engine</div>
    <div class="fx-row">
        <span class="lbl">Type</span>
        <select class="field-input wide" value={$engineDraft.type} on:change={(e) => set({ type: selValue(e) })}>
            {#each ENGINE_TYPES as t}<option value={t.id}>{t.label}</option>{/each}
        </select>
    </div>
    <div class="fx-row">
        <span class="lbl">Speakers</span>
        <select class="field-input wide" value={$engineDraft.output} on:change={(e) => set({ output: selValue(e) })}>
            {#each OUTPUT_MODES as o}<option value={o.id}>{o.label}</option>{/each}
        </select>
    </div>
</div>

<div class="fx-sec">
    <div class="fx-sec-head">RC on/off</div>
    <WizardChannelSelect label="Channel" value={$engineDraft.toggle.input}
        onChange={(fn) => setToggle({ input: fn })} />
    <div class="fx-row">
        <span class="lbl">Threshold</span>
        <input class="field-input narrow" type="number" min="1000" max="2000" step="10"
               value={$engineDraft.toggle.thresholdUs} on:change={(e) => setToggle({ thresholdUs: Math.round(numValue(e)) })} />
        <span class="unit">µs · hysteresis</span>
        <input class="field-input narrow" type="number" min="0" max="200" step="5"
               value={$engineDraft.toggle.hysteresisUs} on:change={(e) => setToggle({ hysteresisUs: Math.round(numValue(e)) })} />
        <span class="unit">µs</span>
    </div>
    <div class="fx-row">
        <span class="lbl">On RC loss</span>
        <select class="field-input wide" value={$engineDraft.toggle.failsafe} on:change={(e) => setToggle({ failsafe: selValue(e) })}>
            {#each FAILSAFE_MODES as m}<option value={m.id}>{m.label}</option>{/each}
        </select>
    </div>
</div>

<div class="fx-sec">
    <div class="fx-sec-head">Sounds</div>
    {#each ([['running', 'Running (required)'], ['starting', 'Start-up'], ['stopping', 'Shut-down']]) as [field, lbl]}
        <div class="fx-row">
            <span class="lbl">{lbl}</span>
            <input class="field-input wide" type="text" placeholder="/sounds/…" value={$engineDraft.sounds[field] ?? ''}
                   on:change={(e) => setSounds({ [field]: strVal(e) })} />
            <button class="small" on:click={() => browse(field)}>… Browse</button>
        </div>
    {/each}
    {#if !$engineDraft.sounds.running}
        <div class="fx-warn">⚠ Pick a running sound — the engine needs at least that.</div>
    {/if}
</div>

<style>
    .fx-sec { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 10px 12px; margin-bottom: 10px; }
    .fx-sec-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 8px; }
    .fx-row { display: flex; align-items: center; gap: 10px; margin: 5px 0; flex-wrap: wrap; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    :global(.fx-sec .field-input.wide) { flex: 1; min-width: 240px; }
    .field-input.narrow { width: 84px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .fx-warn { font-size: 11px; color: var(--warning); margin-top: 6px; }
</style>
