<!-- ScaleFX Studio — Tab Bar.
     Renders the tab list derived from the device model: Ports & Roles,
     Inputs, one tab per capability-available functional domain, then
     Firmware.  See devicemodel.ts::studioTabs. -->
<script lang="ts">
    import { activeTab } from '../stores'
    import { studioTabs, validationCounts, type StudioTab } from '../devicemodel'

    function selectTab(i: number) { $activeTab = i }

    // Clamp the active index if the tab set shrinks (e.g. on disconnect).
    $: if ($activeTab >= $studioTabs.length) $activeTab = Math.max(0, $studioTabs.length - 1)

    // SVG path bodies, drawn inside a common 24×24 stroked frame.
    const ICONS: Record<string, string> = {
        // Input & Ports — sliders / mixer
        io: '<line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/>',
        // Firmware — chip
        firmware: '<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="2" x2="9" y2="4"/><line x1="15" y1="2" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="22"/><line x1="15" y1="20" x2="15" y2="22"/><line x1="2" y1="9" x2="4" y2="9"/><line x1="2" y1="15" x2="4" y2="15"/><line x1="20" y1="9" x2="22" y2="9"/><line x1="20" y1="15" x2="22" y2="15"/>',
        // Landing lights — lightbulb beam
        'landing-lights': '<path d="M9 18h6"/><path d="M10 22h4"/><path d="M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z"/>',
        // Lighting — sun
        lighting: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
        // Landing gear — wheel
        'landing-gear': '<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/><line x1="12" y1="3" x2="12" y2="9"/><line x1="12" y1="15" x2="12" y2="21"/><line x1="3" y1="12" x2="9" y2="12"/><line x1="15" y1="12" x2="21" y2="12"/>',
        // Engine — fan / turbine
        engine: '<circle cx="12" cy="12" r="2.2"/><path d="M12 10c0-4 1-7 2-7s1 4-1 6M14 12c4 0 7 1 7 2s-4 1-6-1M12 14c0 4-1 7-2 7s-1-4 1-6M10 12c-4 0-7-1-7-2s4-1 6 1"/>',
        // Gun — crosshair
        gun: '<circle cx="12" cy="12" r="9"/><line x1="22" y1="12" x2="18" y2="12"/><line x1="6" y1="12" x2="2" y2="12"/><line x1="12" y1="6" x2="12" y2="2"/><line x1="12" y1="22" x2="12" y2="18"/>',
    }
    const FALLBACK = '<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3"/>'

    function icon(tab: StudioTab): string {
        const body = tab.kind === 'domain'
            ? (ICONS[tab.domain?.id ?? ''] ?? FALLBACK)
            : (ICONS[tab.kind] ?? FALLBACK)
        return `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${body}</svg>`
    }
</script>

<div class="tab-bar">
    {#each $studioTabs as tab, i (tab.key)}
        <button class="tab-item" class:active={$activeTab === i} on:click={() => selectTab(i)}>
            <span class="tab-icon">{@html icon(tab)}</span>
            <span class="tab-label">{tab.label}</span>
        </button>
    {/each}
    <div class="spacer"></div>
    {#if $validationCounts.errors > 0}
        <span class="vbadge err" title="Validation errors">{$validationCounts.errors} ✕</span>
    {:else if $validationCounts.warnings > 0}
        <span class="vbadge warn" title="Validation warnings">{$validationCounts.warnings} !</span>
    {/if}
</div>

<style>
    .tab-bar { display: flex; align-items: stretch; background: var(--bg-surface); border-bottom: 1px solid var(--border); flex-shrink: 0; overflow-x: auto; padding: 0 4px; }
    .tab-item { display: flex; align-items: center; gap: 6px; padding: 8px 16px; border: none; border-bottom: 2px solid transparent; background: transparent; color: var(--text-dim); cursor: pointer; font-family: var(--font-ui); font-size: 13px; white-space: nowrap; transition: color 0.15s, border-color 0.15s; }
    .tab-item:hover { color: var(--text); }
    .tab-item.active { color: var(--text-bright); border-bottom-color: var(--accent); }
    .tab-icon { display: flex; align-items: center; flex-shrink: 0; opacity: 0.75; }
    .tab-item.active .tab-icon { opacity: 1; }
    .spacer { flex: 1; }
    .vbadge { align-self: center; font-size: 11px; font-family: var(--font-mono); padding: 2px 8px; border-radius: 10px; margin-right: 6px; }
    .vbadge.err { color: var(--error); background: rgba(255,80,80,0.12); }
    .vbadge.warn { color: var(--warning); background: rgba(255,200,0,0.1); }
</style>
