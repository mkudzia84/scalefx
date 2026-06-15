<!-- AssistantPanel — the Assistant dock pane (chat).  Advisory: it answers and
     points to the Wizard; it does not change config.  A thin client to the
     standalone AI-assistant service (services/ai-assistant): the service owns the
     provider tokens, textbook, model routing, and rate limiting. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { messages, busy, compacting, status, model, ask, refreshStatus, clearChat } from './store'
    import { renderMarkdown } from './markdown'
    import FaqView from './FaqView.svelte'

    let mode: 'chat' | 'faq' = 'chat'
    let input = ''
    let listEl: HTMLElement

    // Monochrome SVG icons (currentColor), matching the TabBar convention.
    const ICONS: Record<string, string> = {
        chat: '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/>',
        faq: '<circle cx="12" cy="12" r="10"/><path d="M9.1 9a3 3 0 0 1 5.82 1c0 2-3 3-3 3"/><line x1="12" y1="17" x2="12.01" y2="17"/>',
    }
    function icon(name: string): string {
        return `<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${ICONS[name] ?? ''}</svg>`
    }

    const SUGGESTIONS = [
        'How do I set up retractable gear?',
        'What channel should I use for the guns?',
        'Why is my engine not starting?',
        'Which port do I use for a smoke heater?',
    ]

    onMount(refreshStatus)

    // Model UI: >1 model -> dropdown (bound to $model); ==1 -> static label; 0 -> nothing.
    $: models = ($status && $status.models) || []
    $: oneModelLabel = models.length === 1 ? (models[0].label || models[0].id) : ''

    // Auto-scroll to the newest message / the thinking indicator.
    $: scheduleScroll($messages.length, $busy)
    function scheduleScroll(_n: number, _b: boolean) {
        if (listEl) requestAnimationFrame(() => { listEl.scrollTop = listEl.scrollHeight })
    }

    async function send() {
        const t = input.trim()
        if (!t) return
        input = ''
        await ask(t)
    }
    function onKey(e: KeyboardEvent) {
        if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send() }
    }
    function pick(s: string) { input = s }
</script>

<div class="asst">
    <div class="asst-head">
        <div class="asst-modeseg">
            <button class="seg" class:on={mode === 'chat'} on:click={() => (mode = 'chat')}><span class="seg-ico">{@html icon('chat')}</span>Assistant</button>
            <button class="seg" class:on={mode === 'faq'} on:click={() => (mode = 'faq')}><span class="seg-ico">{@html icon('faq')}</span>FAQ</button>
        </div>
        {#if mode === 'chat' && $status && $status.available}
            {#if models.length > 1}
                <select class="asst-modelsel" bind:value={$model} title="Model">
                    {#each models as m}<option value={m.id}>{m.label || m.id}</option>{/each}
                </select>
            {:else if models.length === 1}
                <span class="asst-model" title="The only model the assistant service offers">{oneModelLabel}</span>
            {/if}
        {/if}
        {#if mode === 'chat'}
            <button class="small clear" on:click={clearChat} disabled={$busy || $messages.length === 0}
                    title="Clear the conversation">Clear</button>
        {/if}
    </div>

  {#if mode === 'faq'}
    <FaqView />
  {:else}

    {#if $status && !$status.available}
        <div class="asst-down">
            <p class="down-title">Assistant service unreachable</p>
            <p>Studio couldn't reach the AI-assistant service{#if $status.endpoint} at <code>{$status.endpoint}</code>{/if}.
               Start the service (<code>services/ai-assistant</code>) and try again.</p>
            {#if $status.error}<p class="down-err">{$status.error}</p>{/if}
            <button class="small" on:click={refreshStatus}>Retry</button>
        </div>
    {/if}

    <div class="asst-list" bind:this={listEl}>
        {#if $messages.length === 0 && $status && $status.available}
            <div class="asst-empty">
                <p>Ask about configuring your model — features, channels, ports, or why something isn't working. I advise and point you to the <strong>Setup Wizard</strong>; I don't change settings myself.</p>
                <div class="asst-sugg">
                    {#each (SUGGESTIONS) as s}
                        <button class="asst-chip" on:click={() => pick(s)} disabled={$busy}>{s}</button>
                    {/each}
                </div>
            </div>
        {/if}
        {#each ($messages) as m}
            {#if m.summary}
                <div class="asst-compacted" title={m.content}>🗜️ Earlier messages summarized to keep the chat fast</div>
            {:else}
                <div class="asst-msg {m.role}" class:err={m.error}>
                    {#if m.role === 'model' && !m.error}
                        <div class="asst-bubble md">{@html renderMarkdown(m.content)}</div>
                    {:else}
                        <div class="asst-bubble">{m.content}</div>
                    {/if}
                </div>
            {/if}
        {/each}
        {#if $busy}
            <div class="asst-msg model"><div class="asst-bubble thinking">● ● ●</div></div>
        {/if}
        {#if $compacting}
            <div class="asst-compacted">🗜️ Summarizing earlier messages…</div>
        {/if}
    </div>

    <div class="asst-input">
        <textarea rows="2" placeholder="Ask a question…  (Enter to send, Shift+Enter for newline)"
                  bind:value={input} on:keydown={onKey} disabled={$busy || !($status && $status.available)}></textarea>
        <button class="small primary send" on:click={send} disabled={$busy || !input.trim() || !($status && $status.available)}>Send</button>
    </div>
  {/if}
</div>

<style>
    .asst { display: flex; flex-direction: column; height: 100%; min-height: 0; background: var(--bg-surface); }
    .asst-head {
        display: flex; align-items: center; gap: 10px;
        padding: 8px 12px; border-bottom: 1px solid var(--border); background: var(--bg-raised);
        flex-shrink: 0;
    }
    .asst-modeseg { display: inline-flex; gap: 2px; background: var(--bg-input); border-radius: 5px; padding: 2px; }
    .asst-modeseg .seg {
        display: inline-flex; align-items: center; gap: 5px;
        font-size: 11px; padding: 3px 9px; border: none; background: none; color: var(--text-dim);
        cursor: pointer; border-radius: 4px;
    }
    .asst-modeseg .seg-ico { display: flex; }
    .asst-modeseg .seg:hover { color: var(--text); }
    .asst-modeseg .seg.on { background: var(--bg-surface); color: var(--text-bright); font-weight: 600; }
    .asst-model { font-size: 10px; font-family: var(--font-mono); color: var(--text-dim);
        border: 1px solid var(--border); border-radius: 4px; padding: 1px 6px; }
    .asst-modelsel {
        font-size: 10px; font-family: var(--font-mono);
        background: var(--bg-input); color: var(--text);
        border: 1px solid var(--border); border-radius: 4px; padding: 2px 4px;
        max-width: 200px;
    }
    .asst-head .clear { margin-left: auto; }

    .asst-down { padding: 12px; border-bottom: 1px solid var(--border); background: var(--bg-raised); }
    .asst-down p { margin: 0 0 8px; font-size: 11.5px; color: var(--text-dim); line-height: 1.5; }
    .down-title { font-size: 13px !important; font-weight: 700; color: var(--text-bright) !important; }
    .down-err { font-family: var(--font-mono); font-size: 10.5px; color: var(--error); }
    .asst-down :global(code) { font-family: var(--font-mono); font-size: 11px; color: var(--accent);
        background: var(--bg-input); padding: 1px 4px; border-radius: 3px; }

    .asst-list { flex: 1; overflow-y: auto; padding: 12px; display: flex; flex-direction: column; gap: 10px; min-height: 0; }
    .asst-empty p { font-size: 12px; color: var(--text-dim); margin: 0 0 10px; line-height: 1.5; }
    .asst-sugg { display: flex; flex-direction: column; gap: 6px; }
    .asst-chip {
        text-align: left; font-size: 12px; padding: 7px 10px; cursor: pointer;
        background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; color: var(--text);
    }
    .asst-chip:hover:not(:disabled) { border-color: var(--accent); color: var(--text-bright); }

    .asst-compacted {
        align-self: center; max-width: 90%; text-align: center;
        font-size: 10.5px; color: var(--text-dim); cursor: default;
        padding: 2px 8px; border: 1px dashed var(--border); border-radius: 10px;
    }
    .asst-msg { display: flex; }
    .asst-msg.user { justify-content: flex-end; }
    .asst-msg.model { justify-content: flex-start; }
    .asst-bubble {
        max-width: 88%; padding: 8px 11px; border-radius: 10px; font-size: 12.5px; line-height: 1.5;
        white-space: pre-wrap; word-break: break-word;
    }
    .asst-msg.user .asst-bubble { background: color-mix(in srgb, var(--accent) 22%, var(--bg-input)); color: var(--text-bright); border-bottom-right-radius: 3px; }
    .asst-msg.model .asst-bubble { background: var(--bg-raised); border: 1px solid var(--border); border-bottom-left-radius: 3px; }

    /* Rendered markdown ({@html}) — not compile-scoped, so target via :global. */
    .asst-bubble.md { white-space: normal; }
    .asst-bubble.md :global(p) { margin: 0 0 8px; }
    .asst-bubble.md :global(p:last-child) { margin-bottom: 0; }
    .asst-bubble.md :global(h1), .asst-bubble.md :global(h2), .asst-bubble.md :global(h3),
    .asst-bubble.md :global(h4), .asst-bubble.md :global(h5), .asst-bubble.md :global(h6) {
        font-size: 13px; font-weight: 700; color: var(--text-bright); margin: 8px 0 4px; line-height: 1.3;
    }
    .asst-bubble.md :global(ul), .asst-bubble.md :global(ol) { margin: 4px 0 8px; padding-left: 18px; }
    .asst-bubble.md :global(li) { margin: 2px 0; }
    /* Colour scheme: ports / fields / values (inline code) = TEAL (reads as an
       identifier, not an alert); headers + bold labels stay PLAIN BOLD (no
       hue); safety reminders (blockquotes) = RED. */
    .asst-bubble.md :global(strong) { font-weight: 700; color: var(--text-bright); }
    .asst-bubble.md :global(a) { color: var(--accent); text-decoration: underline; }
    .asst-bubble.md :global(code.md-code) {
        font-family: var(--font-mono); font-size: 11.5px;
        color: var(--success, #4ec9b0);
        background: color-mix(in srgb, var(--success, #4ec9b0) 12%, var(--bg-input));
        padding: 1px 5px; border-radius: 3px;
    }
    .asst-bubble.md :global(blockquote.md-quote) {
        margin: 6px 0; padding: 5px 10px;
        border-left: 3px solid var(--error);
        background: color-mix(in srgb, var(--error) 10%, transparent);
        color: var(--error); border-radius: 0 4px 4px 0;
    }
    .asst-bubble.md :global(table.md-table) {
        border-collapse: collapse; margin: 8px 0; font-size: 11.5px; max-width: 100%;
    }
    .asst-bubble.md :global(table.md-table th),
    .asst-bubble.md :global(table.md-table td) {
        border: 1px solid var(--border); padding: 4px 8px; text-align: left; vertical-align: top;
    }
    .asst-bubble.md :global(table.md-table th) {
        background: var(--bg-input); color: var(--text-bright); font-weight: 700;
    }
    .asst-bubble.md :global(table.md-table tbody tr:nth-child(even)) {
        background: color-mix(in srgb, var(--bg-input) 40%, transparent);
    }
    .asst-bubble.md :global(pre.md-pre) {
        background: var(--bg-input); border: 1px solid var(--border); border-radius: 6px;
        padding: 8px 10px; margin: 6px 0; overflow-x: auto;
    }
    .asst-bubble.md :global(pre.md-pre code) {
        font-family: var(--font-mono); font-size: 11.5px; white-space: pre; background: none; padding: 0;
    }
    .asst-msg.model .asst-bubble.err { border-color: var(--error); color: var(--error); }
    .asst-bubble.thinking { color: var(--text-dim); letter-spacing: 2px; animation: asst-pulse 1.1s ease-in-out infinite; }
    @keyframes asst-pulse { 50% { opacity: 0.4; } }

    .asst-input { display: flex; gap: 6px; padding: 10px 12px; border-top: 1px solid var(--border); background: var(--bg-raised); flex-shrink: 0; }
    .asst-input textarea {
        flex: 1; resize: none; font-family: inherit; font-size: 12.5px; line-height: 1.4;
        background: var(--bg-input); color: var(--text); border: 1px solid var(--border); border-radius: 6px; padding: 6px 8px;
    }
    .asst-input textarea:focus { outline: none; border-color: var(--accent); }
    .asst-input .send { align-self: stretch; }
</style>
