<!-- ScaleFX Studio — Tab Bar -->
<!-- Horizontal tab strip. For HubFX: shows hub tab + slave tabs with colored indicators. -->
<script lang="ts">
    import { connectionInfo, activeTab, slaveInfo } from '../stores'

    interface Tab {
        label: string
        type: string // controller type or 'firmware'
        connected: boolean
        ready: boolean
        enabled: boolean
        isSlave: boolean
    }

    $: tabs = buildTabs($connectionInfo, $slaveInfo)

    function buildTabs(ci: typeof $connectionInfo, slaves: typeof $slaveInfo): Tab[] {
        const result: Tab[] = []

        if (!ci.connected) return result

        if (ci.controllerType === 'hubfx') {
            // HubFX: hub tab + slave tabs
            result.push({
                label: ci.controllerName || 'HubFX',
                type: 'hubfx',
                connected: true,
                ready: ci.initialized,
                enabled: true,
                isSlave: false,
            })
            for (const s of slaves) {
                const en = s.enabled ?? true
                result.push({
                    label: s.name,
                    type: s.type,
                    connected: s.connected,
                    ready: s.ready,
                    enabled: en,
                    isSlave: true,
                })
            }
        } else {
            // Direct board: single tab
            result.push({
                label: ci.controllerName || ci.controllerType || 'Board',
                type: ci.controllerType,
                connected: true,
                ready: ci.initialized,
                enabled: true,
                isSlave: false,
            })
        }

        // Firmware tab always last
        result.push({
            label: 'Firmware',
            type: 'firmware',
            connected: true,
            ready: true,
            enabled: true,
            isSlave: false,
        })

        return result
    }

    function selectTab(i: number) {
        $activeTab = i
    }

    function tabIcon(type: string): string {
        switch (type) {
            // LightFX — lightbulb
            case 'lightfx': return '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18h6"/><path d="M10 22h4"/><path d="M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z"/></svg>'
            // GunFX — crosshair
            case 'gunfx': return '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="22" y1="12" x2="18" y2="12"/><line x1="6" y1="12" x2="2" y2="12"/><line x1="12" y1="6" x2="12" y2="2"/><line x1="12" y1="22" x2="12" y2="18"/></svg>'
            // GearControl — gear cog
            case 'gearcontrol': return '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>'
            // HubFX — hub/network
            case 'hubfx': return '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><line x1="12" y1="3" x2="12" y2="9"/><line x1="12" y1="15" x2="12" y2="21"/><line x1="3" y1="12" x2="9" y2="12"/><line x1="15" y1="12" x2="21" y2="12"/><circle cx="12" cy="3" r="1.5"/><circle cx="12" cy="21" r="1.5"/><circle cx="3" cy="12" r="1.5"/><circle cx="21" cy="12" r="1.5"/></svg>'
            // Firmware — chip/download
            case 'firmware': return '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="2" x2="9" y2="4"/><line x1="15" y1="2" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="22"/><line x1="15" y1="20" x2="15" y2="22"/><line x1="2" y1="9" x2="4" y2="9"/><line x1="2" y1="15" x2="4" y2="15"/><line x1="20" y1="9" x2="22" y2="9"/><line x1="20" y1="15" x2="22" y2="15"/></svg>'
            default: return ''
        }
    }

    function tabColor(tab: Tab): string {
        if (tab.type === 'firmware') return ''
        if (tab.isSlave && !tab.enabled) return 'tab-disabled'
        if (!tab.connected) return 'tab-disconnected'
        if (tab.ready) return 'tab-ready'
        return 'tab-connected'
    }

    function dotColor(tab: Tab): string {
        if (tab.type === 'firmware') return 'var(--text-dim)'
        if (tab.isSlave && !tab.enabled) return 'var(--text-dim)'
        if (!tab.connected) return 'var(--text-dim)'
        if (tab.ready) return 'var(--success)'
        return 'var(--warning)'
    }

    function statusText(tab: Tab): string {
        if (tab.type === 'firmware') return ''
        if (tab.isSlave && !tab.enabled) return 'disabled'
        if (!tab.connected) return 'offline'
        if (tab.ready) return 'ready'
        return 'connected'
    }
</script>

<div class="tab-bar">
    {#each tabs as tab, i}
        <button
            class="tab-item {tabColor(tab)}"
            class:active={$activeTab === i}
            on:click={() => selectTab(i)}
        >
            {#if tab.type !== 'firmware'}
                <span class="tab-dot" style="background: {dotColor(tab)}"></span>
            {/if}
            <span class="tab-icon">{@html tabIcon(tab.type)}</span>
            <span class="tab-label">{tab.label}</span>
            {#if tab.isSlave}
                <span class="tab-status">{statusText(tab)}</span>
            {/if}
        </button>
    {/each}
</div>

<style>
    .tab-bar {
        display: flex;
        align-items: stretch;
        background: var(--bg-surface);
        border-bottom: 1px solid var(--border);
        flex-shrink: 0;
        overflow-x: auto;
        padding: 0 4px;
    }

    .tab-item {
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 8px 16px;
        border: none;
        border-bottom: 2px solid transparent;
        background: transparent;
        color: var(--text-dim);
        cursor: pointer;
        font-family: var(--font-ui);
        font-size: 13px;
        white-space: nowrap;
        transition: color 0.15s, border-color 0.15s;
    }

    .tab-item:hover {
        color: var(--text);
    }

    .tab-item.active {
        color: var(--text-bright);
        border-bottom-color: var(--accent);
    }

    .tab-disconnected {
        opacity: 0.5;
    }

    .tab-disabled {
        opacity: 0.4;
        font-style: italic;
    }

    .tab-dot {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        flex-shrink: 0;
    }

    .tab-icon {
        display: flex;
        align-items: center;
        flex-shrink: 0;
        opacity: 0.75;
    }

    .tab-item.active .tab-icon {
        opacity: 1;
    }

    .tab-status {
        font-size: 10px;
        font-family: var(--font-mono);
        text-transform: uppercase;
        letter-spacing: 0.3px;
        color: var(--text-dim);
        opacity: 0.7;
    }
</style>
