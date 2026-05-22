<!-- ScaleFX Studio — Save Configuration Dialog -->
<!-- Single-scroll modal: verification report + editable YAML in one column. -->
<!-- Driven by a BoardConfigDriver<TState> — the dialog is board-agnostic. -->
<script lang="ts">
    import type { VerifyResult, Severity, VerifyIssue } from '../config/config-verifier'
    import { EMPTY_RESULT } from '../config/config-verifier'
    import type { BoardConfigDriver } from '../config/board-driver'
    import { OpenTextFile, SaveTextFile } from '../../../wailsjs/go/main/App'
    import CodeMirrorEditor from '../components/CodeMirrorEditor.svelte'
    import UploadProgressDialog from './UploadProgressDialog.svelte'

    export let driver: BoardConfigDriver<any> | null = null
    export let open: boolean = false
    /** Optional: tab's already-computed verify result. When provided, the dialog
     *  shows this verbatim (single source of truth with the tab) until the user
     *  edits the YAML, at which point the dialog re-verifies locally. */
    export let initialResult: VerifyResult | null = null
    export let onSave: ((yaml: string) => Promise<void> | void) | null = null
    export let onClose: (() => void) | null = null

    let saving = false
    let saveError = ''
    let importError = ''
    let importInfo = ''
    let exportInfo = ''
    let showProgress = false

    let yamlText = ''
    let verifyResult: VerifyResult = EMPTY_RESULT
    let parseErrors: string[] = []
    let parseWarnings: string[] = []
    let dirty = false
    let activeTab: 'report' | 'yaml' = 'report'

    $: boardLabel = driver?.boardLabel ?? 'Configuration'
    $: canSave = verifyResult.valid && !saving && parseErrors.length === 0
    $: hasErrors = verifyResult.counts.error > 0
    $: hasWarnings = verifyResult.counts.warning > 0
    $: issues = verifyResult.issues as VerifyIssue[]

    let _wasOpen = false
    $: {
        if (open && driver && !_wasOpen) { _wasOpen = true; initialize() }
        else if (!open) { _wasOpen = false }
    }

    function initialize() {
        if (!driver) return
        saveError = importError = importInfo = exportInfo = ''
        dirty = false
        try {
            const state = driver.buildState()
            yamlText = driver.generateYaml(state)
            parseErrors = []
            parseWarnings = []
            // Prefer the tab's already-computed result (keeps both in lockstep);
            // fall back to re-verifying here if the parent didn't supply one.
            verifyResult = initialResult ?? driver.verify(state)
        } catch (err: any) {
            yamlText = `# Error generating configuration:\n# ${err?.message || err}`
            parseErrors = [`Generator threw: ${err?.message || err}`]
            parseWarnings = []
            verifyResult = EMPTY_RESULT
            console.error('[SaveConfigDialog] initialize() failed:', err)
        }
    }

    // While the dialog is open and the user hasn't edited YAML, keep the
    // dialog's verifyResult in sync with the tab's live result — so a
    // background state change (e.g. reconciled broadcast) reflects immediately.
    $: if (open && !dirty && initialResult) verifyResult = initialResult

    function onYamlEdit() {
        if (!driver) return
        dirty = true
        importInfo = exportInfo = ''
        try {
            const result = driver.parseYaml(yamlText)
            parseErrors = result.errors
            parseWarnings = result.warnings
            if (result.state) verifyResult = driver.verify(result.state)
        } catch (err: any) {
            parseErrors = [`Parser threw: ${err?.message || err}`]
            parseWarnings = []
        }
    }

    function close() {
        if (saving) return
        open = false
        onClose?.()
    }

    async function doSave() {
        if (saving || !driver) return
        if (parseErrors.length > 0 || hasErrors) return
        if (!canSave) return
        saving = true
        saveError = ''
        showProgress = true
        try {
            await onSave?.(yamlText)
            saving = false
            // Leave the progress dialog open so the user sees the completion
            // summary (speed + MD5). Close this dialog; user closes the popup.
            close()
        } catch (err: any) {
            // Wails rejects Go errors with a plain string, not an Error object —
            // fall back through string / message / String(err) so we always
            // surface whatever the binding gave us.
            saveError = (typeof err === 'string' && err) || err?.message || String(err) || 'Save failed'
            console.error('[SaveConfigDialog] save failed:', err)
            saving = false
        }
    }

    async function doImport() {
        if (!driver) return
        importError = importInfo = ''
        try {
            const file = await OpenTextFile(`Import ${boardLabel} Configuration`)
            if (!file || !file.path) return
            const result = driver.parseYaml(file.content)
            if (result.errors.length > 0) { importError = `Parse failed:\n${result.errors.join('\n')}`; return }
            if (!result.state) { importError = 'Parse produced no state'; return }
            driver.applyState(result.state)
            const snap = driver.buildState()
            yamlText = driver.generateYaml(snap)
            parseErrors = []
            parseWarnings = result.warnings
            verifyResult = driver.verify(snap)
            dirty = false
            const fname = file.path.split(/[\\/]/).pop() ?? file.path
            importInfo = `Imported ${fname} — applied to tab`
        } catch (err: any) {
            importError = (typeof err === 'string' && err) || err?.message || String(err) || 'Import failed'
        }
    }

    async function doExport() {
        if (!driver) return
        exportInfo = ''
        try {
            const path = await SaveTextFile(yamlText, `${driver.boardType}-config.yaml`,
                `Export ${boardLabel} Configuration`)
            if (path) {
                const fname = path.split(/[\\/]/).pop() ?? path
                exportInfo = `Exported to ${fname}`
            }
        } catch (err: any) {
            exportInfo = `Export failed: ${err?.message || err}`
        }
    }

    async function copyYaml() {
        await navigator.clipboard.writeText(yamlText)
        exportInfo = 'Copied to clipboard'
    }

    function doReset() {
        if (!driver) return
        const state = driver.buildState()
        yamlText = driver.generateYaml(state)
        parseErrors = []
        parseWarnings = []
        verifyResult = driver.verify(state)
        dirty = false
    }

    function handleKeydown(e: KeyboardEvent) {
        if (!open) return
        if (e.key === 'Escape' && !saving) close()
    }

    function sevIcon(sev: Severity): string {
        if (sev === 'error')   return '\u2717'
        if (sev === 'warning') return '\u26A0'
        return '\u2139'
    }
    function sevColor(sev: Severity): string {
        if (sev === 'error')   return 'var(--error)'
        if (sev === 'warning') return 'var(--warning)'
        return 'var(--info)'
    }
</script>

<svelte:window on:keydown={handleKeydown} />

{#if open}
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <!-- svelte-ignore a11y-no-static-element-interactions -->
    <div class="sc-backdrop" on:click|self={close}>
        <div class="sc-dialog">

            <header class="sc-header">
                <h2>
                    {#if saving}<span class="sc-spin">⟳</span> Saving…
                    {:else if hasErrors}<span style="color: var(--error)">✗</span> Save Configuration — {boardLabel}
                    {:else}Save Configuration — {boardLabel}
                    {/if}
                </h2>
                {#if !saving}
                    <button class="sc-close" on:click={close} title="Close">✕</button>
                {/if}
            </header>

            <div class="sc-strip"
                 class:sc-strip-ok={verifyResult.valid && parseErrors.length === 0}
                 class:sc-strip-err={hasErrors || parseErrors.length > 0}
                 class:sc-strip-warn={!hasErrors && parseErrors.length === 0 && hasWarnings}>
                {#if parseErrors.length > 0}
                    <span>✗</span><span>{parseErrors.length} YAML parse error{parseErrors.length !== 1 ? 's' : ''} — fix to save</span>
                {:else if verifyResult.valid && issues.length === 0}
                    <span>✓</span><span>Configuration is valid — ready to save</span>
                {:else if verifyResult.valid}
                    <span>⚠</span><span>Valid with {verifyResult.counts.warning} warning{verifyResult.counts.warning !== 1 ? 's' : ''}</span>
                {:else}
                    <span>✗</span><span>{verifyResult.counts.error} error{verifyResult.counts.error !== 1 ? 's' : ''} — fix before saving</span>
                {/if}
            </div>

            <div class="sc-tabs" role="tablist">
                <button class="sc-tab" class:sc-tab-active={activeTab === 'report'}
                        role="tab" aria-selected={activeTab === 'report'}
                        on:click={() => activeTab = 'report'}>
                    Verification Report
                    {#if verifyResult.counts.error > 0 || parseErrors.length > 0}
                        <span class="sc-tab-badge sc-tab-badge-err">{verifyResult.counts.error + parseErrors.length}</span>
                    {:else if verifyResult.counts.warning > 0 || parseWarnings.length > 0}
                        <span class="sc-tab-badge sc-tab-badge-warn">{verifyResult.counts.warning + parseWarnings.length}</span>
                    {:else if verifyResult.counts.info > 0}
                        <span class="sc-tab-badge sc-tab-badge-info">{verifyResult.counts.info}</span>
                    {:else}
                        <span class="sc-tab-badge sc-tab-badge-ok">✓</span>
                    {/if}
                </button>
                <button class="sc-tab" class:sc-tab-active={activeTab === 'yaml'}
                        role="tab" aria-selected={activeTab === 'yaml'}
                        on:click={() => activeTab = 'yaml'}>
                    YAML Configuration{dirty ? ' •' : ''}
                </button>
            </div>

            <div class="sc-body">

                <!-- ═══ Verification report ═══ -->
                {#if activeTab === 'report'}
                <section class="sc-section sc-report-section">
                    <div class="sc-section-head">
                        <span class="sc-section-title">Verification Report</span>
                        <span class="sc-badges">
                            {#if verifyResult.counts.error > 0}
                                <span class="sc-badge sc-badge-err">{verifyResult.counts.error} Error{verifyResult.counts.error !== 1 ? 's' : ''}</span>
                            {/if}
                            {#if verifyResult.counts.warning > 0}
                                <span class="sc-badge sc-badge-warn">{verifyResult.counts.warning} Warning{verifyResult.counts.warning !== 1 ? 's' : ''}</span>
                            {/if}
                            {#if verifyResult.counts.info > 0}
                                <span class="sc-badge sc-badge-info">{verifyResult.counts.info} Note{verifyResult.counts.info !== 1 ? 's' : ''}</span>
                            {/if}
                            {#if issues.length === 0 && parseErrors.length === 0}
                                <span class="sc-badge sc-badge-ok">Clean</span>
                            {/if}
                        </span>
                    </div>

                    {#if parseErrors.length > 0}
                        <ul class="sc-issues">
                            {#each parseErrors as err}
                                <li class="sc-issue sc-issue-err">
                                    <span class="sc-issue-icon" style="color: var(--error)">✗</span>
                                    <span class="sc-issue-msg">YAML parse: {err}</span>
                                </li>
                            {/each}
                        </ul>
                    {/if}

                    {#if parseWarnings.length > 0}
                        <ul class="sc-issues">
                            {#each parseWarnings as w}
                                <li class="sc-issue sc-issue-warn">
                                    <span class="sc-issue-icon" style="color: var(--warning)">⚠</span>
                                    <span class="sc-issue-msg">YAML: {w}</span>
                                </li>
                            {/each}
                        </ul>
                    {/if}

                    {#if issues.length === 0 && parseErrors.length === 0 && parseWarnings.length === 0}
                        <div class="sc-empty">No issues found — configuration is clean.</div>
                    {:else if issues.length > 0}
                        <ul class="sc-issues">
                            {#each issues as issue}
                                <li class="sc-issue" class:sc-issue-err={issue.severity === 'error'}
                                    class:sc-issue-warn={issue.severity === 'warning'}
                                    class:sc-issue-info={issue.severity === 'info'}>
                                    <span class="sc-issue-icon" style="color: {sevColor(issue.severity)}">{sevIcon(issue.severity)}</span>
                                    <span class="sc-issue-msg">
                                        {issue.message}
                                        {#if issue.path}<span class="sc-issue-path">[{issue.path}]</span>{/if}
                                    </span>
                                </li>
                            {/each}
                        </ul>
                    {/if}
                </section>
                {/if}

                <!-- ═══ Generated YAML ═══ -->
                {#if activeTab === 'yaml'}
                <section class="sc-section sc-yaml-section">
                    <div class="sc-section-head">
                        <span class="sc-section-title">Generated YAML {dirty ? '(edited)' : ''}</span>
                        <span class="sc-yaml-btns">
                            <button class="sc-btn-small" on:click={doImport} title="Import YAML from local file">Import…</button>
                            <button class="sc-btn-small" on:click={doExport} title="Export YAML to local file">Export…</button>
                            <button class="sc-btn-small" on:click={copyYaml} title="Copy to clipboard">Copy</button>
                            {#if dirty}
                                <button class="sc-btn-small" on:click={doReset} title="Discard edits, reload from tab state">Reset</button>
                            {/if}
                        </span>
                    </div>

                    {#if importError}<div class="sc-msg sc-msg-err">{importError}</div>{/if}
                    {#if importInfo}<div class="sc-msg sc-msg-ok">{importInfo}</div>{/if}
                    {#if exportInfo}<div class="sc-msg sc-msg-ok">{exportInfo}</div>{/if}

                    <CodeMirrorEditor bind:value={yamlText}
                                      placeholder="# (empty configuration)"
                                      on:change={onYamlEdit} />
                </section>
                {/if}

            </div>

            <footer class="sc-footer">
                {#if saveError}<span class="sc-save-err">✗ {saveError}</span>{/if}
                <div class="sc-spacer"></div>
                <button on:click={close} disabled={saving}>Cancel</button>
                <button class="sc-primary" class:sc-blocked={!canSave && !saving}
                        on:click={doSave} disabled={saving}
                        title={parseErrors.length > 0 ? 'Fix YAML parse errors first'
                             : hasErrors ? `Fix ${verifyResult.counts.error} error(s)`
                             : 'Save configuration to device'}>
                    {#if saving}Saving…
                    {:else if parseErrors.length > 0 || hasErrors}✗ Cannot save
                    {:else}💾 Save to Device
                    {/if}
                </button>
            </footer>

        </div>
    </div>
{/if}

<UploadProgressDialog bind:open={showProgress} title={`Save Configuration — ${boardLabel}`} />

<style>
    .sc-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.5);
        z-index: 150;
        display: flex;
        align-items: center;
        justify-content: center;
    }

    .sc-dialog {
        background: var(--bg-surface);
        border: 1px solid var(--border);
        border-radius: 8px;
        box-shadow: 0 12px 48px var(--shadow);
        width: 820px;
        max-width: 92vw;
        height: 80vh;
        max-height: 80vh;
        display: flex;
        flex-direction: column;
        color: var(--text);
    }

    .sc-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 14px 20px 10px;
    }
    .sc-header h2 {
        font-size: 15px;
        font-weight: 600;
        color: var(--text-bright);
        margin: 0;
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .sc-close {
        background: none;
        border: none;
        color: var(--text-dim);
        font-size: 16px;
        cursor: pointer;
        padding: 4px 8px;
        border-radius: 4px;
    }
    .sc-close:hover { color: var(--text); background: var(--bg-raised); }

    .sc-spin { display: inline-block; animation: sc-spin 1s linear infinite; }
    @keyframes sc-spin { to { transform: rotate(360deg) } }

    .sc-strip {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 20px;
        font-size: 12px;
        font-weight: 500;
        border-top: 1px solid var(--border);
        border-bottom: 1px solid var(--border);
    }
    .sc-strip-ok   { background: color-mix(in srgb, var(--success) 10%, var(--bg-surface)); color: var(--success); }
    .sc-strip-err  { background: color-mix(in srgb, var(--error)   10%, var(--bg-surface)); color: var(--error); }
    .sc-strip-warn { background: color-mix(in srgb, var(--warning) 10%, var(--bg-surface)); color: var(--warning); }

    .sc-tabs {
        display: flex;
        gap: 2px;
        padding: 0 20px;
        border-bottom: 1px solid var(--border);
        background: var(--bg-surface);
    }
    .sc-tab {
        background: transparent;
        color: var(--text-dim);
        border: none;
        border-bottom: 2px solid transparent;
        padding: 10px 14px;
        font-size: 12px;
        font-weight: 500;
        cursor: pointer;
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: -1px;
    }
    .sc-tab:hover { color: var(--text); }
    .sc-tab-active {
        color: var(--text-bright);
        border-bottom-color: var(--accent);
    }
    .sc-tab-badge {
        font-size: 10px;
        font-weight: 600;
        padding: 1px 7px;
        border-radius: 8px;
        line-height: 1.3;
        min-width: 16px;
        text-align: center;
    }
    .sc-tab-badge-err  { background: color-mix(in srgb, var(--error)   20%, var(--bg-raised)); color: var(--error); }
    .sc-tab-badge-warn { background: color-mix(in srgb, var(--warning) 20%, var(--bg-raised)); color: var(--warning); }
    .sc-tab-badge-info { background: color-mix(in srgb, var(--info)    15%, var(--bg-raised)); color: var(--info); }
    .sc-tab-badge-ok   { background: color-mix(in srgb, var(--success) 20%, var(--bg-raised)); color: var(--success); }

    .sc-body {
        flex: 1 1 auto;
        min-height: 0;
        overflow: hidden;
        padding: 14px 20px;
        display: flex;
        flex-direction: column;
        gap: 16px;
    }

    .sc-section {
        display: flex;
        flex-direction: column;
        gap: 8px;
        min-height: 0;
    }
    .sc-yaml-section,
    .sc-report-section {
        flex: 1 1 auto;
        min-height: 180px;
    }
    .sc-report-section { overflow: hidden; }
    .sc-section-head {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        flex-wrap: wrap;
    }
    .sc-section-title {
        font-size: 11px;
        font-weight: 600;
        text-transform: uppercase;
        letter-spacing: 0.5px;
        color: var(--text-dim);
    }

    .sc-badges { display: flex; gap: 6px; flex-wrap: wrap; }
    .sc-badge {
        font-size: 11px;
        font-weight: 600;
        padding: 2px 10px;
        border-radius: 10px;
    }
    .sc-badge-err  { background: color-mix(in srgb, var(--error)   15%, var(--bg-raised)); color: var(--error); }
    .sc-badge-warn { background: color-mix(in srgb, var(--warning) 15%, var(--bg-raised)); color: var(--warning); }
    .sc-badge-info { background: color-mix(in srgb, var(--info)    12%, var(--bg-raised)); color: var(--info); }
    .sc-badge-ok   { background: color-mix(in srgb, var(--success) 15%, var(--bg-raised)); color: var(--success); }

    .sc-issues {
        list-style: none;
        margin: 0;
        padding: 0;
        display: flex;
        flex-direction: column;
        gap: 4px;
        overflow-y: auto;
        flex: 1 1 auto;
        min-height: 0;
    }
    .sc-issue {
        display: flex;
        align-items: flex-start;
        gap: 8px;
        padding: 8px 10px;
        border-radius: 4px;
        font-size: 12px;
        line-height: 1.4;
        color: var(--text);
        background: var(--bg-raised);
        border: 1px solid var(--border);
    }
    .sc-issue-err  { background: color-mix(in srgb, var(--error)   12%, var(--bg-raised)); border-color: color-mix(in srgb, var(--error)   40%, var(--border)); }
    .sc-issue-warn { background: color-mix(in srgb, var(--warning) 12%, var(--bg-raised)); border-color: color-mix(in srgb, var(--warning) 40%, var(--border)); }
    .sc-issue-info { background: color-mix(in srgb, var(--info)     8%, var(--bg-raised)); border-color: color-mix(in srgb, var(--info)    30%, var(--border)); }

    .sc-issue-icon { font-size: 13px; font-weight: 700; flex-shrink: 0; margin-top: 1px; min-width: 14px; }
    .sc-issue-msg { flex: 1 1 auto; }
    .sc-issue-path {
        display: inline-block;
        margin-left: 6px;
        font-family: var(--font-mono);
        font-size: 10px;
        color: var(--text-dim);
    }

    .sc-empty {
        padding: 16px;
        text-align: center;
        color: var(--success);
        font-size: 13px;
        background: color-mix(in srgb, var(--success) 6%, var(--bg-raised));
        border: 1px solid color-mix(in srgb, var(--success) 25%, var(--border));
        border-radius: 4px;
    }

    .sc-yaml-btns { display: flex; gap: 6px; flex-wrap: wrap; }
    .sc-btn-small {
        font-size: 11px;
        padding: 3px 10px;
        background: var(--bg-raised);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        cursor: pointer;
    }
    .sc-btn-small:hover { background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised)); }

    .sc-msg {
        padding: 6px 10px;
        border-radius: 4px;
        font-size: 11px;
        line-height: 1.4;
        font-family: var(--font-mono);
        white-space: pre-wrap;
    }
    .sc-msg-err { background: color-mix(in srgb, var(--error)   12%, transparent); color: var(--error); }
    .sc-msg-ok  { background: color-mix(in srgb, var(--success) 12%, transparent); color: var(--success); }

    .sc-footer {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 12px 20px;
        border-top: 1px solid var(--border);
    }
    .sc-spacer { flex: 1; }
    .sc-save-err { font-size: 12px; color: var(--error); font-weight: 500; }

    .sc-footer button {
        font-size: 13px;
        padding: 6px 16px;
        background: var(--bg-raised);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        cursor: pointer;
    }
    .sc-footer button:disabled { opacity: 0.5; cursor: not-allowed; }
    .sc-footer button:hover:not(:disabled) { background: color-mix(in srgb, var(--accent) 15%, var(--bg-raised)); }

    .sc-primary {
        background: var(--accent) !important;
        color: var(--bg-base) !important;
        border-color: var(--accent) !important;
        font-weight: 600 !important;
    }
    .sc-blocked {
        background: color-mix(in srgb, var(--error) 25%, var(--bg-raised)) !important;
        color: var(--error) !important;
        border-color: var(--error) !important;
    }
</style>
