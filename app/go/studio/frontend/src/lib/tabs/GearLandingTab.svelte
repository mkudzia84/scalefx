<!-- ScaleFX Studio — Gear & Landing tab.
     Collocates GearControl (when a gearcontrol expander is present) with the
     hub-hosted Landing-light groups (2026-06-02) — both are servo-deploy +
     lights mechanisms.  Landing always shows (hub ports); the gear domain panel
     only when a gearcontrol board is connected.  Both register their own
     DirtySource (Rule 46) so the global ConfigToolbar gates Apply. -->
<script lang="ts">
    import DomainTab from './DomainTab.svelte'
    import LandingPanel from './LandingPanel.svelte'
    import type { Domain } from '../devicemodel'

    export let domain: Domain | undefined = undefined
</script>

<div class="gl">
    <div class="gl-scroll">
        {#if domain}
            <div class="gl-section">
                <DomainTab {domain} />
            </div>
        {/if}
        <div class="gl-section">
            <LandingPanel />
        </div>
    </div>
</div>

<style>
    .gl { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .gl-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; display: flex; flex-direction: column; gap: 14px; }
    .gl-section { min-width: 0; }
</style>
