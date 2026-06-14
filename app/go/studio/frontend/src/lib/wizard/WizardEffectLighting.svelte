<!-- WizardEffectLighting — master brightness, LED channels (auto-attaching),
     program selector, and a one-click default program.  The full timeline
     editor stays in the Lighting panel; the wizard gets you a working setup. -->
<script lang="ts">
    import { RoleKind } from '../devicemodel'
    import {
        lightfxDraft, setMasterBrightness, addChannel, removeChannel, updateChannel,
        setChannelPort, setChannelBrightness, setSelectorInput, setSelectorHysteresis, addBlankActive,
    } from '../lightfx'
    import WizardChannelSelect from './WizardChannelSelect.svelte'
    import WizardPortPicker from './WizardPortPicker.svelte'
    import type { PortRefT } from '../wizard-ports'

    const numValue = (e: Event) => Number((e.target as HTMLInputElement).value)
    const strVal   = (e: Event) => (e.target as HTMLInputElement).value
</script>

<div class="fx-sec">
    <div class="fx-sec-head">Master</div>
    <div class="fx-row"><span class="lbl">Brightness</span>
        <input type="range" min="0" max="100" value={$lightfxDraft.masterBrightnessPct}
               on:input={(e) => setMasterBrightness(Math.round(numValue(e)))} />
        <span class="unit">{$lightfxDraft.masterBrightnessPct}%</span>
    </div>
</div>

<div class="fx-sec">
    <div class="fx-sec-head">LED channels <button class="small add" on:click={() => addChannel()}>+ Add channel</button></div>
    {#if $lightfxDraft.channels.length === 0}<p class="muted">No channels — add one per LED output you want to drive.</p>{/if}
    {#each ($lightfxDraft.channels) as ch, idx (idx)}
        <div class="chan">
            <input class="field-input nm" type="text" value={ch.name} placeholder="channel name (e.g. nav, strobe)"
                   on:change={(e) => updateChannel(idx, { name: strVal(e) })} />
            <button class="small danger rm" on:click={() => removeChannel(idx)}>×</button>
            <WizardPortPicker label="LED port" kind="pwm" role={RoleKind.LedAnimator} value={ch.port}
                onChange={(ref) => setChannelPort(idx, ref)} />
            <div class="fx-row"><span class="lbl">Default brightness</span>
                <input class="field-input narrow" type="number" min="0" max="100" step="5" value={ch.defaultBrightnessPct}
                       on:change={(e) => setChannelBrightness(idx, Math.round(numValue(e)))} /><span class="unit">%</span>
            </div>
        </div>
    {/each}
</div>

<div class="fx-sec">
    <div class="fx-sec-head">Program selector (optional)</div>
    <WizardChannelSelect label="Select channel" value={$lightfxDraft.programSelector.input}
        onChange={(fn) => setSelectorInput(fn)} />
    <div class="fx-row"><span class="lbl">Hysteresis</span>
        <input class="field-input narrow" type="number" min="0" max="500" step="10" value={$lightfxDraft.programSelector.hysteresisUs}
               on:change={(e) => setSelectorHysteresis(Math.round(numValue(e)))} /><span class="unit">µs</span>
    </div>
</div>

<div class="fx-sec">
    <div class="fx-sec-head">Programs</div>
    <p class="muted">{$lightfxDraft.activePrograms?.length ?? 0} program(s). Add a blank one to start, then design the timeline in the Lighting tab.</p>
    <button class="small add" on:click={() => addBlankActive()}>+ Add blank program</button>
</div>

<style>
    .fx-sec { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 8px 10px; margin-bottom: 8px; }
    .fx-sec-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 6px; display: flex; gap: 10px; align-items: center; }
    .fx-row { display: flex; align-items: center; gap: 8px; margin: 5px 0; flex-wrap: wrap; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .field-input.narrow { width: 70px; }
    .field-input.nm { flex: 1; min-width: 180px; }
    .fx-row input[type=range] { flex: 1; max-width: 220px; }
    .chan { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-surface); padding: 8px 10px; margin: 6px 0; display: flex; flex-wrap: wrap; align-items: center; gap: 8px; }
    .add { font-size: 10px; }
    .muted { color: var(--text-dim); font-size: 12px; }
</style>
