<!-- ScaleFX Studio — port-role inline config editor.

     Rule 44 (supersedes Rule 42 for servos): servo motion profile is
     now configured INLINE with the feature (GunFx panel embeds
     ServoProfileEditor next to each axis binding).  This component is
     therefore HEATER + DC-MOTOR ONLY — element voltage scaling stays
     on the role layer because it's a hardware fact (the element's
     rated mV), not a per-effect preference.

     Servo ports get an empty body (the IO tab still shows port + role
     + name in PortRoleTab.svelte; the motion-profile editor moved).

     Live tuning: every input is debounced ~350 ms and pushed via the
     role-layer live-tune commands (`MotorSetElement` / `HeaterSetElement`).
     In-flight target / position state is preserved (the role re-applies
     the new shape without re-attaching).

     Limit to hub-local ports for now — cross-board live-tune goes
     through Topology in a future pass.

     Props:
       portKind  — 'servo' | 'pwm' (servo just renders the empty hint)
       portIdx   — 0-based port index
       roleKind  — current attached role (drives which editor renders)
       portRailMv — Studio-side rail voltage (display only)
-->
<script lang="ts">
    import { onMount } from 'svelte'
    import {
        MotorGetElement, MotorSetElement,
        HeaterGetElement, HeaterSetElement,
    } from '../../../wailsjs/go/main/App'
    import { RoleKind, formatPortRail } from '../devicemodel'

    export let portKind: 'servo' | 'pwm'
    export let portIdx: number
    export let roleKind: number
    export let portRailMv: number = 0

    type MotorEl = { elementMv: number; scaling: number; portRailMv: number }
    type HeaterEl = { elementMv: number; scaling: number; drivePct: number; hystCx10: number; portRailMv: number }

    let motor: MotorEl = { elementMv:0, scaling:1, portRailMv:0 }
    let heater: HeaterEl = { elementMv:0, scaling:1, drivePct:100, hystCx10:50, portRailMv:0 }

    let loaded = false
    let busy = false
    let error = ''
    let pendingTimer: ReturnType<typeof setTimeout> | null = null

    $: kind = roleKind   // alias for the template

    onMount(async () => {
        try {
            if (portKind === 'pwm' && kind === RoleKind.DcMotor) {
                motor = await MotorGetElement(portIdx) as MotorEl
            } else if (portKind === 'pwm' && kind === RoleKind.Heater) {
                heater = await HeaterGetElement(portIdx) as HeaterEl
            }
            loaded = true
        } catch (e) {
            error = String(e)
        }
    })

    function scheduleSetMotor() {
        if (pendingTimer) clearTimeout(pendingTimer)
        pendingTimer = setTimeout(async () => {
            pendingTimer = null
            busy = true; error = ''
            try { await MotorSetElement(portIdx, motor) } catch (e) { error = String(e) } finally { busy = false }
        }, 350)
    }
    function scheduleSetHeater() {
        if (pendingTimer) clearTimeout(pendingTimer)
        pendingTimer = setTimeout(async () => {
            pendingTimer = null
            busy = true; error = ''
            try { await HeaterSetElement(portIdx, heater) } catch (e) { error = String(e) } finally { busy = false }
        }, 350)
    }

</script>

<div class="role-config">
    {#if !loaded}
        <div class="loading">loading config from firmware…</div>
    {:else if error}
        <div class="err">⚠ {error}</div>
    {:else if portKind === 'servo' && kind === RoleKind.ServoActuator}
        <!-- Rule 44 — servo motion profile moved to the feature panel
             (GunFx Turret section, EngineFx servo binding, …).  Nothing
             to configure here. -->
        <div class="empty">
            Servo motion profile (min / max / center / speed / accel / jerk)
            is set on the <b>feature panel</b> that uses this servo — Rule 44.
        </div>

    {:else if portKind === 'pwm' && kind === RoleKind.DcMotor}
        <div class="cfg-grid element-grid">
            <div class="field"><label>element rated mV</label>
                <input type="number" min="0" max="48000" step="100" bind:value={motor.elementMv}
                       on:change={scheduleSetMotor} /></div>
            <div class="field"><label>scaling</label>
                <select bind:value={motor.scaling} on:change={scheduleSetMotor}>
                    <option value={0}>passthrough</option>
                    <option value={1}>linear (Vₑ/Vₚ)</option>
                    <option value={2}>quadratic (Vₑ²/Vₚ²)</option>
                </select></div>
            <div class="field readonly"><label>port rail</label>
                <span class="readonly-val">{formatPortRail(motor.portRailMv)}</span></div>
            {#if motor.elementMv > 0 && motor.portRailMv > 0 && motor.elementMv < motor.portRailMv}
                <div class="field readonly">
                    <label>duty cap at 100 %</label>
                    <span class="readonly-val">{((motor.elementMv / motor.portRailMv) * 100).toFixed(0)} %</span>
                </div>
            {/if}
        </div>
        <div class="rule-pointer">Rule 42 — element scaling is applied by the role automatically when you call `setPct(N)` from any effect.</div>

    {:else if portKind === 'pwm' && kind === RoleKind.Heater}
        <div class="cfg-grid element-grid">
            <div class="field"><label>element rated mV</label>
                <input type="number" min="0" max="48000" step="100" bind:value={heater.elementMv}
                       on:change={scheduleSetHeater} /></div>
            <div class="field"><label>scaling</label>
                <select bind:value={heater.scaling} on:change={scheduleSetHeater}>
                    <option value={0}>passthrough</option>
                    <option value={1}>linear</option>
                    <option value={2}>quadratic</option>
                </select></div>
            <div class="field"><label>drive %</label>
                <input type="number" min="0" max="100" step="1" bind:value={heater.drivePct}
                       on:change={scheduleSetHeater} /></div>
            <div class="field"><label>hyst (¹⁄₁₀ °C)</label>
                <input type="number" min="0" max="500" step="1" bind:value={heater.hystCx10}
                       on:change={scheduleSetHeater} />
                <span class="hint">{(heater.hystCx10/10).toFixed(1)} °C</span></div>
            <div class="field readonly"><label>port rail</label>
                <span class="readonly-val">{formatPortRail(heater.portRailMv)}</span></div>
        </div>
        <div class="rule-pointer">Rule 42 — heater bang-bang uses these values. Changes push live ({busy ? 'sending…' : 'idle'}).</div>

    {:else}
        <div class="empty">No role-side config for this role kind.</div>
    {/if}
</div>

<style>
    .role-config { background: var(--bg-raised); border: 1px solid var(--border); border-radius: 4px; padding: 8px 10px; margin: 4px 0 8px 32px; }
    .loading { font-style: italic; color: var(--text-dim); font-size: 11px; }
    .err { color: var(--error); font-size: 11px; padding: 4px 0; }
    .empty { font-style: italic; color: var(--text-dim); font-size: 11px; line-height: 1.5; }

    .cfg-grid { display: grid; gap: 6px 12px; }
    .element-grid { grid-template-columns: repeat(3, 1fr); }

    .field { display: flex; flex-direction: column; gap: 2px; }
    .field label { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .field input[type="number"], .field select { padding: 3px 6px; font-size: 11px; background: var(--bg-input); color: var(--text); border: 1px solid var(--border); border-radius: 3px; height: 24px; box-sizing: border-box; font-family: var(--font-mono); }
    .field input[type="checkbox"] { width: 16px; height: 16px; accent-color: var(--accent); }
    .field .hint { font-size: 9px; color: var(--text-dim); font-style: italic; }
    .field.readonly .readonly-val { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); padding: 3px 0; }

    .rule-pointer { margin-top: 6px; font-size: 9px; font-style: italic; color: var(--text-dim); }
</style>
