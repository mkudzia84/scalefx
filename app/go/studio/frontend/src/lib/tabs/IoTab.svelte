<!-- ScaleFX Studio — Input & Ports tab.
     Two columns: input configuration (protocol, channels, functions, live
     value bars) on the left; the physical port → role wiring on the
     right. -->
<script lang="ts">
    import InputPanel from './InputPanel.svelte'
    import PortRoleTab from './PortRoleTab.svelte'
</script>

<div class="io">
    <div class="col left"><InputPanel /></div>
    <div class="divider"></div>
    <div class="col right"><PortRoleTab /></div>
</div>

<style>
    .io { display: flex; height: 100%; min-height: 0; }
    .col { min-height: 0; overflow: auto; }
    .left { flex: 0 0 46%; }
    .right { flex: 1; }
    .divider { width: 1px; background: var(--border); flex-shrink: 0; }
</style>
