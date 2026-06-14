<!-- ConfigWizard.svelte — Setup Wizard modal shell (config-wizard).
     Left step rail + body + footer.  Steps are DYNAMIC (wizardSteps): the
     head (features/input/channels), one dedicated step per ENABLED effect,
     then review.  See instructions/33-CONFIG-WIZARD.md. -->
<script lang="ts">
    import { showConfigWizard } from '../stores'
    import { svgIcon, WAND_PATHS } from '../icons'
    import {
        wizardSteps, wizardStep, nextStep, prevStep, gotoStep,
        closeWizard, isFirstStep, isLastStep,
    } from '../wizard'
    import WizardStepFeatures from '../wizard/WizardStepFeatures.svelte'
    import WizardStepInput from '../wizard/WizardStepInput.svelte'
    import WizardStepChannels from '../wizard/WizardStepChannels.svelte'
    import WizardStepReview from '../wizard/WizardStepReview.svelte'
    import WizardEffectEngine from '../wizard/WizardEffectEngine.svelte'
    import WizardEffectGear from '../wizard/WizardEffectGear.svelte'
    import WizardEffectGun from '../wizard/WizardEffectGun.svelte'
    import WizardEffectLanding from '../wizard/WizardEffectLanding.svelte'
    import WizardEffectLighting from '../wizard/WizardEffectLighting.svelte'

    // Clamp the index if the step list shrank (a feature was disabled).
    $: if ($wizardStep >= $wizardSteps.length) gotoStep($wizardSteps.length - 1)
    $: step = Math.min($wizardStep, $wizardSteps.length - 1)
    $: current = $wizardSteps[step]
</script>

{#if $showConfigWizard}
<!-- svelte-ignore a11y-click-events-have-key-events -->
<div class="wiz-backdrop" on:click|self={closeWizard}>
    <div class="wiz">
        <div class="wiz-header">
            <div class="wiz-title">
                <span class="wiz-wand">{@html svgIcon(WAND_PATHS, 20)}</span>
                <div>
                    <h2>ScaleFX Setup Wizard</h2>
                    <p class="wiz-sub">Guided setup — pick features, set up the radio, configure each effect.</p>
                </div>
            </div>
            <button class="wiz-close" on:click={closeWizard} title="Close (your changes aren't applied until the last step)">✕</button>
        </div>

        <div class="wiz-body">
            <!-- Left rail: step list -->
            <nav class="wiz-rail">
                {#each $wizardSteps as s, i}
                    <button class="wiz-step" class:active={i === step} class:done={i < step}
                            on:click={() => gotoStep(i)}>
                        <span class="wiz-num">{i < step ? '✓' : i + 1}</span>
                        <span class="wiz-step-label">{s.title}</span>
                    </button>
                {/each}
            </nav>

            <!-- Step surface -->
            <section class="wiz-surface">
                {#if current}
                    <header class="wiz-step-head">
                        <h3>{current.title}</h3>
                        <p>{current.blurb}</p>
                    </header>
                    <div class="wiz-step-content">
                        {#if current.id === 'features'}<WizardStepFeatures />
                        {:else if current.id === 'input'}<WizardStepInput />
                        {:else if current.id === 'channels'}<WizardStepChannels />
                        {:else if current.id === 'review'}<WizardStepReview />
                        {:else if current.feature === 'engine'}<WizardEffectEngine />
                        {:else if current.feature === 'gear'}<WizardEffectGear />
                        {:else if current.feature === 'gun'}<WizardEffectGun />
                        {:else if current.feature === 'landing'}<WizardEffectLanding />
                        {:else if current.feature === 'lighting'}<WizardEffectLighting />
                        {/if}
                    </div>
                {/if}
            </section>
        </div>

        <div class="wiz-footer">
            <button class="small" on:click={closeWizard}>Cancel</button>
            <div class="wiz-foot-right">
                <button class="small" on:click={prevStep} disabled={isFirstStep(step)}>← Back</button>
                {#if isLastStep(step)}
                    <button class="small" on:click={closeWizard}>Done</button>
                {:else}
                    <button class="small primary" on:click={nextStep}>Next →</button>
                {/if}
            </div>
        </div>
    </div>
</div>
{/if}

<style>
    .wiz-backdrop {
        position: fixed; inset: 0; z-index: 200;
        background: rgba(0,0,0,0.55);
        display: flex; align-items: center; justify-content: center;
    }
    .wiz {
        width: min(960px, 92vw); height: min(680px, 88vh);
        display: flex; flex-direction: column;
        background: var(--bg-surface); border: 1px solid var(--border);
        border-radius: 8px; overflow: hidden;
        box-shadow: 0 12px 48px rgba(0,0,0,0.5);
    }
    .wiz-header {
        display: flex; align-items: center; justify-content: space-between;
        padding: 14px 18px; border-bottom: 1px solid var(--border);
        background: var(--bg-raised);
    }
    .wiz-title { display: flex; align-items: center; gap: 12px; }
    .wiz-wand { display: flex; align-items: center; color: var(--accent); }
    .wiz-header h2 { font-size: 15px; font-weight: 700; color: var(--text-bright); margin: 0; }
    .wiz-sub { font-size: 11px; color: var(--text-dim); margin: 2px 0 0; }
    .wiz-close {
        background: none; border: none; border-radius: 4px;
        color: var(--text-dim); width: 30px; height: 30px; cursor: pointer; font-size: 16px;
    }
    .wiz-close:hover { color: var(--text-bright); background: var(--bg-input); }

    .wiz-body { flex: 1; display: flex; min-height: 0; }

    .wiz-rail {
        width: 200px; flex-shrink: 0; padding: 12px 8px;
        border-right: 1px solid var(--border); background: var(--bg-raised);
        display: flex; flex-direction: column; gap: 2px;
    }
    .wiz-step {
        display: flex; align-items: center; gap: 10px;
        padding: 9px 10px; border-radius: 6px; cursor: pointer;
        background: none; border: none; text-align: left;
        color: var(--text-dim); font-size: 12px;
    }
    .wiz-step:hover { background: var(--bg-input); }
    .wiz-step.active { background: var(--bg-input); color: var(--text-bright); font-weight: 600; }
    .wiz-step.done { color: var(--success); }
    .wiz-num {
        flex-shrink: 0; width: 22px; height: 22px; border-radius: 50%;
        display: inline-flex; align-items: center; justify-content: center;
        font-size: 11px; font-family: var(--font-mono);
        border: 1px solid var(--border); background: var(--bg-surface);
    }
    .wiz-step.active .wiz-num { border-color: var(--accent); color: var(--accent); }
    .wiz-step.done .wiz-num { border-color: var(--success); color: var(--success); }

    .wiz-surface { flex: 1; display: flex; flex-direction: column; min-width: 0; padding: 18px 22px; overflow: auto; }
    .wiz-step-head h3 { font-size: 16px; font-weight: 700; color: var(--text-bright); margin: 0 0 4px; }
    .wiz-step-head p { font-size: 12px; color: var(--text-dim); margin: 0 0 14px; }
    .wiz-step-content { flex: 1; }

    .wiz-footer {
        display: flex; align-items: center; justify-content: space-between;
        padding: 12px 18px; border-top: 1px solid var(--border);
        background: var(--bg-raised);
    }
    .wiz-foot-right { display: flex; gap: 8px; }
    .wiz-footer button { height: 30px; min-width: 92px; box-sizing: border-box; }
</style>
