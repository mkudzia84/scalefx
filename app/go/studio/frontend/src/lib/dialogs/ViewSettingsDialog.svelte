<!-- ScaleFX Studio — View Settings Dialog -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { showViewSettings } from '../stores'
    import {
        themeChoice, fontSize,
        MIN_FONT_SIZE, MAX_FONT_SIZE, DEFAULT_FONT_SIZE, DEFAULT_THEME
    } from '../theme'
    import type { ThemeChoice } from '../theme'
    import { AssistantStatus, SetAssistantKey, SetAssistantProvider } from '../../../wailsjs/go/main/App'
    import { refreshStatus as refreshAssistant } from '../assistant/store'

    // ─── AI Assistant — provider + key ───  (model is chosen in the Assistant dock)
    interface ProviderInfo { id: string; label: string; hasKey: boolean; keySource: string }
    interface AiStatus { available: boolean; provider: string; model: string; keySource: string; providers: ProviderInfo[] }
    let aiStatus: AiStatus | null = null
    let keyInput = ''
    let aiSaving = false
    let aiMsg = ''
    const selValue = (e: Event) => (e.target as HTMLSelectElement).value

    async function loadAi() {
        try { aiStatus = (await AssistantStatus()) as AiStatus } catch { aiStatus = null }
    }
    onMount(loadAi)

    async function chooseProvider(id: string) {
        aiSaving = true; aiMsg = ''
        try {
            await SetAssistantProvider(id)
            keyInput = ''
            await loadAi()
            await refreshAssistant()
        } catch (e) { aiMsg = 'Failed: ' + String(e) } finally { aiSaving = false }
    }
    async function saveAiKey() {
        if (!aiStatus) return
        aiSaving = true; aiMsg = ''
        try {
            await SetAssistantKey(aiStatus.provider, keyInput.trim())
            keyInput = ''
            await loadAi()
            await refreshAssistant()
            aiMsg = 'Saved.'
        } catch (e) { aiMsg = 'Failed: ' + String(e) } finally { aiSaving = false }
    }
    async function clearAiKey() {
        if (!aiStatus) return
        aiSaving = true; aiMsg = ''
        try {
            await SetAssistantKey(aiStatus.provider, '')
            keyInput = ''
            await loadAi()
            await refreshAssistant()
            aiMsg = 'Cleared.'
        } catch (e) { aiMsg = 'Failed: ' + String(e) } finally { aiSaving = false }
    }
    function activeLabel(s: AiStatus | null): string {
        if (!s || !s.providers) return 'AI'
        const p = s.providers.find(x => x.id === s.provider)
        return p ? p.label : 'AI'
    }
    function aiStateLabel(s: AiStatus | null): string {
        if (!s) return ''
        if (!s.available) return 'Not configured — paste a key for this provider to enable the assistant.'
        return s.keySource === 'builtin' ? 'Enabled via built-in key.' : 'Enabled · key stored locally.'
    }

    function close() {
        $showViewSettings = false
    }

    function handleKeydown(e: KeyboardEvent) {
        if (e.key === 'Escape') close()
    }

    function resetDefaults() {
        $themeChoice = DEFAULT_THEME
        $fontSize = DEFAULT_FONT_SIZE
    }

    const themes: { value: ThemeChoice; label: string; desc: string }[] = [
        { value: 'auto',  label: 'OS Default', desc: 'Follow system theme' },
        { value: 'dark',  label: 'Dark',       desc: 'Dark background' },
        { value: 'light', label: 'Light',      desc: 'Light background' },
        { value: 'gray',  label: 'Gray',       desc: 'Neutral mid-tone' },
    ]
</script>

<svelte:window on:keydown={handleKeydown} />

<div class="modal-backdrop" on:click|self={close}>
    <div class="modal settings-modal">
        <h2>Settings</h2>

        <!-- Theme selection -->
        <div class="setting-group">
            <span class="setting-label">Theme</span>
            <div class="theme-options">
                {#each themes as t}
                    <label class="theme-option" class:active={$themeChoice === t.value}>
                        <input type="radio" name="theme" value={t.value}
                               bind:group={$themeChoice} />
                        <span class="theme-swatch" data-theme-preview={t.value}></span>
                        <span class="theme-name">{t.label}</span>
                    </label>
                {/each}
            </div>
        </div>

        <!-- Font size -->
        <div class="setting-group">
            <span class="setting-label">Font Size</span>
            <div class="font-size-control">
                <input type="range" class="font-slider"
                       bind:value={$fontSize}
                       min={MIN_FONT_SIZE} max={MAX_FONT_SIZE} step={1} />
                <span class="font-value">{$fontSize}px</span>
            </div>
            <span class="setting-hint">
                Affects all text in the application ({MIN_FONT_SIZE}–{MAX_FONT_SIZE} px)
            </span>
        </div>

        <!-- AI Assistant — provider + key -->
        <div class="setting-group">
            <span class="setting-label">AI Assistant</span>
            <div class="ai-row">
                <span class="ai-sublabel">Provider</span>
                <select class="ai-key" value={aiStatus ? aiStatus.provider : 'gemini'}
                        on:change={(e) => chooseProvider(selValue(e))} disabled={aiSaving}>
                    {#each ((aiStatus && aiStatus.providers) || []) as p}
                        <option value={p.id}>{p.label}{p.hasKey ? '  ✓ key set' : ''}</option>
                    {/each}
                </select>
            </div>
            <div class="ai-row">
                <input class="ai-key" type="password"
                       placeholder={`Paste ${activeLabel(aiStatus)} API key…`}
                       bind:value={keyInput} disabled={aiSaving} />
                <button class="primary" on:click={saveAiKey} disabled={aiSaving || !keyInput.trim()}>Save</button>
                <button on:click={clearAiKey}
                        disabled={aiSaving || !(aiStatus && aiStatus.keySource === 'settings')}
                        title="Remove the locally-stored key for this provider">Clear</button>
            </div>
            <span class="setting-hint">
                {aiStateLabel(aiStatus)}{aiMsg ? ` — ${aiMsg}` : ''}<br />
                Powers the Assistant dock; pick the model there. Keys are stored locally on this machine
                (never uploaded or committed). Free keys: Gemini → Google AI Studio · Groq → console.groq.com ·
                Mistral → console.mistral.ai.
            </span>
        </div>

        <div class="modal-actions">
            <button on:click={resetDefaults}>Reset Defaults</button>
            <div style="flex:1"></div>
            <button class="primary" on:click={close}>Close</button>
        </div>
    </div>
</div>

<style>
    .settings-modal {
        min-width: 380px;
        max-width: 420px;
    }

    .setting-group {
        margin-bottom: 20px;
    }

    .setting-label {
        display: block;
        font-size: 12px;
        font-weight: 600;
        color: var(--text-dim);
        text-transform: uppercase;
        letter-spacing: 0.5px;
        margin-bottom: 8px;
    }

    /* ─── Theme radio buttons ─── */

    .theme-options {
        display: flex;
        gap: 8px;
    }

    .theme-option {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 5px;
        padding: 8px 10px;
        border: 1px solid var(--border);
        border-radius: 5px;
        cursor: pointer;
        flex: 1;
        transition: border-color 0.15s, background 0.15s;
    }

    .theme-option:hover {
        border-color: var(--text-dim);
    }

    .theme-option.active {
        border-color: var(--accent);
        background: rgba(0, 120, 212, 0.08);
    }

    .theme-option input[type="radio"] {
        display: none;
    }

    .theme-swatch {
        width: 32px;
        height: 22px;
        border-radius: 3px;
        border: 1px solid rgba(128, 128, 128, 0.3);
    }

    .theme-swatch[data-theme-preview="auto"] {
        background: linear-gradient(135deg, #1e1e1e 50%, #f3f3f3 50%);
    }

    .theme-swatch[data-theme-preview="dark"] {
        background: #1e1e1e;
    }

    .theme-swatch[data-theme-preview="light"] {
        background: #f3f3f3;
    }

    .theme-swatch[data-theme-preview="gray"] {
        background: #333333;
    }

    .theme-name {
        font-size: 11px;
        color: var(--text);
    }

    /* ─── Font size slider ─── */

    .font-size-control {
        display: flex;
        align-items: center;
        gap: 12px;
    }

    .font-slider {
        flex: 1;
        accent-color: var(--accent);
        height: 4px;
    }

    .font-value {
        font-family: var(--font-mono);
        font-size: 13px;
        color: var(--text-bright);
        min-width: 36px;
        text-align: right;
    }

    .setting-hint {
        display: block;
        font-size: 11px;
        color: var(--text-dim);
        margin-top: 4px;
        line-height: 1.5;
    }

    /* ─── AI assistant key ─── */

    .ai-row {
        display: flex;
        gap: 6px;
        align-items: center;
        margin-bottom: 6px;
    }

    .ai-sublabel {
        font-size: 11px;
        color: var(--text-dim);
        min-width: 56px;
    }

    .ai-key {
        flex: 1;
        background: var(--bg-input);
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 4px;
        padding: 6px 8px;
        font-size: 12px;
    }

    .ai-key:focus {
        outline: none;
        border-color: var(--accent);
    }
</style>
