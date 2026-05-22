<!-- ScaleFX Studio — Board Tab -->
<!-- Generic board tab content. Placeholder for future board-specific controls. -->
<script lang="ts">
    import { connectionInfo } from '../stores'

    export let boardType: string = ''
    export let boardLabel: string = 'Board'

    function controllerColor(ct: string): string {
        switch (ct) {
            case 'gearcontrol': return 'var(--success)'
            case 'gunfx': return 'var(--error)'
            case 'hubfx': return 'var(--info)'
            case 'lightfx': return '#569cd6'
            default: return 'var(--accent)'
        }
    }
</script>

<div class="board-tab">
    <div class="board-header">
        <span class="board-icon" style="color: {controllerColor(boardType)}">⎔</span>
        <div class="board-info">
            <h2>{boardLabel}</h2>
            <span class="board-type">{boardType}</span>
        </div>
    </div>

    <div class="board-placeholder">
        <p>Board controls and configuration will appear here.</p>
        <p class="hint">Use <strong>View → Console</strong> or press <kbd>Ctrl+`</kbd> for CLI commands.</p>
    </div>
</div>

<style>
    .board-tab {
        padding: 24px;
    }

    .board-header {
        display: flex;
        align-items: center;
        gap: 16px;
        margin-bottom: 24px;
        padding-bottom: 16px;
        border-bottom: 1px solid var(--border);
    }

    .board-icon {
        font-size: 32px;
    }

    .board-info h2 {
        font-size: 18px;
        font-weight: 600;
        color: var(--text-bright);
        margin-bottom: 2px;
    }

    .board-type {
        font-size: 12px;
        color: var(--text-dim);
        font-family: var(--font-mono);
    }

    .board-placeholder {
        padding: 40px;
        text-align: center;
        color: var(--text-dim);
        line-height: 1.8;
    }

    .hint {
        font-size: 12px;
        margin-top: 8px;
    }

    kbd {
        background: var(--bg-raised);
        border: 1px solid var(--border);
        border-radius: 3px;
        padding: 1px 6px;
        font-family: var(--font-mono);
        font-size: 11px;
    }
</style>
