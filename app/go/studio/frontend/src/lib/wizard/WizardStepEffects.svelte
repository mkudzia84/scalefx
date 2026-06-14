<!-- WizardStepEffects — Step 4: per enabled effect, wire its RC channel
     (the key mapping) + show advice + a deep-link to the full panel for
     detailed port assignment.  Engine is fully configurable here (no ports). -->
<script lang="ts">
    import { get } from 'svelte/store'
    import { deviceModel, studioTabs } from '../devicemodel'
    import { activeTab } from '../stores'
    import { collectChannelOptions } from '../channels'
    import { WIZARD_FEATURES } from '../wizard-features'
    import { analyzeWizard, adviceFor } from '../wizard-advice'
    import { closeWizard } from '../wizard'
    import { engineDraft, ENGINE_TYPES, OUTPUT_MODES } from '../effects'
    import { gunfxDraft } from '../gunfx'
    import { lightfxDraft } from '../lightfx'
    import { landingDraft } from '../landing'
    import { gearDraft } from '../gear'

    // Track every draft so getInput()/isEnabled() reads stay reactive.
    $: _track = [$engineDraft, $gunfxDraft, $lightfxDraft, $landingDraft, $gearDraft]
    $: enabled = (_track, WIZARD_FEATURES.filter(f => f.isEnabled()))
    $: chanOpts = collectChannelOptions($deviceModel)
    $: advice = analyzeWizard($deviceModel)

    // De-dup channel options by function id (one entry per named channel).
    $: opts = (() => {
        const seen = new Set<string>()
        return chanOpts.filter(o => (seen.has(o.fnId) ? false : (seen.add(o.fnId), true)))
    })()

    const selValue = (e: Event) => (e.target as HTMLSelectElement).value

    function openPanel(kind: string) {
        const tabs = get(studioTabs)
        const idx = tabs.findIndex(t => t.kind === kind)
        if (idx >= 0) activeTab.set(idx)
        closeWizard()
    }
</script>

{#if enabled.length === 0}
    <p class="muted">No effects enabled — go back to <strong>Features</strong> and pick at least one.</p>
{:else}
    {#each enabled as f (f.id)}
        {@const items = adviceFor(advice, f.id)}
        <div class="fx-card">
            <div class="fx-head">
                <span class="fx-icon">{f.icon}</span>
                <span class="fx-name">{f.label}</span>
                <button class="small fx-open" on:click={() => openPanel(f.tabKind)}>Open panel →</button>
            </div>

            <div class="fx-row">
                <span class="lbl">{f.inputs[0]?.label ?? 'RC channel'}</span>
                <select class="field-input" value={f.getInput()} on:change={(e) => f.setInput(selValue(e))}>
                    <option value="">— manual (no radio) —</option>
                    {#each opts as o}<option value={o.fnId}>{o.label}</option>{/each}
                </select>
            </div>

            {#if f.id === 'engine'}
                <div class="fx-row">
                    <span class="lbl">Type</span>
                    <select class="field-input" value={$engineDraft.type}
                            on:change={(e) => engineDraft.update(c => ({ ...c, type: selValue(e) }))}>
                        {#each ENGINE_TYPES as t}<option value={t.id}>{t.label}</option>{/each}
                    </select>
                </div>
                <div class="fx-row">
                    <span class="lbl">Output</span>
                    <select class="field-input" value={$engineDraft.output}
                            on:change={(e) => engineDraft.update(c => ({ ...c, output: selValue(e) }))}>
                        {#each OUTPUT_MODES as o}<option value={o.id}>{o.label}</option>{/each}
                    </select>
                </div>
            {/if}

            {#each items as a}
                <div class="fx-advice {a.level}"><span>{a.level === 'error' ? '✖' : a.level === 'warn' ? '⚠' : 'ℹ'}</span>{a.message}</div>
            {/each}
            <p class="fx-note">Assign ports (motors, servos, LEDs) and fine-tune in the <button class="linkish" on:click={() => openPanel(f.tabKind)}>{f.label} panel</button>.</p>
        </div>
    {/each}
{/if}

<style>
    .fx-card { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-input); padding: 12px 14px; margin-bottom: 10px; }
    .fx-head { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
    .fx-icon { font-size: 16px; }
    .fx-name { font-size: 13px; font-weight: 700; color: var(--text-bright); }
    .fx-open { margin-left: auto; }
    .fx-row { display: flex; align-items: center; gap: 10px; margin: 6px 0; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 120px; }
    .fx-row .field-input { flex: 1; max-width: 300px; }
    .fx-advice { font-size: 12px; display: flex; gap: 8px; padding: 4px 0; }
    .fx-advice.warn { color: var(--warning); }
    .fx-advice.error { color: var(--error); }
    .fx-advice.info { color: var(--text-dim); }
    .fx-note { font-size: 11px; color: var(--text-dim); margin: 8px 0 0; }
    .muted { color: var(--text-dim); font-size: 12px; }
    .linkish { background: none; border: none; color: var(--accent); cursor: pointer; padding: 0; font: inherit; text-decoration: underline; }
</style>
