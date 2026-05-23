<!-- ScaleFX Studio — port-role inline config editor.

     Rule 42: motion profile (servo) + element scaling (heater/motor)
     are configured on the role-attach row in /hubfx.yaml's ports[]
     block, NOT inside the effects that use them.  This component is
     where the operator authors those values — one expanding panel per
     port row in the IO tab.

     Live tuning: every input is debounced ~350 ms and pushed via the
     role-layer live-tune commands (`ServoSetProfile` /
     `MotorSetElement` / `HeaterSetElement`).  In-flight target /
     position state is preserved (the role re-applies the new shape
     without re-attaching).

     Limit to hub-local ports for now — cross-board live-tune goes
     through Topology in a future pass.

     Props:
       portKind  — 'servo' | 'pwm'
       portIdx   — 0-based port index
       roleKind  — current attached role (drives which editor renders)
       portRailMv — Studio-side rail voltage (display only)
-->
<script lang="ts">
    import { onMount } from 'svelte'
    import {
        ServoGetProfile, ServoSetProfile,
        MotorGetElement, MotorSetElement,
        HeaterGetElement, HeaterSetElement,
    } from '../../../wailsjs/go/main/App'
    import { RoleKind, formatPortRail } from '../devicemodel'

    export let portKind: 'servo' | 'pwm'
    export let portIdx: number
    export let roleKind: number
    export let portRailMv: number = 0

    // ── Local draft state for the editor.  Loaded on mount via
    // GET; on every change the new value is debounced + pushed via SET.
    type Profile = {
        minUs: number; maxUs: number; centerUs: number; reversed: boolean
        maxSpeedUsPerSec: number; maxAccelUsPerSec2: number; maxJerkUsPerSec3: number
    }
    type MotorEl = { elementMv: number; scaling: number; portRailMv: number }
    type HeaterEl = { elementMv: number; scaling: number; drivePct: number; hystCx10: number; portRailMv: number }

    let profile: Profile = { minUs:1000, maxUs:2000, centerUs:1500, reversed:false, maxSpeedUsPerSec:800, maxAccelUsPerSec2:1600, maxJerkUsPerSec3:0 }
    let motor: MotorEl = { elementMv:0, scaling:1, portRailMv:0 }
    let heater: HeaterEl = { elementMv:0, scaling:1, drivePct:100, hystCx10:50, portRailMv:0 }

    let loaded = false
    let busy = false
    let error = ''
    let pendingTimer: ReturnType<typeof setTimeout> | null = null

    $: kind = roleKind   // alias for the template

    onMount(async () => {
        try {
            if (portKind === 'servo' && kind === RoleKind.ServoActuator) {
                profile = await ServoGetProfile(portIdx) as Profile
            } else if (portKind === 'pwm' && kind === RoleKind.DcMotor) {
                motor = await MotorGetElement(portIdx) as MotorEl
            } else if (portKind === 'pwm' && kind === RoleKind.Heater) {
                heater = await HeaterGetElement(portIdx) as HeaterEl
            }
            loaded = true
        } catch (e) {
            error = String(e)
        }
    })

    function scheduleSetProfile() {
        if (pendingTimer) clearTimeout(pendingTimer)
        pendingTimer = setTimeout(async () => {
            pendingTimer = null
            busy = true; error = ''
            try { await ServoSetProfile(portIdx, profile) } catch (e) { error = String(e) } finally { busy = false }
        }, 350)
    }
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

    function numInput(e: Event): number { return Number((e.target as HTMLInputElement).value) }
    function boolInput(e: Event): boolean { return (e.target as HTMLInputElement).checked }
    function selVal(e: Event): string { return (e.target as HTMLSelectElement).value }
</script>

<div class="role-config">
    {#if !loaded}
        <div class="loading">loading config from firmware…</div>
    {:else if error}
        <div class="err">⚠ {error}</div>
    {:else if portKind === 'servo' && kind === RoleKind.ServoActuator}
        <div class="cfg-grid servo-grid">
            <div class="field"><label>min µs</label>
                <input type="number" min="500" max="2500" step="10" bind:value={profile.minUs}
                       on:change={() => { profile = { ...profile, minUs: profile.minUs }; scheduleSetProfile() }} /></div>
            <div class="field"><label>max µs</label>
                <input type="number" min="500" max="2500" step="10" bind:value={profile.maxUs}
                       on:change={scheduleSetProfile} /></div>
            <div class="field"><label>center µs</label>
                <input type="number" min="500" max="2500" step="10" bind:value={profile.centerUs}
                       on:change={scheduleSetProfile} /></div>
            <div class="field"><label>reversed</label>
                <input type="checkbox" bind:checked={profile.reversed}
                       on:change={scheduleSetProfile} /></div>
            <div class="field"><label>max speed µs/s</label>
                <input type="number" min="0" max="10000" step="50" bind:value={profile.maxSpeedUsPerSec}
                       on:change={scheduleSetProfile} /></div>
            <div class="field"><label>max accel µs/s²</label>
                <input type="number" min="0" max="50000" step="100" bind:value={profile.maxAccelUsPerSec2}
                       on:change={scheduleSetProfile} /></div>
            <div class="field"><label>max jerk µs/s³</label>
                <input type="number" min="0" max="100000" step="100" bind:value={profile.maxJerkUsPerSec3}
                       on:change={scheduleSetProfile} />
                <span class="hint">{profile.maxJerkUsPerSec3 > 0 ? 'S-curve' : 'trapezoidal'}</span>
            </div>
        </div>
        <div class="rule-pointer">Rule 42 — motion shape lives on the role, not the effect. Changes push live ({busy ? 'sending…' : 'idle'}).</div>

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
    .empty { font-style: italic; color: var(--text-dim); font-size: 11px; }

    .cfg-grid { display: grid; gap: 6px 12px; }
    .servo-grid { grid-template-columns: repeat(4, 1fr); }
    .element-grid { grid-template-columns: repeat(3, 1fr); }

    .field { display: flex; flex-direction: column; gap: 2px; }
    .field label { font-size: 9px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--text-dim); }
    .field input[type="number"], .field select { padding: 3px 6px; font-size: 11px; background: var(--bg-input); color: var(--text); border: 1px solid var(--border); border-radius: 3px; height: 24px; box-sizing: border-box; font-family: var(--font-mono); }
    .field input[type="checkbox"] { width: 16px; height: 16px; accent-color: var(--accent); }
    .field .hint { font-size: 9px; color: var(--text-dim); font-style: italic; }
    .field.readonly .readonly-val { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); padding: 3px 0; }

    .rule-pointer { margin-top: 6px; font-size: 9px; font-style: italic; color: var(--text-dim); }
</style>
