<!-- ScaleFX Studio — Lighting tab.
     LightFX program editor only (full width — internal left/right split).
     Landing-light groups have their own "Landing" tab (2026-06-05).  Registers
     its DirtySource (Rule 46) at App.svelte startup so the global ConfigToolbar
     gates Apply. -->
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
