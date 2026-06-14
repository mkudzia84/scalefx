<!-- WizardStepFeatures — Step 1: pick the effects to use.
     Toggles each effect's existing draft `enabled` (Rule 46), capability-gated
     by the hub's advertised domains.  Enabling seeds a sensible starter block
     (the effect's own default factory) so nothing is empty. -->
<script lang="ts">
    import { deviceModel } from '../devicemodel'
    import { WIZARD_FEATURES, availableFeatures } from '../wizard-features'
    // Subscribe to every effect draft so the on/off state is reactive.
    import { engineDraft } from '../effects'
    import { gunfxDraft } from '../gunfx'
    import { lightfxDraft } from '../lightfx'
    import { landingDraft } from '../landing'
    import { gearDraft } from '../gear'

    $: features = availableFeatures($deviceModel)

    // Recompute the enabled map whenever any draft changes (the store refs in
    // the args make Svelte track them; the registry reads them via get()).
    function enabledMap(..._deps: unknown[]): Record<string, boolean> {
        return Object.fromEntries(WIZARD_FEATURES.map(f => [f.id, f.isEnabled()]))
    }
    $: enabled = enabledMap($engineDraft, $gunfxDraft, $lightfxDraft, $landingDraft, $gearDraft)
</script>

<div class="feat-grid">
    {#each features as f (f.id)}
        <button class="feat-card" class:on={enabled[f.id]}
                on:click={() => f.setEnabled(!enabled[f.id])}>
            <div class="feat-top">
                <span class="feat-icon">{f.icon}</span>
                <span class="feat-name">{f.label}</span>
                <span class="feat-toggle" class:on={enabled[f.id]}>{enabled[f.id] ? 'ON' : 'OFF'}</span>
            </div>
            <p class="feat-blurb">{f.blurb}</p>
            {#if f.inputs.length}
                <p class="feat-uses">uses: {f.inputs.map(i => i.label).join(' · ')}</p>
            {/if}
        </button>
    {/each}
</div>

{#if features.length === 0}
    <p class="muted">No effect domains advertised by this board yet — connect a HubFX to see the full set.</p>
{/if}
<p class="muted hint">Enabling an effect seeds a sensible starter config. You'll wire its radio channel and fine-tune ports in the next steps and panels.</p>

<style>
    .feat-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(230px, 1fr)); gap: 10px; }
    .feat-card {
        text-align: left; cursor: pointer; padding: 12px 14px;
        background: var(--bg-input); border: 1px solid var(--border); border-radius: 8px;
        color: var(--text); transition: border-color .12s, background .12s;
    }
    .feat-card:hover { border-color: var(--text-dim); }
    .feat-card.on { border-color: var(--accent); background: color-mix(in srgb, var(--accent) 10%, var(--bg-input)); }
    .feat-top { display: flex; align-items: center; gap: 8px; }
    .feat-icon { font-size: 18px; }
    .feat-name { font-size: 13px; font-weight: 700; color: var(--text-bright); }
    .feat-toggle {
        margin-left: auto; font-size: 10px; font-family: var(--font-mono); font-weight: 700;
        padding: 2px 7px; border-radius: 4px; border: 1px solid var(--border); color: var(--text-dim);
    }
    .feat-toggle.on { color: var(--accent); border-color: var(--accent); }
    .feat-blurb { font-size: 11px; color: var(--text-dim); margin: 8px 0 0; line-height: 1.4; }
    .feat-uses { font-size: 10px; color: var(--text-dim); margin: 6px 0 0; font-family: var(--font-mono); opacity: 0.8; }
    .muted { color: var(--text-dim); font-size: 12px; }
    .hint { margin-top: 16px; }
</style>
