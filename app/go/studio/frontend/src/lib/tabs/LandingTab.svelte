<!-- ScaleFX Studio — Landing tab.
     Hub-hosted Landing-light groups (servo-deployed LED searchlights): each
     group pairs a deploy servo with one or more LED bulbs, deploys servo-first
     then fades the bulb on, and is activated manually / by an RC channel / by a
     LightFx program.  Its own tab (2026-06-05).  Registers its DirtySource
     (Rule 46) at App.svelte startup so the global ConfigToolbar gates Apply. -->
<script lang="ts">
    import LandingPanel from './LandingPanel.svelte'
</script>

<div class="landing">
    <div class="landing-scroll">
        <LandingPanel />
    </div>
</div>

<style>
    .landing { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .landing-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; }
</style>
