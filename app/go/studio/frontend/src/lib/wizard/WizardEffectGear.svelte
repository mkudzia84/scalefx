<!-- WizardEffectGear — full undercarriage setup: coordination + RC up/down +
     per-strut motor (auto-attaches BiDcMotor) and doors (auto-attaches
     ServoActuator) with sequencing. -->
<script lang="ts">
    import { RoleKind } from '../devicemodel'
    import {
        gearDraft, setGearCoord, updateGearInput, setGearDeployOnConnLoss,
        addGearChannel, removeGearChannel, updateGearChannel, addGearDoor, removeGearDoor,
        type GearChannelT,
    } from '../gear'
    import WizardChannelSelect from './WizardChannelSelect.svelte'
    import WizardPortPicker from './WizardPortPicker.svelte'
    import type { PortRefT } from '../wizard-ports'

    const COORDS = [{ id: 'independent', label: 'Independent' }, { id: 'full_sync', label: 'Full-sync' }] as const
    const DOOR_MODES = [{ id: 'sync', label: 'Together' }, { id: 'delay', label: 'Staggered' }, { id: 'sequence', label: 'One, then other' }] as const
    const CLOSE = [{ id: 'both', label: 'Both close' }, { id: 'first', label: 'One closes' }, { id: 'none', label: 'Stay open' }] as const

    const numValue = (e: Event) => Number((e.target as HTMLInputElement).value)
    const strVal   = (e: Event) => (e.target as HTMLInputElement).value
    const boolValue = (e: Event) => (e.target as HTMLInputElement).checked
    const fwd = (g: GearChannelT) => !g.reverse
    function setDir(id: number, forward: boolean) {
        updateGearChannel(id, g => ({ ...g, reverse: !forward }))
    }
</script>

<div class="fx-sec">
    <div class="fx-sec-head">Coordination</div>
    <div class="seg">
        {#each COORDS as c}
            <button class="seg-btn" class:on={$gearDraft.coord === c.id} on:click={() => setGearCoord(c.id)}>{c.label}</button>
        {/each}
    </div>
</div>

<div class="fx-sec">
    <div class="fx-sec-head">RC up/down</div>
    <WizardChannelSelect label="Channel" value={$gearDraft.input.name}
        onChange={(fn) => updateGearInput(i => ({ ...i, name: fn }))} />
    <div class="fx-row">
        <span class="lbl">Threshold</span>
        <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={$gearDraft.input.thresholdUs}
               on:change={(e) => updateGearInput(i => ({ ...i, thresholdUs: Math.round(numValue(e)) }))} />
        <span class="unit">µs (switch ON = gear down)</span>
    </div>
    <label class="fx-check"><input type="checkbox" checked={$gearDraft.deployOnConnectionLoss}
            on:change={(e) => setGearDeployOnConnLoss(boolValue(e))} /> Deploy the gear if the radio link drops (safe failsafe)</label>
</div>

<div class="fx-sec">
    <div class="fx-sec-head">Struts <button class="small add" on:click={() => addGearChannel()}>+ Add strut</button></div>
    {#if $gearDraft.gears.length === 0}<p class="muted">No struts — add one.</p>{/if}
    {#each ($gearDraft.gears) as g (g.id)}
        <div class="strut">
            <div class="strut-head">
                <input class="field-input nm" type="text" value={g.name} placeholder="strut name"
                       on:change={(e) => updateGearChannel(g.id, x => ({ ...x, name: strVal(e) }))} />
                <button class="small danger rm" on:click={() => removeGearChannel(g.id)}>×</button>
            </div>
            <WizardPortPicker label="Motor" kind="hbridge" role={RoleKind.BiDcMotor} value={g.motor}
                onChange={(ref) => updateGearChannel(g.id, x => ({ ...x, motor: ref }))} />
            <div class="fx-row">
                <span class="lbl">Deploy drives</span>
                <div class="seg">
                    <button class="seg-btn" class:on={fwd(g)} on:click={() => setDir(g.id, true)}>Forward</button>
                    <button class="seg-btn" class:on={!fwd(g)} on:click={() => setDir(g.id, false)}>Reverse</button>
                </div>
                <span class="lbl2">Timeout</span>
                <input class="field-input narrow" type="number" min="0" max="60000" step="500" value={g.timeoutMs}
                       on:change={(e) => updateGearChannel(g.id, x => ({ ...x, timeoutMs: Math.max(0, Math.round(numValue(e))) }))} />
                <span class="unit">ms</span>
            </div>

            <div class="doors-head">Doors ({g.doors.length}/2)
                {#if g.doors.length < 2}<button class="small add" on:click={() => addGearDoor(g.id)}>+ Add door</button>{/if}
            </div>
            {#each (g.doors) as d, di (di)}
                <div class="door">
                    <WizardPortPicker label={`Door ${di + 1}`} kind="servo" role={RoleKind.ServoActuator} value={d.port}
                        onChange={(ref) => updateGearChannel(g.id, x => ({ ...x, doors: x.doors.map((dd, i) => i === di ? { ...dd, port: ref } : dd) }))} />
                    <button class="small danger rm" on:click={() => removeGearDoor(g.id, di)}>×</button>
                </div>
            {/each}
            {#if g.doors.length >= 1}
                <div class="fx-row">
                    <span class="lbl">Opening</span>
                    <div class="seg">
                        {#each DOOR_MODES as m}<button class="seg-btn" class:on={g.doorMode === m.id} on:click={() => updateGearChannel(g.id, x => ({ ...x, doorMode: m.id }))}>{m.label}</button>{/each}
                    </div>
                </div>
                <div class="fx-row">
                    <span class="lbl">After deploy</span>
                    <div class="seg">
                        {#each CLOSE as c}<button class="seg-btn" class:on={g.closePolicy === c.id} on:click={() => updateGearChannel(g.id, x => ({ ...x, closePolicy: c.id }))}>{c.label}</button>{/each}
                    </div>
                </div>
            {/if}
        </div>
    {/each}
</div>

<style>
    .fx-sec { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 10px 12px; margin-bottom: 10px; }
    .fx-sec-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 8px; display: flex; align-items: center; gap: 10px; }
    .fx-row { display: flex; align-items: center; gap: 8px; margin: 5px 0; flex-wrap: wrap; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    .lbl2 { font-size: 11px; color: var(--text-dim); margin-left: 8px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .field-input.narrow { width: 84px; }
    .field-input.nm { flex: 1; min-width: 180px; }
    .seg { display: inline-flex; gap: 3px; flex-wrap: wrap; }
    .seg-btn { font-size: 11px; padding: 4px 10px; border: 1px solid var(--border); background: var(--bg-input); color: var(--text-dim); cursor: pointer; border-radius: 4px; }
    .seg-btn.on { border-color: var(--accent); color: var(--accent); background: color-mix(in srgb, var(--accent) 12%, var(--bg-input)); }
    .fx-check { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--text-dim); margin: 8px 0 0; cursor: pointer; }
    .strut { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-surface); padding: 8px 10px; margin: 8px 0; }
    .strut-head { display: flex; gap: 8px; align-items: center; margin-bottom: 6px; }
    .doors-head { font-size: 11px; color: var(--text-dim); margin: 8px 0 4px; display: flex; gap: 8px; align-items: center; }
    .door { display: flex; align-items: center; gap: 6px; }
    .add { font-size: 10px; }
    .rm { min-width: 26px; }
    .muted { color: var(--text-dim); font-size: 12px; }
</style>
