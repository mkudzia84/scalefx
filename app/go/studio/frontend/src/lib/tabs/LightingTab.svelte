<!-- ScaleFX Studio — Lighting tab.
     LightFX only (full width — the program editor uses an internal
     left/right split).  Landing-light groups moved to the Gear & Landing
     tab (2026-06-02).  Registers its DirtySource (Rule 46) at App.svelte
     startup so the global ConfigToolbar gates Apply. -->
<script lang="ts">
    import LightFxPanel from './LightFxPanel.svelte'
</script>

<div class="lighting">
    <div class="lighting-scroll">
        <LightFxPanel />
    </div>
</div>

<style>
    .lighting { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .lighting-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; }
</style>
