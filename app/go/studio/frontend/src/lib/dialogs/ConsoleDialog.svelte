<!-- ScaleFX Studio — Console Panel -->
<!-- Right-side slide-out console panel embedded in the main layout. -->
<script lang="ts">
    import { onMount, afterUpdate, onDestroy } from 'svelte'
    import { consoleMessages, pushConsoleMessage, connectionInfo, showConsole } from '../stores'
    import type { ConsoleMessage } from '../stores'
    import { EventsOn, EventsOff } from '../../../wailsjs/runtime/runtime'
    import { SendCommand } from '../../../wailsjs/go/main/App'

    let inputValue = ''
    let outputEl: HTMLDivElement
    let inputEl: HTMLInputElement
    let shouldAutoScroll = true

    // Command history
    let history: string[] = []
    let historyIndex = -1
    let savedInput = ''

    onMount(() => {
        EventsOn('console:output', (msg: { type: string; content: string }) => {
            pushConsoleMessage(msg.type as ConsoleMessage['type'], msg.content)
        })
        inputEl?.focus()
    })

    onDestroy(() => {
        EventsOff('console:output')
    })

    afterUpdate(() => {
        if (shouldAutoScroll && outputEl) {
            outputEl.scrollTop = outputEl.scrollHeight
        }
    })

    function onScroll() {
        if (!outputEl) return
        const atBottom = outputEl.scrollHeight - outputEl.scrollTop - outputEl.clientHeight < 40
        shouldAutoScroll = atBottom
    }

    function handleKeydown(e: KeyboardEvent) {
        if (e.key === 'Escape') {
            $showConsole = false
            return
        }
        if (e.key === 'Enter' && inputValue.trim()) {
            const cmd = inputValue.trim()
            history = [...history, cmd]
            historyIndex = -1
            savedInput = ''
            inputValue = ''
            SendCommand(cmd)
        } else if (e.key === 'ArrowUp') {
            e.preventDefault()
            if (history.length === 0) return
            if (historyIndex === -1) {
                savedInput = inputValue
                historyIndex = history.length - 1
            } else if (historyIndex > 0) {
                historyIndex--
            }
            inputValue = history[historyIndex]
        } else if (e.key === 'ArrowDown') {
            e.preventDefault()
            if (historyIndex === -1) return
            if (historyIndex < history.length - 1) {
                historyIndex++
                inputValue = history[historyIndex]
            } else {
                historyIndex = -1
                inputValue = savedInput
            }
        } else if (e.key === 'l' && e.ctrlKey) {
            e.preventDefault()
            $consoleMessages = []
        }
    }

    function handleBodyClick() {
        // Only focus input if no text is selected (allow copy)
        const sel = window.getSelection()
        if (!sel || sel.isCollapsed) {
            inputEl?.focus()
        }
    }

    function icon(type: string): string {
        switch (type) {
            case 'ok': return '<span class="c-green">✓</span>'
            case 'error': return '<span class="c-red">✗</span>'
            case 'info': return '<span class="c-cyan">ℹ</span>'
            case 'warning': return '<span class="c-yellow">⚠</span>'
            case 'command': return '<span class="c-gray">›</span>'
            default: return ''
        }
    }

    $: prompt = $connectionInfo.controllerName
        ? $connectionInfo.controllerName + '>'
        : $connectionInfo.connected
            ? 'connected>'
            : 'scalefx>'

    $: promptClass = $connectionInfo.controllerType
        ? controllerColor($connectionInfo.controllerType)
        : $connectionInfo.connected ? 'c-yellow' : 'c-gray'

    function controllerColor(ct: string): string {
        switch (ct) {
            case 'gearcontrol': return 'c-green'
            case 'gunfx': return 'c-red'
            case 'hubfx': return 'c-cyan'
            case 'lightfx': return 'c-blue'
            default: return 'c-cyan'
        }
    }
</script>

<div class="console-panel">
    <!-- svelte-ignore a11y-click-events-have-key-events -->
    <div class="console-body" on:click={handleBodyClick}>
        <div class="console-output" bind:this={outputEl} on:scroll={onScroll}>
            {#each $consoleMessages as msg}
                <div class="console-line console-{msg.type}">
                    {#if msg.type === 'command'}
                        <span class="cmd-prompt c-gray">›</span>
                        <span class="cmd-text">{msg.content}</span>
                    {:else if msg.type === 'output'}
                        <span class="output-text">{@html msg.content}</span>
                    {:else}
                        <span class="msg-icon">{@html icon(msg.type)}</span>
                        <span class="msg-text">{@html msg.content}</span>
                    {/if}
                </div>
            {/each}
        </div>

        <div class="console-input-row">
            <span class="input-prompt {promptClass}">{prompt}</span>
            <input
                class="console-input"
                bind:this={inputEl}
                bind:value={inputValue}
                on:keydown={handleKeydown}
                spellcheck="false"
                autocomplete="off"
            />
        </div>
    </div>
</div>

<style>
    .console-panel {
        display: flex;
        flex-direction: column;
        height: 100%;
        border-left: 1px solid var(--border);
        background: var(--bg-surface);
    }

    .console-body {
        flex: 1;
        display: flex;
        flex-direction: column;
        background: var(--bg-base);
        min-height: 0;
    }

    .console-output {
        flex: 1;
        overflow-y: auto;
        padding: 8px 12px;
        font-family: var(--font-mono);
        font-size: 13px;
        line-height: 1.5;
        white-space: pre-wrap;
        word-break: break-all;
        user-select: text;
        cursor: text;
    }

    .console-line {
        display: flex;
        gap: 6px;
        padding: 0 0 1px;
    }

    .cmd-prompt, .msg-icon {
        flex-shrink: 0;
        user-select: none;
    }

    .msg-icon {
        width: 14px;
        text-align: center;
    }

    .output-text, .msg-text, .cmd-text {
        flex: 1;
        min-width: 0;
        user-select: text;
    }

    .console-input-row {
        display: flex;
        align-items: center;
        padding: 6px 12px;
        border-top: 1px solid var(--border);
        background: var(--bg-surface);
        gap: 6px;
    }

    .input-prompt {
        font-family: var(--font-mono);
        font-size: 13px;
        flex-shrink: 0;
        user-select: none;
    }

    .console-input {
        flex: 1;
        background: transparent;
        border: none;
        outline: none;
        color: var(--text-bright);
        font-family: var(--font-mono);
        font-size: 13px;
        caret-color: var(--accent);
    }
</style>
