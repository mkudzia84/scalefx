<!-- ScaleFX Studio — Effects tab.
     Two columns: EngineFX (left) and GunFX (right).  Each section has its
     own enable toggle in the card header — content expands when enabled. -->
<script lang="ts">
    import EnginePanel from './EnginePanel.svelte'
    import GunFxPanel from './GunFxPanel.svelte'
</script>

<div class="effects">
    <div class="col left"><EnginePanel /></div>
    <div class="col right"><GunFxPanel /></div>
</div>

<style>
    .effects { display: flex; height: 100%; min-height: 0; padding: 14px; gap: 14px; }
    .col { flex: 1; min-width: 0; min-height: 0; overflow: auto; }
</style>
