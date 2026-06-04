<!-- ScaleFX Studio — Gear tab.
     GearControl config for a connected gearcontrol expander.  (Landing-light
     groups moved to the Lighting tab 2026-06-03 — a landing group is an LED
     searchlight on a deploy servo, so it lives with the LED effects.)  The gear
     domain registers its own DirtySource (Rule 46) so the global ConfigToolbar
     gates Apply. -->
<script lang="ts">
    import DomainTab from './DomainTab.svelte'
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
    </div>
</div>

<style>
    .gl { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .gl-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; display: flex; flex-direction: column; gap: 14px; }
    .gl-section { min-width: 0; }
</style>
