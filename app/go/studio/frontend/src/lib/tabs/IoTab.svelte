<!-- ScaleFX Studio — Input & Ports tab.
     Two columns under a unified scroll: Inputs (left) + Output ports
     & roles (right).  Pure view component — Rule 46: hubConfigSource
     is pre-registered by App.svelte; the global ConfigToolbar (above
     the tab strip) owns Apply / Diagram / Refresh / dirty-flag. -->
<script lang="ts">
    import InputPanel from './InputPanel.svelte'
    import PortRoleTab from './PortRoleTab.svelte'
</script>

<div class="io">
    <div class="io-scroll">
        <div class="io-cols">
            <div class="col left"><InputPanel /></div>
            <div class="col right"><PortRoleTab /></div>
        </div>
    </div>
</div>

<style>
    .io { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .io-scroll { flex: 1; min-height: 0; overflow: auto; }
    .io-cols   { display: flex; min-height: 100%; }
    .col       { min-width: 0; }
    .left  { flex: 0 0 46%; }
    .right { flex: 1; }
</style>
