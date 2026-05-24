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
    import { anyDirty, anyErrors, dirtyLabels, errorLabels, applyAll, refreshAll } from '../dirty-registry'
    import { showPcbOverlay } from '../stores'
    import { diag } from '../diag'

    let busy = false

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
        <button class="small" on:click={() => ($showPcbOverlay = true)}
                title="Open the board diagram — assign roles on the PCB photo">▣ Diagram</button>
        <button class="small" on:click={onRefresh} disabled={busy}
                title="Re-read every config source from the device">↻ Refresh</button>
    </div>

    <div class="right">
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

    .status-flag {
        font-size: 11px;
        font-family: var(--font-mono);
        padding: 4px 10px;
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
