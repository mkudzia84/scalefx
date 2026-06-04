<!-- ScaleFX Studio — Lighting tab.
     LightFX program editor (full width — internal left/right split) PLUS the
     hub-hosted Landing-light groups (2026-06-03 — a landing group is a servo-
     deployed searchlight whose bulb runs an LED fade-in/on, so it lives with the
     LED effects).  Both register their own DirtySource (Rule 46) at App.svelte
     startup so the global ConfigToolbar gates Apply. -->
<script lang="ts">
    import LightFxPanel from './LightFxPanel.svelte'
    import LandingPanel from './LandingPanel.svelte'
</script>

<div class="lighting">
    <div class="lighting-scroll">
        <div class="lighting-section">
            <LightFxPanel />
        </div>
        <div class="lighting-section">
            <LandingPanel />
        </div>
    </div>
</div>

<style>
    .lighting { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .lighting-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; display: flex; flex-direction: column; gap: 14px; }
    .lighting-section { min-width: 0; }
</style>
