<!-- WizardStepReview — Step 5: summarise the config + apply it to the board.
     Apply runs the global applyAll() — the SAME ordered hub-first apply the
     toolbar uses (Rule 46) — so the wizard's draft edits land like any other. -->
<script lang="ts">
    import { deviceModel } from '../devicemodel'
    import { applyAll, anyErrors, dirtyLabels } from '../dirty-registry'
    import { collectChannelOptions } from '../channels'
    import { WIZARD_FEATURES } from '../wizard-features'
    import { analyzeWizard } from '../wizard-advice'
    import { closeWizard } from '../wizard'
    import { engineDraft } from '../effects'
    import { gunfxDraft } from '../gunfx'
    import { lightfxDraft } from '../lightfx'
    import { landingDraft } from '../landing'
    import { gearDraft } from '../gear'

    $: _track = [$engineDraft, $gunfxDraft, $lightfxDraft, $landingDraft, $gearDraft]
    $: enabled = (_track, WIZARD_FEATURES.filter(f => f.isEnabled()))
    $: advice = analyzeWizard($deviceModel)
    $: fnLabel = new Map($deviceModel.channelFunctions.map(f => [f.id, f.label] as const))
    $: mapped = new Set(collectChannelOptions($deviceModel).map(o => o.fnId))

    let busy = false
    let result: { ok: boolean; msg: string } | null = null

    async function apply() {
        busy = true; result = null
        try {
            await applyAll()
            result = { ok: true, msg: 'Applied — every config saved and reloaded on the board.' }
        } catch (e) {
            result = { ok: false, msg: String(e) }
        } finally { busy = false }
    }
</script>

<div class="rev">
    <div class="rev-summary">
        <div class="rev-head">Effects</div>
        {#if enabled.length === 0}
            <p class="muted">None enabled.</p>
        {:else}
            {#each enabled as f}
                {@const bound = f.getInput()}
                <div class="rev-fx">
                    <span class="rev-icon">{f.icon}</span>
                    <span class="rev-name">{f.label}</span>
                    <span class="rev-ch">
                        {#if bound}
                            {mapped.has(bound) ? '📡' : '⚠'} {fnLabel.get(bound) ?? bound}
                        {:else}
                            ✋ manual
                        {/if}
                    </span>
                </div>
            {/each}
        {/if}

        {#if advice.filter(a => a.level !== 'info').length}
            <div class="rev-head warn-head">Before you apply</div>
            {#each advice.filter(a => a.level !== 'info') as a}
                <div class="rev-advice {a.level}"><span>{a.level === 'error' ? '✖' : '⚠'}</span>{a.message}</div>
            {/each}
        {/if}
    </div>

    <div class="rev-apply">
        {#if $dirtyLabels.length}
            <p class="muted">Will save + reload: <strong>{$dirtyLabels.join(', ')}</strong></p>
        {:else}
            <p class="muted">Nothing changed — already in sync with the board.</p>
        {/if}
        <button class="primary big" on:click={apply}
                disabled={busy || $anyErrors || $dirtyLabels.length === 0}
                title={$anyErrors ? 'Resolve validation errors first' : $dirtyLabels.length === 0 ? 'No changes to apply' : 'Save + reload every changed config on the board'}>
            {busy ? 'Applying…' : '✓ Apply to board'}
        </button>
        {#if result}
            <div class="rev-result {result.ok ? 'ok' : 'err'}">{result.msg}</div>
            {#if result.ok}<button class="small" on:click={closeWizard}>Close wizard</button>{/if}
        {/if}
    </div>
</div>

<style>
    .rev { display: flex; gap: 18px; flex-wrap: wrap; }
    .rev-summary { flex: 1 1 320px; }
    .rev-apply { flex: 1 1 240px; display: flex; flex-direction: column; gap: 10px; align-items: flex-start; }
    .rev-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin: 0 0 6px; }
    .rev-head.warn-head { color: var(--warning); margin-top: 14px; }
    .rev-fx { display: flex; align-items: center; gap: 8px; padding: 5px 8px; border: 1px solid var(--border); border-radius: 5px; background: var(--bg-input); margin-bottom: 5px; }
    .rev-icon { font-size: 14px; }
    .rev-name { font-size: 12px; font-weight: 600; color: var(--text-bright); }
    .rev-ch { margin-left: auto; font-size: 11px; font-family: var(--font-mono); color: var(--text-dim); }
    .rev-advice { font-size: 12px; display: flex; gap: 8px; padding: 3px 0; }
    .rev-advice.warn { color: var(--warning); }
    .rev-advice.error { color: var(--error); }
    .primary.big { height: 38px; padding: 0 20px; font-size: 13px; }
    .rev-result { font-size: 12px; padding: 8px 10px; border-radius: 5px; }
    .rev-result.ok { color: var(--success); background: color-mix(in srgb, var(--success) 12%, transparent); }
    .rev-result.err { color: var(--error); background: color-mix(in srgb, var(--error) 12%, transparent); }
    .muted { color: var(--text-dim); font-size: 12px; }
</style>
