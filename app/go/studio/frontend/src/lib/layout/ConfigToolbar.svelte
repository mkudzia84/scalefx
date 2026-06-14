<!-- ScaleFX Studio — Global Config Toolbar
     Lives between TabBar and the tab content, always visible.  Reads
     from `dirty-registry`: every tab/panel registers its config source
     on mount; this bar shows aggregate dirty + error state across all
     of them and runs `applyAll()` when the operator presses Apply.

     Rule 35 + 45: Apply is disabled when ANY source has validation
     errors (validation runs on every data change inside each source).
     The dirty pill lists which sources need attention.

     The Diagram + Refresh buttons live here too so they're reachable
     from every tab, not just the IO tab. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { anyDirty, anyErrors, dirtyLabels, errorLabels, applyAll, refreshAll } from '../dirty-registry'
    import { showPcbOverlay } from '../stores'
    import { openWizard } from '../wizard'
    import { diag } from '../diag'
    import { SetInputRouting, GetInputRouting } from '../../../wailsjs/go/main/App'
    import { EventsOn } from '../../../wailsjs/runtime/runtime'

    let busy = false

    // Global RC→effect routing gate (mirrors the hub's InputDispatcher
    // flag).  ON = RC sticks drive effects; OFF = effects ignore RC and
    // hold last state, so the operator drives them from Studio.  Live
    // channel monitors keep working either way.
    let rcRouting = true
    let routingBusy = false

    async function syncRouting() {
        try { rcRouting = await GetInputRouting() } catch { /* disconnected — keep optimistic */ }
    }
    async function toggleRouting() {
        routingBusy = true
        const next = !rcRouting
        try {
            await SetInputRouting(next)
            rcRouting = next
            diag.info('FE.CFG', `RC routing → ${next ? 'on' : 'off (manual)'}`)
        } catch (e) {
            diag.error('FE.CFG', 'RC routing toggle failed', { err: String(e) })
        } finally { routingBusy = false }
    }

    onMount(() => {
        syncRouting()
        // Re-sync whenever a (re)connect republishes the device model.
        const off = EventsOn('devicemodel:changed', () => syncRouting())
        return off
    })

    // Success / note messages route to the diag system (console + status
    // bar already mirror those).  Only Apply ERRORS render inline below
    // the toolbar — those need immediate operator attention before the
    // next edit.
    let lastError = ''

    async function onApply() {
        busy = true; lastError = ''
        try {
            await applyAll()
            diag.info('FE.CFG', 'global apply complete — all dirty configs saved + reloaded')
        } catch (e) {
            lastError = String(e)
            diag.error('FE.CFG', 'global apply failed', { err: String(e) })
        } finally { busy = false }
    }

    async function onRefresh() {
        busy = true; lastError = ''
        try {
            await refreshAll()
            diag.info('FE.CFG', 'refresh complete — every source re-fetched')
        } catch (e) {
            lastError = String(e)
            diag.error('FE.CFG', 'refresh failed', { err: String(e) })
        } finally { busy = false }
    }
</script>

<div class="config-toolbar">
    <div class="left">
        <button class="small primary" on:click={openWizard}
                title="Guided setup — pick features, set up the radio, and configure each effect step by step">🪄 Setup Wizard</button>
        <button class="small" on:click={() => ($showPcbOverlay = true)}
                title="Open the board diagram — assign roles on the PCB photo">▣ Diagram</button>
        <button class="small" on:click={onRefresh} disabled={busy}
                title="Re-read every config source from the device">↻ Refresh</button>
    </div>

    <div class="right">
        <button class="small state-toggle" class:state-on={rcRouting}
                on:click={toggleRouting} disabled={routingBusy}
                title={rcRouting
                    ? 'RC inputs are driving effects. Click to take manual control (effects ignore RC, hold last state; live monitors keep working).'
                    : 'Manual override: effects ignore RC and hold last state — drive them from Studio. Click to hand control back to RC.'}>
            {rcRouting ? '📡 RC routing' : '✋ Manual'}
        </button>

        {#if $errorLabels.length > 0}
            <span class="status-flag err"
                  title="Resolve validation errors before Apply: {$errorLabels.join(', ')}">
                ⚠ resolve errors: {$errorLabels.join(', ')}
            </span>
        {:else if $dirtyLabels.length > 0}
            <span class="status-flag dirty"
                  title="Unapplied changes in: {$dirtyLabels.join(', ')}">
                ● unapplied: {$dirtyLabels.join(', ')}
            </span>
        {:else}
            <span class="status-flag in-sync" title="All configs match what's on the device.">
                ✓ in sync
            </span>
        {/if}

        <button class="small primary" on:click={onApply}
                disabled={busy || !$anyDirty || $anyErrors}
                title={
                    $anyErrors ? 'Resolve validation errors first' :
                    !$anyDirty ? 'No changes to apply' :
                    `Save + reload ${$dirtyLabels.join(' + ')}`
                }>
            ✓ Apply{$dirtyLabels.length > 0 ? ` (${$dirtyLabels.length})` : ''}
        </button>
    </div>
</div>

{#if lastError}<div class="toolbar-banner err">{lastError}</div>{/if}

<style>
    .config-toolbar {
        display: flex; align-items: center;
        gap: 12px;
        padding: 6px 12px;
        background: var(--bg-raised);
        border-bottom: 1px solid var(--border);
        flex-shrink: 0;
    }
    .left, .right {
        display: flex; align-items: center; gap: 8px;
    }
    .right { margin-left: auto; }
    .config-toolbar button {
        height: 28px;
        min-width: 108px;
        box-sizing: border-box;
        padding: 0 12px;
    }

    /* RC-routing toggle uses the shared button.state-toggle look (Rule
       45) so it's identical to the per-effect on/off toggles — see
       style.css.  No local skin. */

    /* Status flag is sized to match the toolbar buttons exactly — same
       28px height + box model — so the sync pill, RC toggle, and Apply
       read as one uniform control row. */
    .status-flag {
        height: 28px;
        display: inline-flex;
        align-items: center;
        box-sizing: border-box;
        font-size: 11px;
        font-family: var(--font-mono);
        padding: 0 12px;
        border-radius: 4px;
        border: 1px solid var(--border);
        text-transform: lowercase;
        letter-spacing: 0.2px;
    }
    .status-flag.in-sync {
        color: var(--success, #4ec9b0);
        background: color-mix(in srgb, var(--success, #4ec9b0) 10%, transparent);
        border-color: color-mix(in srgb, var(--success, #4ec9b0) 50%, var(--border));
        font-weight: 600;
    }
    .status-flag.dirty {
        color: var(--accent);
        background: color-mix(in srgb, var(--accent) 12%, transparent);
        border-color: color-mix(in srgb, var(--accent) 50%, var(--border));
    }
    .status-flag.err {
        color: var(--error);
        background: color-mix(in srgb, var(--error) 12%, transparent);
        border-color: color-mix(in srgb, var(--error) 50%, var(--border));
        font-weight: 600;
    }

    .toolbar-banner {
        padding: 6px 12px;
        font-size: 12px;
        flex-shrink: 0;
        border-bottom: 1px solid var(--border);
    }
    .toolbar-banner.err {
        background: rgba(255,80,80,0.12);
        color: var(--error);
        border-bottom-color: var(--error);
    }
</style>
