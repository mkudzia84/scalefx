<!-- ScaleFX Studio — Input & Ports tab.
     Sub-tab bar (Rule 61 — the shared .sub-bar / .sub-item underline pattern,
     same as LightingTab):
       • Port Configuration (default) — Inputs (left) + Output ports & roles
         (right), the two-column setup view.
       • Telemetry — the live telemetry collection, present ONLY when telemetry
         emission is configured (an input carries the jeti-ex-input role, i.e.
         the IN_1 responder; future telemetry protocols add their role kind to
         `hasTelemetry`).
     Only the active surface is mounted (one live-view at a time).  Pure view —
     Rule 46: hubConfigSource is pre-registered by App.svelte; the global
     ConfigToolbar owns Apply / Diagram / Refresh. -->
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

    const ICON_PORTS = '<rect x="3" y="3" width="18" height="18" rx="2"/><line x1="12" y1="3" x2="12" y2="21"/>'
    const ICON_TELEM = '<path d="M22 12h-4l-3 9L9 3l-3 9H2"/>'
    const svg = (b: string) => `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${b}</svg>`
</script>

<div class="io">
    <div class="sub-bar">
        <button class="sub-item" class:active={sub === 'ports'} on:click={() => (sub = 'ports')}>
            <span class="sub-icon">{@html svg(ICON_PORTS)}</span>
            <span>Port Configuration</span>
        </button>
        {#if hasTelemetry}
            <button class="sub-item" class:active={sub === 'telemetry'} on:click={() => (sub = 'telemetry')}>
                <span class="sub-icon">{@html svg(ICON_TELEM)}</span>
                <span>Telemetry</span>
            </button>
        {/if}
    </div>

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
    .io-scroll { flex: 1; min-height: 0; overflow: auto; }
    .io-cols   { display: flex; min-height: 100%; }
    .col       { min-width: 0; }
    .left  { flex: 0 0 46%; }
    .right { flex: 1; }
    /* Telemetry sub-tab: full-width group with standard tab margins (matches
       the Lighting scroll / other tab panels). */
    .io-telemetry { padding: 12px; }
</style>
