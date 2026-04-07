<!-- ScaleFX Studio — Tab Bar -->
<!-- Horizontal tab strip. For HubFX: shows hub tab + slave tabs with colored indicators. -->
<script lang="ts">
    import { connectionInfo, activeTab, slaveInfo } from '../stores'

    interface Tab {
        label: string
        type: string // controller type or 'firmware'
        connected: boolean
        ready: boolean
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
            })
            for (const s of slaves) {
                result.push({
                    label: s.name,
                    type: s.type,
                    connected: s.connected,
                    ready: s.ready,
                })
            }
        } else {
            // Direct board: single tab
            result.push({
                label: ci.controllerName || ci.controllerType || 'Board',
                type: ci.controllerType,
                connected: true,
                ready: ci.initialized,
            })
        }

        // Firmware tab always last
        result.push({
            label: 'Firmware',
            type: 'firmware',
            connected: true,
            ready: true,
        })

        return result
    }

    function selectTab(i: number) {
        $activeTab = i
    }

    function tabColor(tab: Tab): string {
        if (tab.type === 'firmware') return ''
        if (!tab.connected) return 'tab-disconnected'
        if (tab.ready) return 'tab-ready'
        return 'tab-connected'
    }

    function dotColor(tab: Tab): string {
        if (tab.type === 'firmware') return 'var(--text-dim)'
        if (!tab.connected) return 'var(--text-dim)'
        if (tab.ready) return 'var(--success)'
        return 'var(--warning)'
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
            <span class="tab-label">{tab.label}</span>
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

    .tab-dot {
        width: 7px;
        height: 7px;
        border-radius: 50%;
        flex-shrink: 0;
    }
</style>
