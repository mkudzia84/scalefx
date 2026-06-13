<!-- ScaleFX Studio — Input & Ports tab.
     Sub-tabs (Rule 60.3 segmented selector):
       • Port Configuration (default) — Inputs (left) + Output ports & roles
         (right), the two-column setup view.
       • Telemetry — the live telemetry collection, shown ONLY when telemetry
         emission is configured (an input carries the jeti-ex-input role, i.e.
         the IN_1 responder; future telemetry protocols add their role kind to
         `hasTelemetry`).
     Pure view component — Rule 46: hubConfigSource is pre-registered by
     App.svelte; the global ConfigToolbar owns Apply / Diagram / Refresh. -->
<script lang="ts">
    import InputPanel from './InputPanel.svelte'
    import PortRoleTab from './PortRoleTab.svelte'
    import TelemetryPanel from '../components/TelemetryPanel.svelte'
    import { deviceModel, RoleKind } from '../devicemodel'

    // Telemetry emission is configured when an input port carries the
    // jeti-ex-input role (IN_1 = the EX responder that emits telemetry to the
    // radio).  Add future telemetry-emitting protocols' role kinds here.
    $: hasTelemetry = $deviceModel.ports.some(
        p => p.kindName === 'input' && p.roleKind === RoleKind.JetiExInput)

    type SubTab = 'ports' | 'telemetry'
    let sub: SubTab = 'ports'
    // If telemetry stops being configured while it's the active sub-tab (role
    // detached), fall back to Port Configuration.
    $: if (!hasTelemetry && sub === 'telemetry') sub = 'ports'
</script>

<div class="io">
    {#if hasTelemetry}
        <div class="io-subtabs">
            <div class="seg-select">
                <button class="seg" class:on={sub === 'ports'}
                        on:click={() => (sub = 'ports')}>Port Configuration</button>
                <button class="seg" class:on={sub === 'telemetry'}
                        on:click={() => (sub = 'telemetry')}>Telemetry</button>
            </div>
        </div>
    {/if}

    <div class="io-scroll">
        {#if sub === 'ports'}
            <div class="io-cols">
                <div class="col left"><InputPanel /></div>
                <div class="col right"><PortRoleTab /></div>
            </div>
        {:else}
            <div class="io-telemetry"><TelemetryPanel /></div>
        {/if}
    </div>
</div>

<style>
    .io { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .io-subtabs { padding: 10px 12px 0; flex: 0 0 auto; }
    .io-scroll { flex: 1; min-height: 0; overflow: auto; }
    .io-cols   { display: flex; min-height: 100%; }
    .col       { min-width: 0; }
    .left  { flex: 0 0 46%; }
    .right { flex: 1; }
    /* Telemetry sub-tab: a single standard-width group, padded like the other
       single-column panels (Engine/Gun) rather than full-bleed. */
    .io-telemetry { padding: 10px 12px; max-width: 1100px; }
</style>
