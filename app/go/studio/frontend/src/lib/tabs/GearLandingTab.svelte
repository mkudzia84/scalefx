<!-- ScaleFX Studio — Gear tab.
     The Gear / Undercarriage panel (GearPanel) edits /gearcontrol.yaml for
     the hub's GearControl effect: per-channel gear motor + door servos +
     coordination.  (Landing-light groups moved to the Lighting tab
     2026-06-03.)  The gear domain registers its own DirtySource (Rule 46)
     so the global ConfigToolbar gates Apply; this tab is a pure view. -->
<script lang="ts">
    import GearPanel from './GearPanel.svelte'
    import type { Domain } from '../devicemodel'

    export let domain: Domain | undefined = undefined
</script>

<div class="gl">
    <div class="gl-scroll">
        <div class="gl-section">
            <GearPanel />
        </div>
    </div>
</div>

<style>
    .gl { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .gl-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; display: flex; flex-direction: column; gap: 14px; }
    .gl-section { min-width: 0; }
</style>
