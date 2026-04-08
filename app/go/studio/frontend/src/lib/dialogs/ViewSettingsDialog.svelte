<!-- ScaleFX Studio — View Settings Dialog -->
<script lang="ts">
    import { showViewSettings } from '../stores'
    import {
        themeChoice, fontSize,
        MIN_FONT_SIZE, MAX_FONT_SIZE, DEFAULT_FONT_SIZE, DEFAULT_THEME
    } from '../theme'
    import type { ThemeChoice } from '../theme'

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
        <h2>View Settings</h2>

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
    }
</style>
