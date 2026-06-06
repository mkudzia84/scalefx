<!-- ScaleFX Studio — Lighting tab.
     A segmented sub-tab switcher over the two LED surfaces (2026-06-05):
       • Light Programs — the LightFX program editor (full-width left/right split)
       • Retractable Lights — servo-deployed LED searchlight groups
     Landing lights live under Lighting because they're an LED effect (the servo
     is just the deploy mechanism) and they attach to a Light program via
     landing_bindings.  Only the active sub-surface is mounted (matches the
     top-level tab behaviour — one live-view at a time).  Both register their own
     DirtySource (Rule 46) at App.svelte startup so the global ConfigToolbar gates
     Apply regardless of which sub-tab is showing. -->
<script lang="ts">
    import LightFxPanel from './LightFxPanel.svelte'
    import LandingPanel from './LandingPanel.svelte'

    let sub: 'programs' | 'landing' = 'programs'

    const ICON_SUN = '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>'
    const ICON_BULB = '<path d="M9 18h6"/><path d="M10 22h4"/><path d="M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z"/>'
    const svg = (b: string) => `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${b}</svg>`
</script>

<div class="lighting">
    <div class="sub-bar">
        <button class="sub-item" class:active={sub === 'programs'} on:click={() => sub = 'programs'}>
            <span class="sub-icon">{@html svg(ICON_SUN)}</span>
            <span>Light Programs</span>
        </button>
        <button class="sub-item" class:active={sub === 'landing'} on:click={() => sub = 'landing'}>
            <span class="sub-icon">{@html svg(ICON_BULB)}</span>
            <span>Retractable Lights</span>
        </button>
    </div>
    <div class="lighting-scroll">
        {#if sub === 'programs'}
            <LightFxPanel />
        {:else}
            <LandingPanel />
        {/if}
    </div>
</div>

<style>
    .lighting { display: flex; flex-direction: column; height: 100%; min-height: 0; }
    .sub-bar { display: flex; align-items: stretch; gap: 2px; background: var(--bg-surface); border-bottom: 1px solid var(--border); flex-shrink: 0; padding: 0 8px; }
    .sub-item { display: flex; align-items: center; gap: 6px; padding: 6px 14px; border: none; border-bottom: 2px solid transparent; background: transparent; color: var(--text-dim); cursor: pointer; font-family: var(--font-ui); font-size: 12px; white-space: nowrap; transition: color 0.15s, border-color 0.15s; }
    .sub-item:hover { color: var(--text); }
    .sub-item.active { color: var(--text-bright); border-bottom-color: var(--accent); }
    .sub-icon { display: flex; align-items: center; opacity: 0.7; }
    .sub-item.active .sub-icon { opacity: 1; }
    .lighting-scroll { flex: 1; min-height: 0; overflow: auto; padding: 12px; }
</style>
