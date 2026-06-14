<!-- WizardEffectGun — per-gun setup: trigger, rate-of-fire, muzzle flash,
     recoil, smoke (heater+fan), turret yaw/pitch — auto-attaching ports. -->
<script lang="ts">
    import { RoleKind } from '../devicemodel'
    import { gunfxDraft, addGun, removeGun, updateGun, type GunT } from '../gunfx'
    import WizardChannelSelect from './WizardChannelSelect.svelte'
    import WizardPortPicker from './WizardPortPicker.svelte'
    import type { PortRefT } from '../wizard-ports'

    const numValue = (e: Event) => Number((e.target as HTMLInputElement).value)
    const strVal   = (e: Event) => (e.target as HTMLInputElement).value
    const boolValue = (e: Event) => (e.target as HTMLInputElement).checked
    const up = (id: number, m: (g: GunT) => GunT) => updateGun(id, m)

    function setRofRpm(id: number, rpm: number) {
        up(id, x => ({ ...x, rof: { ...x.rof, items: x.rof.items.map((it, i) => i === 0 ? { ...it, rpm } : it) } }))
    }
    function setAxis(id: number, axis: 'yaw' | 'pitch', patch: Partial<GunT['yaw']>) {
        up(id, x => ({ ...x, [axis]: { ...x[axis], ...patch } }))
    }
</script>

<div class="fx-top"><button class="small add" on:click={() => addGun()}>+ Add gun</button></div>
{#if $gunfxDraft.guns.length === 0}<p class="muted">No guns — add one.</p>{/if}

{#each ($gunfxDraft.guns) as g (g.id)}
    <div class="gun">
        <div class="gun-head">
            <input class="field-input nm" type="text" value={g.name} placeholder="gun name"
                   on:change={(e) => up(g.id, x => ({ ...x, name: strVal(e) }))} />
            <button class="small danger rm" on:click={() => removeGun(g.id)}>× Remove</button>
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Trigger</div>
            <WizardChannelSelect label="Fire channel" value={g.trigger.input}
                onChange={(fn) => up(g.id, x => ({ ...x, trigger: { ...x.trigger, input: fn } }))} />
            <div class="fx-row"><span class="lbl">Threshold</span>
                <input class="field-input narrow" type="number" min="1000" max="2000" step="10" value={g.trigger.thresholdUs}
                       on:change={(e) => up(g.id, x => ({ ...x, trigger: { ...x.trigger, thresholdUs: Math.round(numValue(e)) } }))} /><span class="unit">µs</span>
                <span class="lbl2">Rate of fire</span>
                <input class="field-input narrow" type="number" min="30" max="3000" step="10" value={g.rof.items[0]?.rpm ?? 600}
                       on:change={(e) => setRofRpm(g.id, Math.round(numValue(e)))} /><span class="unit">rpm</span>
            </div>
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Muzzle flash</div>
            <WizardPortPicker label="LED port" kind="pwm" role={RoleKind.LedAnimator} value={g.muzzleFlash.port}
                onChange={(ref) => up(g.id, x => ({ ...x, muzzleFlash: { ...x.muzzleFlash, port: ref } }))} />
            <div class="fx-row"><span class="lbl">Duration</span>
                <input class="field-input narrow" type="number" min="10" max="500" step="5" value={g.muzzleFlash.durationMs}
                       on:change={(e) => up(g.id, x => ({ ...x, muzzleFlash: { ...x.muzzleFlash, durationMs: Math.round(numValue(e)) } }))} /><span class="unit">ms · brightness</span>
                <input class="field-input narrow" type="number" min="0" max="100" step="5" value={g.muzzleFlash.brightness}
                       on:change={(e) => up(g.id, x => ({ ...x, muzzleFlash: { ...x.muzzleFlash, brightness: Math.round(numValue(e)) } }))} /><span class="unit">%</span>
            </div>
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Recoil</div>
            <label class="fx-check"><input type="checkbox" checked={g.recoil.enabled}
                    on:change={(e) => up(g.id, x => ({ ...x, recoil: { ...x.recoil, enabled: boolValue(e) } }))} /> Recoil kick on each shot</label>
            {#if g.recoil.enabled}
                <div class="fx-row"><span class="lbl">Kick strength</span>
                    <input class="field-input narrow" type="number" min="0" max="500" step="5" value={g.recoil.jerkUs}
                           on:change={(e) => up(g.id, x => ({ ...x, recoil: { ...x.recoil, jerkUs: Math.round(numValue(e)) } }))} /><span class="unit">µs</span>
                </div>
            {/if}
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Smoke (optional)</div>
            <WizardPortPicker label="Heater" kind="pwm" role={RoleKind.Heater} value={g.smoke.heater.port}
                onChange={(ref) => up(g.id, x => ({ ...x, smoke: { ...x.smoke, heater: { ...x.smoke.heater, port: ref } } }))} />
            <WizardPortPicker label="Fan" kind="pwm" role={RoleKind.DcMotor} value={g.smoke.fan.port}
                onChange={(ref) => up(g.id, x => ({ ...x, smoke: { ...x.smoke, fan: { ...x.smoke.fan, port: ref } } }))} />
            <div class="fx-row"><span class="lbl">Element voltage</span>
                <input class="field-input narrow" type="number" min="3000" max="12000" step="500" value={g.smoke.heater.elementMv}
                       on:change={(e) => up(g.id, x => ({ ...x, smoke: { ...x.smoke, heater: { ...x.smoke.heater, elementMv: Math.round(numValue(e)) } } }))} /><span class="unit">mV</span>
            </div>
        </div>

        <div class="fx-sec">
            <div class="fx-sec-head">Turret (optional)</div>
            <label class="fx-check"><input type="checkbox" checked={g.yaw.enabled}
                    on:change={(e) => setAxis(g.id, 'yaw', { enabled: boolValue(e) })} /> Yaw servo</label>
            {#if g.yaw.enabled}
                <WizardPortPicker label="Yaw servo" kind="servo" role={RoleKind.ServoActuator} value={g.yaw.servoPort}
                    onChange={(ref) => setAxis(g.id, 'yaw', { servoPort: ref })} />
                <WizardChannelSelect label="Yaw channel" value={g.yaw.input}
                    onChange={(fn) => setAxis(g.id, 'yaw', { input: fn })} />
            {/if}
            <label class="fx-check"><input type="checkbox" checked={g.pitch.enabled}
                    on:change={(e) => setAxis(g.id, 'pitch', { enabled: boolValue(e) })} /> Pitch servo</label>
            {#if g.pitch.enabled}
                <WizardPortPicker label="Pitch servo" kind="servo" role={RoleKind.ServoActuator} value={g.pitch.servoPort}
                    onChange={(ref) => setAxis(g.id, 'pitch', { servoPort: ref })} />
                <WizardChannelSelect label="Pitch channel" value={g.pitch.input}
                    onChange={(fn) => setAxis(g.id, 'pitch', { input: fn })} />
            {/if}
        </div>
    </div>
{/each}

<style>
    .fx-top { margin-bottom: 6px; }
    .gun { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-surface); padding: 8px 10px; margin-bottom: 12px; }
    .gun-head { display: flex; gap: 8px; align-items: center; margin-bottom: 8px; }
    .fx-sec { border: 1px solid var(--border); border-radius: 6px; background: var(--bg-raised); padding: 8px 10px; margin-bottom: 8px; }
    .fx-sec-head { font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-dim); margin-bottom: 6px; }
    .fx-row { display: flex; align-items: center; gap: 8px; margin: 5px 0; flex-wrap: wrap; }
    .lbl { font-size: 11px; color: var(--text-dim); min-width: 110px; }
    .lbl2 { font-size: 11px; color: var(--text-dim); margin-left: 8px; }
    .unit { font-size: 11px; color: var(--text-dim); }
    .field-input.narrow { width: 84px; }
    .field-input.nm { flex: 1; min-width: 180px; }
    .fx-check { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--text-dim); margin: 4px 0; cursor: pointer; }
    .add { font-size: 10px; }
    .muted { color: var(--text-dim); font-size: 12px; }
</style>
