<!-- ScaleFX Studio — Lighting tab.
     Two-column layout matching EffectsTab: LightFx programs on the
     left, landing-light groups on the right.  Both panels register
     their own DirtySource (Rule 46) at App.svelte startup so the
     global ConfigToolbar gates Apply across the pair. -->
<script lang="ts">
    import LightFxPanel  from './LightFxPanel.svelte'
    import LandingPanel  from './LandingPanel.svelte'
</script>

<div class="lighting">
    <div class="lighting-header">
        <h3>Lighting</h3>
    </div>
    <div class="lighting-scroll">
        <div class="lighting-cols">
            <div class="col left"><LightFxPanel /></div>
            <div class="col right"><LandingPanel /></div>
        </div>
    </div>
</div>

<style>
    .lighting { display: flex; flex-direction: column; height: 100%; min-height: 0; }

    .lighting-header {
        display: flex; align-items: center; gap: 12px;
        padding: 6px 12px 8px;
        border-bottom: 1px solid var(--border);
        background: var(--bg);
        flex-shrink: 0;
    }
    .lighting-header h3 { margin: 0; font-size: 14px; font-weight: 600; color: var(--text-bright); }

    .lighting-scroll { flex: 1; min-height: 0; overflow: auto; }
    .lighting-cols   { display: flex; min-height: 100%; padding: 12px; gap: 14px; }
    .col             { flex: 1; min-width: 0; }
</style>
