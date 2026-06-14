<!-- WizardEffectLanding — retractable searchlights: per light, servo(s) +
     LED(s) (auto-attaching) + fade-in + activation. -->
<script lang="ts">
    import { RoleKind } from '../devicemodel'
    import { landingDraft, addLandingLight, removeLandingLight, updateLandingLight, type LandingLightT } from '../landing'
    import WizardChannelSelect from './WizardChannelSelect.svelte'
    import WizardPortPicker from './WizardPortPicker.svelte'
    import { emptyRef, type PortRefT } from '../wizard-ports'

    const numValue = (e: Event) => Number((e.target as HTMLInputElement).value)
    const strVal   = (e: Event) => (e.target as HTMLInputElement).value
    const up = (id: number, m: (l: LandingLightT) => LandingLightT) => updateLandingLight(id, m)
</script>

<div class="fx-top"><button class="small add" on:click={() => addLandingLight()}>+ Add light</button></div>
{#if $landingDraft.lights.length === 0}<p class="muted">No lights — add one.</p>{/if}

{#each ($landingDraft.lights) as l (l.id)}
    <div class="light">
        <div class="light-head">
            <input class="field-input nm" type="text" value={l.name} placeholder="light name"
                   on:change={(e) => up(l.id, x => ({ ...x, name: strVal(e) }))} />
            <button class="small danger rm" on:click={() => removeLandingLight(l.id)}>× Remove</button>
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Servos <button class="small add" on:click={() => up(l.id, x => ({ ...x, servos: [...x.servos, { port: emptyRef('servo') }] }))}>+ Add</button></div>
            {#each (l.servos) as s, si (si)}
                <div class="row2">
                    <WizardPortPicker label={`Servo ${si + 1}`} kind="servo" role={RoleKind.ServoActuator} value={s.port}
                        onChange={(ref) => up(l.id, x => ({ ...x, servos: x.servos.map((ss, i) => i === si ? { port: ref } : ss) }))} />
                    <button class="small danger rm" on:click={() => up(l.id, x => ({ ...x, servos: x.servos.filter((_, i) => i !== si) }))}>×</button>
                </div>
            {/each}
            {#if l.servos.length === 0}<p class="muted small">no servo = LEDs only (always on)</p>{/if}
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">LEDs <button class="small add" on:click={() => up(l.id, x => ({ ...x, leds: [...x.leds, { port: emptyRef('pwm'), brightnessPct: 100 }] }))}>+ Add</button></div>
            {#each (l.leds) as led, li (li)}
                <div class="row2">
                    <WizardPortPicker label={`LED ${li + 1}`} kind="pwm" role={RoleKind.LedAnimator} value={led.port}
                        onChange={(ref) => up(l.id, x => ({ ...x, leds: x.leds.map((ll, i) => i === li ? { ...ll, port: ref } : ll) }))} />
                    <input class="field-input narrow" type="number" min="0" max="100" step="5" value={led.brightnessPct}
                           on:change={(e) => up(l.id, x => ({ ...x, leds: x.leds.map((ll, i) => i === li ? { ...ll, brightnessPct: Math.round(numValue(e)) } : ll) }))} /><span class="unit">%</span>
                    <button class="small danger rm" on:click={() => up(l.id, x => ({ ...x, leds: x.leds.filter((_, i) => i !== li) }))}>×</button>
                </div>
            {/each}
            {#if l.leds.length === 0}<div class="fx-warn">⚠ Add at least one LED — a light with no LEDs does nothing.</div>{/if}
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Behaviour</div>
            <div class="fx-row"><span class="lbl">LED fade-in</span>
                <input class="field-input narrow" type="number" min="0" max="5000" step="50" value={l.fadeInMs}
                       on:change={(e) => up(l.id, x => ({ ...x, fadeInMs: Math.round(numValue(e)) }))} /><span class="unit">ms after deploy</span>
            </div>
            <div class="fx-row"><span class="lbl">Activation</span>
                <div class="seg">
                    <button class="seg-btn" class:on={l.activation.mode === 'manual'} on:click={() => up(l.id, x => ({ ...x, activation: { ...x.activation, mode: 'manual' } }))}>Manual</button>
                    <button class="seg-btn" class:on={l.activation.mode === 'input_channel'} on:click={() => up(l.id, x => ({ ...x, activation: { ...x.activation, mode: 'input_channel' } }))}>RC channel</button>
                </div>
            </div>
            {#if l.activation.mode === 'input_channel'}
                <WizardChannelSelect label="Deploy channel" value={l.activation.input}
                    onChange={(fn) => up(l.id, x => ({ ...x, activation: { ...x.activation, input: fn } }))} />
                <div class="fx-row"><span class="lbl">Threshold</span>
                    <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={l.activation.thresholdUs}
                           on:change={(e) => up(l.id, x => ({ ...x, activation: { ...x.activation, thresholdUs: Math.round(numValue(e)) } }))} /><span class="unit">µs</span>
                </div>
            {/if}
        </div>
    </div>
{/each}

<style>
    .fx-top { margin-bottom: 6px; }
    .light { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-surface); padding: 8px 10px; margin-bottom: 12px; }
    .light-head { display: flex; gap: 8px; align-items: center; margin-bottom: 8px; }
    .fx-sec { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 8px 10px; margin-bottom: 8px; }
    .fx-sec-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 6px; display: flex; gap: 10px; align-items: center; }
    .fx-row { display: flex; align-items: center; gap: 8px; margin: 5px 0; flex-wrap: wrap; }
    .row2 { display: flex; align-items: center; gap: 6px; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .field-input.narrow { width: 70px; }
    .field-input.nm { flex: 1; min-width: 180px; }
    .seg { display: inline-flex; gap: 3px; }
    .seg-btn { font-size: 11px; padding: 4px 10px; border: 1px solid var(--border); background: var(--bg-input); color: var(--text-dim); cursor: pointer; border-radius: 4px; }
    .seg-btn.on { border-color: var(--accent); color: var(--accent); background: color-mix(in srgb, var(--accent) 12%, var(--bg-input)); }
    .add { font-size: 10px; }
    .fx-warn { font-size: 11px; color: var(--warning); }
    .muted { color: var(--text-dim); font-size: 12px; }
    .muted.small { font-size: 11px; }
</style>
