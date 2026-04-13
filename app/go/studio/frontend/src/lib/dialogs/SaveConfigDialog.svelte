<!-- ScaleFX Studio — Save Configuration Dialog -->
<!-- Two-tab modal: Summary (verification report + settings overview) and YAML (generated config). -->
<!-- Save button only enabled when verification passes (zero errors). -->
<script lang="ts">
    import type { VerifyResult, VerifyIssue, Severity } from '../config/config-verifier'
    import { EMPTY_RESULT } from '../config/config-verifier'

    // ─── Props ───

    /** Board type for display and save behavior */
    export let boardType: string = 'lightfx'
    /** Board label for the dialog title */
    export let boardLabel: string = 'LightFX'
    /** Verification result from the board-specific verifier */
    export let verifyResult: VerifyResult = EMPTY_RESULT
    /** Generated YAML/XML config text */
    export let configText: string = ''
    /** Whether the dialog is visible (bind:this from parent) */
    export let open: boolean = false
    /** Callback when save is confirmed */
    export let onSave: (() => void) | null = null
    /** Callback when dialog is closed */
    export let onClose: (() => void) | null = null

    // ─── State ───
    let activeDialogTab: 'summary' | 'yaml' = 'summary'
    let saving = false
    let saveError = ''

    $: canSave = verifyResult.valid && !saving
    $: totalIssues = verifyResult.issues.length
    $: hasErrors = verifyResult.counts.error > 0
    $: hasWarnings = verifyResult.counts.warning > 0

    // ─── Actions ───

    function close() {
        if (saving) return
        open = false
        saveError = ''
        activeDialogTab = 'summary'
        onClose?.()
    }

    async function doSave() {
        if (!canSave) return
        saving = true
        saveError = ''
        try {
            onSave?.()
            // Brief pause to show success state
            setTimeout(() => {
                saving = false
                close()
            }, 500)
        } catch (err: any) {
            saveError = err?.message || 'Save failed'
            saving = false
        }
    }

    function handleKeydown(e: KeyboardEvent) {
        if (!open) return
        if (e.key === 'Escape' && !saving) close()
    }

    function copyYaml() {
        navigator.clipboard.writeText(configText)
    }

    // ─── Severity helpers ───

    function sevIcon(sev: Severity): string {
        switch (sev) {
            case 'error':   return '✗'
            case 'warning': return '⚠'
            case 'info':    return 'ℹ'
        }
    }

    function sevColor(sev: Severity): string {
        switch (sev) {
            case 'error':   return 'var(--error)'
            case 'warning': return 'var(--warning)'
            case 'info':    return 'var(--info)'
        }
    }
</script>

<svelte:window on:keydown={handleKeydown} />

{#if open}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <div class="modal-backdrop save-backdrop" on:click|self={close}>
        <div class="save-dialog">

            <!-- Header -->
            <div class="save-header">
                <h2>
                    {#if saving}
                        <span class="header-spin">⟳</span> Saving…
                    {:else if hasErrors}
                        <span style="color: var(--error)">✗</span> Save Configuration — {boardLabel}
                    {:else}
                        Save Configuration — {boardLabel}
                    {/if}
                </h2>
                {#if !saving}
                    <button class="close-btn" on:click={close} title="Close">✕</button>
                {/if}
            </div>

            <!-- Verification status strip -->
            <div class="verify-strip" class:strip-ok={verifyResult.valid}
                 class:strip-error={hasErrors} class:strip-warn={!hasErrors && hasWarnings}>
                {#if verifyResult.valid && totalIssues === 0}
                    <span class="strip-icon ok">✓</span>
                    <span>Configuration is valid — ready to save</span>
                {:else if verifyResult.valid}
                    <span class="strip-icon warn">⚠</span>
                    <span>Valid with {verifyResult.counts.warning} warning{verifyResult.counts.warning !== 1 ? 's' : ''}</span>
                {:else}
                    <span class="strip-icon err">✗</span>
                    <span>{verifyResult.counts.error} error{verifyResult.counts.error !== 1 ? 's' : ''} — fix before saving</span>
                {/if}
                {#if verifyResult.counts.info > 0}
                    <span class="strip-info-count">{verifyResult.counts.info} note{verifyResult.counts.info !== 1 ? 's' : ''}</span>
                {/if}
            </div>

            <!-- Tab bar -->
            <div class="dialog-tabs">
                <button class="dtab" class:active={activeDialogTab === 'summary'}
                        on:click={() => activeDialogTab = 'summary'}>
                    Summary
                    {#if totalIssues > 0}
                        <span class="tab-badge" class:badge-err={hasErrors}
                              class:badge-warn={!hasErrors && hasWarnings}>{totalIssues}</span>
                    {/if}
                </button>
                <button class="dtab" class:active={activeDialogTab === 'yaml'}
                        on:click={() => activeDialogTab = 'yaml'}>
                    Generated YAML
                </button>
            </div>

            <!-- Tab content -->
            <div class="dialog-body">
                {#if activeDialogTab === 'summary'}
                    <!-- ═══ Summary Tab ═══ -->
                    <div class="summary-tab">
                        {#if totalIssues === 0}
                            <div class="empty-state">
                                <span class="empty-icon">✓</span>
                                <span>No issues found — configuration is clean.</span>
                            </div>
                        {:else}
                            <div class="issue-list">
                                {#each verifyResult.issues as issue}
                                    <div class="issue-row" class:issue-error={issue.severity === 'error'}
                                         class:issue-warning={issue.severity === 'warning'}
                                         class:issue-info={issue.severity === 'info'}>
                                        <span class="issue-icon" style="color: {sevColor(issue.severity)}">
                                            {sevIcon(issue.severity)}
                                        </span>
                                        <div class="issue-content">
                                            <span class="issue-msg">{issue.message}</span>
                                            {#if issue.path}
                                                <span class="issue-path">{issue.path}</span>
                                            {/if}
                                        </div>
                                    </div>
                                {/each}
                            </div>
                        {/if}

                        <!-- Counts summary -->
                        <div class="counts-row">
                            {#if verifyResult.counts.error > 0}
                                <span class="count-badge err">{verifyResult.counts.error} Error{verifyResult.counts.error !== 1 ? 's' : ''}</span>
                            {/if}
                            {#if verifyResult.counts.warning > 0}
                                <span class="count-badge warn">{verifyResult.counts.warning} Warning{verifyResult.counts.warning !== 1 ? 's' : ''}</span>
                            {/if}
                            {#if verifyResult.counts.info > 0}
                                <span class="count-badge info">{verifyResult.counts.info} Note{verifyResult.counts.info !== 1 ? 's' : ''}</span>
                            {/if}
                        </div>
                    </div>

                {:else}
                    <!-- ═══ YAML Tab ═══ -->
                    <div class="yaml-tab">
                        <div class="yaml-toolbar">
                            <span class="yaml-label">Generated configuration ({boardType})</span>
                            <button class="small" on:click={copyYaml} title="Copy to clipboard">📋 Copy</button>
                        </div>
                        <pre class="yaml-code">{configText || '# (empty configuration)'}</pre>
                    </div>
                {/if}
            </div>

            <!-- Footer -->
            <div class="save-footer">
                {#if saveError}
                    <span class="save-error">✗ {saveError}</span>
                {/if}
                <div style="flex: 1"></div>
                <button on:click={close} disabled={saving}>Cancel</button>
                <button class="primary" on:click={doSave} disabled={!canSave}
                        title={hasErrors ? 'Fix errors before saving' : 'Save configuration to device'}>
                    {#if saving}
                        Saving…
                    {:else}
                        💾 Save to Device
                    {/if}
                </button>
            </div>
        </div>
    </div>
{/if}

<style>
    /* ─── Backdrop ─── */
    .save-backdrop {
        z-index: 150;
    }

    /* ─── Dialog ─── */
    .save-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 720px;
        max-width: 92vw;
        max-height: 85vh;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }

    /* ─── Header ─── */
    .save-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 16px 20px 10px;
        flex-shrink: 0;
    }

    .save-header h2 {
        font-size: 15px;
        font-weight: 600;
        color: var(--text-bright);
        margin: 0;
        display: flex;
        align-items: center;
        gap: 8px;
    }

    .close-btn {
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 16px;
        cursor: pointer;
        padding: 4px 8px;
        border-radius: 4px;
    }
    .close-btn:hover { color: var(--text); background: var(--bg-raised); }

    .header-spin {
        display: inline-block;
        animation: spin 1s linear infinite;
    }
    @keyframes spin { to { transform: rotate(360deg) } }

    /* ─── Verify Strip ─── */
    .verify-strip {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 20px;
        font-size: 12px;
        font-weight: 500;
        border-top: 1px solid var(--border);
        border-bottom: 1px solid var(--border);
    }
    .strip-ok   { background: color-mix(in srgb, var(--success) 10%, var(--bg-surface)); color: var(--success); }
    .strip-error { background: color-mix(in srgb, var(--error) 10%, var(--bg-surface)); color: var(--error); }
    .strip-warn  { background: color-mix(in srgb, var(--warning) 10%, var(--bg-surface)); color: var(--warning); }

    .strip-icon { font-weight: 700; font-size: 14px; }
    .strip-icon.ok   { color: var(--success); }
    .strip-icon.err  { color: var(--error); }
    .strip-icon.warn { color: var(--warning); }

    .strip-info-count {
        margin-left: auto;
        font-size: 11px;
        color: var(--text-dim);
    }

    /* ─── Dialog Tabs ─── */
    .dialog-tabs {
        display: flex;
        gap: 0;
        padding: 0 20px;
        border-bottom: 1px solid var(--border);
        flex-shrink: 0;
    }

    .dtab {
        padding: 8px 16px;
        font-size: 12px;
        font-weight: 500;
        color: var(--text-dim);
        background: none;
        border: none;
        border-bottom: 2px solid transparent;
        cursor: pointer;
        display: flex;
        align-items: center;
        gap: 6px;
        transition: color 0.15s, border-color 0.15s;
    }
    .dtab:hover { color: var(--text); }
    .dtab.active {
        color: var(--accent);
        border-bottom-color: var(--accent);
    }

    .tab-badge {
        font-size: 10px;
        font-weight: 700;
        padding: 1px 6px;
        border-radius: 10px;
        background: var(--bg-raised);
        color: var(--text-dim);
    }
    .tab-badge.badge-err { background: color-mix(in srgb, var(--error) 20%, var(--bg-raised)); color: var(--error); }
    .tab-badge.badge-warn { background: color-mix(in srgb, var(--warning) 20%, var(--bg-raised)); color: var(--warning); }

    /* ─── Dialog Body ─── */
    .dialog-body {
        flex: 1;
        overflow-y: auto;
        min-height: 200px;
        max-height: 50vh;
    }

    /* ─── Summary Tab ─── */
    .summary-tab {
        padding: 12px 20px;
    }

    .empty-state {
        display: flex;
        align-items: center;
        justify-content: center;
        gap: 10px;
        padding: 32px 0;
        color: var(--success);
        font-size: 14px;
    }
    .empty-icon {
        font-size: 20px;
        font-weight: 700;
    }

    .issue-list {
        display: flex;
        flex-direction: column;
        gap: 4px;
    }

    .issue-row {
        display: flex;
        align-items: flex-start;
        gap: 8px;
        padding: 6px 10px;
        border-radius: 4px;
        font-size: 12px;
    }
    .issue-error   { background: color-mix(in srgb, var(--error) 8%, transparent); }
    .issue-warning { background: color-mix(in srgb, var(--warning) 8%, transparent); }
    .issue-info    { background: color-mix(in srgb, var(--info) 5%, transparent); }

    .issue-icon {
        font-size: 13px;
        font-weight: 700;
        flex-shrink: 0;
        margin-top: 1px;
    }

    .issue-content {
        display: flex;
        flex-direction: column;
        gap: 2px;
    }

    .issue-msg {
        color: var(--text);
        line-height: 1.4;
    }

    .issue-path {
        font-family: var(--font-mono);
        font-size: 10px;
        color: var(--text-dim);
    }

    .counts-row {
        display: flex;
        gap: 8px;
        margin-top: 12px;
        padding-top: 10px;
        border-top: 1px solid color-mix(in srgb, var(--border) 50%, transparent);
    }

    .count-badge {
        font-size: 11px;
        font-weight: 600;
        padding: 2px 10px;
        border-radius: 10px;
    }
    .count-badge.err  { background: color-mix(in srgb, var(--error) 15%, var(--bg-raised)); color: var(--error); }
    .count-badge.warn { background: color-mix(in srgb, var(--warning) 15%, var(--bg-raised)); color: var(--warning); }
    .count-badge.info { background: color-mix(in srgb, var(--info) 12%, var(--bg-raised)); color: var(--info); }

    /* ─── YAML Tab ─── */
    .yaml-tab {
        display: flex;
        flex-direction: column;
        height: 100%;
    }

    .yaml-toolbar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 8px 20px;
        flex-shrink: 0;
    }

    .yaml-label {
        font-size: 11px;
        color: var(--text-dim);
        font-weight: 500;
    }

    .yaml-code {
        margin: 0;
        padding: 12px 20px;
        font-family: var(--font-mono);
        font-size: 12px;
        line-height: 1.5;
        color: var(--text);
        background: var(--bg-base);
        overflow: auto;
        flex: 1;
        white-space: pre;
        user-select: text;
        border-top: 1px solid var(--border);
    }

    /* ─── Footer ─── */
    .save-footer {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 12px 20px;
        border-top: 1px solid var(--border);
        flex-shrink: 0;
    }

    .save-error {
        font-size: 12px;
        color: var(--error);
        font-weight: 500;
    }
</style>
