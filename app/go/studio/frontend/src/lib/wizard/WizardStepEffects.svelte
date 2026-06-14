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
    import { engineDraft, ENGINE_TYPES, OUTPUT_MODES, FAILSAFE_MODES } from '../effects'
    import { gunfxDraft } from '../gunfx'
    import { lightfxDraft } from '../lightfx'
    import { landingDraft } from '../landing'
    import { gearDraft } from '../gear'

    const GEAR_COORDS = [
        { id: 'independent', label: 'Independent' },
        { id: 'full_sync',   label: 'Full-sync' },
    ] as const

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
    const numValue = (e: Event) => Number((e.target as HTMLInputElement).value)
    const boolValue = (e: Event) => (e.target as HTMLInputElement).checked

    function openPanel(kind: string) {
        const tabs = get(studioTabs)
        const idx = tabs.findIndex(t => t.kind === kind)
        if (idx >= 0) activeTab.set(idx)
        closeWizard()
    }

    // ── Simple per-effect param setters (mutate the existing drafts) ──
    const setEngine = (p: any) => engineDraft.update(c => ({ ...c, ...p }))
    const setEngineToggle = (p: any) => engineDraft.update(c => ({ ...c, toggle: { ...c.toggle, ...p } }))
    const setGear = (p: any) => gearDraft.update(c => ({ ...c, ...p }))
    const setGearInput = (p: any) => gearDraft.update(c => ({ ...c, input: { ...c.input, ...p } }))
    const setGunTrigger = (p: any) => gunfxDraft.update(c => {
        const g = [...c.guns]; if (g[0]) g[0] = { ...g[0], trigger: { ...g[0].trigger, ...p } }; return { ...c, guns: g }
    })
    const toggleGunSub = (part: 'recoil' | 'yaw' | 'pitch') => gunfxDraft.update(c => {
        const g = [...c.guns]
        if (g[0]) g[0] = { ...g[0], [part]: { ...(g[0] as any)[part], enabled: !(g[0] as any)[part].enabled } }
        return { ...c, guns: g }
    })
    const setLight = (p: any) => lightfxDraft.update(c => ({ ...c, ...p }))
    const setLanding0 = (p: any) => landingDraft.update(c => {
        const l = [...c.lights]; if (l[0]) l[0] = { ...l[0], ...p }; return { ...c, lights: l }
    })
    const setLanding0Act = (p: any) => landingDraft.update(c => {
        const l = [...c.lights]; if (l[0]) l[0] = { ...l[0], activation: { ...l[0].activation, ...p } }; return { ...c, lights: l }
    })
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
                    <select class="field-input" value={$engineDraft.type} on:change={(e) => setEngine({ type: selValue(e) })}>
                        {#each ENGINE_TYPES as t}<option value={t.id}>{t.label}</option>{/each}
                    </select>
                </div>
                <div class="fx-row">
                    <span class="lbl">Speakers</span>
                    <select class="field-input" value={$engineDraft.output} on:change={(e) => setEngine({ output: selValue(e) })}>
                        {#each OUTPUT_MODES as o}<option value={o.id}>{o.label}</option>{/each}
                    </select>
                </div>
                <div class="fx-row">
                    <span class="lbl">On/off threshold</span>
                    <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={$engineDraft.toggle.thresholdUs}
                           on:change={(e) => setEngineToggle({ thresholdUs: Math.round(numValue(e)) })} />
                    <span class="unit">µs · on RC loss</span>
                    <select class="field-input" value={$engineDraft.toggle.failsafe} on:change={(e) => setEngineToggle({ failsafe: selValue(e) })}>
                        {#each FAILSAFE_MODES as m}<option value={m.id}>{m.label}</option>{/each}
                    </select>
                </div>
            {:else if f.id === 'gear'}
                <div class="fx-row">
                    <span class="lbl">Coordination</span>
                    <div class="seg">
                        {#each GEAR_COORDS as c}
                            <button class="seg-btn" class:on={$gearDraft.coord === c.id} on:click={() => setGear({ coord: c.id })}>{c.label}</button>
                        {/each}
                    </div>
                </div>
                <div class="fx-row">
                    <span class="lbl">Up/down threshold</span>
                    <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={$gearDraft.input.thresholdUs}
                           on:change={(e) => setGearInput({ thresholdUs: Math.round(numValue(e)) })} />
                    <span class="unit">µs (switch ON = gear down)</span>
                </div>
                <label class="fx-check"><input type="checkbox" checked={$gearDraft.deployOnConnectionLoss}
                        on:change={(e) => setGear({ deployOnConnectionLoss: boolValue(e) })} /> Deploy the gear if the radio link drops (safe failsafe)</label>
            {:else if f.id === 'gun'}
                <div class="fx-row">
                    <span class="lbl">Trigger threshold</span>
                    <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={$gunfxDraft.guns[0]?.trigger.thresholdUs ?? 1500}
                           on:change={(e) => setGunTrigger({ thresholdUs: Math.round(numValue(e)) })} />
                    <span class="unit">µs</span>
                </div>
                <div class="fx-row">
                    <span class="lbl">Add-ons</span>
                    <div class="seg">
                        <button class="seg-btn" class:on={$gunfxDraft.guns[0]?.recoil.enabled} on:click={() => toggleGunSub('recoil')}>Recoil kick</button>
                        <button class="seg-btn" class:on={$gunfxDraft.guns[0]?.yaw.enabled} on:click={() => toggleGunSub('yaw')}>Turret yaw</button>
                        <button class="seg-btn" class:on={$gunfxDraft.guns[0]?.pitch.enabled} on:click={() => toggleGunSub('pitch')}>Turret pitch</button>
                    </div>
                </div>
            {:else if f.id === 'lighting'}
                <div class="fx-row">
                    <span class="lbl">Master brightness</span>
                    <input type="range" min="0" max="100" value={$lightfxDraft.masterBrightnessPct}
                           on:input={(e) => setLight({ masterBrightnessPct: Math.round(numValue(e)) })} />
                    <span class="unit">{$lightfxDraft.masterBrightnessPct}%</span>
                </div>
            {:else if f.id === 'landing'}
                <div class="fx-row">
                    <span class="lbl">LED fade-in</span>
                    <input class="field-input narrow" type="number" min="0" max="5000" step="50" value={$landingDraft.lights[0]?.fadeInMs ?? 400}
                           on:change={(e) => setLanding0({ fadeInMs: Math.round(numValue(e)) })} />
                    <span class="unit">ms after the servo deploys</span>
                </div>
                <div class="fx-row">
                    <span class="lbl">Deploy threshold</span>
                    <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={$landingDraft.lights[0]?.activation.thresholdUs ?? 1500}
                           on:change={(e) => setLanding0Act({ thresholdUs: Math.round(numValue(e)) })} />
                    <span class="unit">µs</span>
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
    .fx-card { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 12px 14px; margin-bottom: 10px; }
    .fx-head { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
    .fx-icon { font-size: 16px; }
    .fx-name { font-size: 13px; font-weight: 700; color: var(--text-bright); }
    .fx-open { margin-left: auto; }
    .fx-row { display: flex; align-items: center; gap: 10px; margin: 6px 0; flex-wrap: wrap; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 120px; }
    .fx-row .field-input { flex: 0 1 200px; max-width: 240px; }
    .fx-row .field-input.narrow { flex: 0 0 84px; width: 84px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .seg { display: inline-flex; gap: 3px; flex-wrap: wrap; }
    .seg-btn { font-size: 11px; padding: 4px 10px; border: 1px solid var(--border); background: var(--bg-input); color: var(--text-dim); cursor: pointer; border-radius: 4px; }
    .seg-btn.on { border-color: var(--accent); color: var(--accent); background: color-mix(in srgb, var(--accent) 12%, var(--bg-input)); }
    .fx-check { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--text-dim); margin: 8px 0 0; cursor: pointer; }
    .fx-row input[type=range] { flex: 1; max-width: 200px; }
    .fx-advice { font-size: 12px; display: flex; gap: 8px; padding: 4px 0; }
    .fx-advice.warn { color: var(--warning); }
    .fx-advice.error { color: var(--error); }
    .fx-advice.info { color: var(--text-dim); }
    .fx-note { font-size: 11px; color: var(--text-dim); margin: 8px 0 0; }
    .muted { color: var(--text-dim); font-size: 12px; }
    .linkish { background: none; border: none; color: var(--accent); cursor: pointer; padding: 0; font: inherit; text-decoration: underline; }
</style>
