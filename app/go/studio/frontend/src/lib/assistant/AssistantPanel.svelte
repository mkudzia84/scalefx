<!-- AssistantPanel — the Assistant dock pane (chat).  Advisory: it answers and
     points to the Wizard; it does not change config.  Mirrors the Console pane
     docking. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { messages, busy, status, ask, refreshStatus, setKey, clearChat } from './store'
    import { renderMarkdown } from './markdown'
    import { ListAssistantModels, SetAssistantModel } from '../../../wailsjs/go/main/App'

    let input = ''
    let keyInput = ''
    let listEl: HTMLElement

    // Model dropdown — every provider is LIMITED to a sensible short-list of
    // models for config-assistant chat; availableIds flags which the key can
    // actually use.  A provider with no entry would fall back to its live list.
    const ALLOWLISTS: Record<string, string[]> = {
        gemini: ['gemini-2.5-flash', 'gemini-3.5-flash'],
        groq: ['llama-3.3-70b-versatile', 'llama-3.1-8b-instant'],
        mistral: ['mistral-small-latest', 'mistral-medium-latest', 'mistral-large-latest'],
    }
    const allowFor = (prov: string): string[] | null => ALLOWLISTS[prov] || null
    let liveModels: { id: string; displayName: string }[] = []
    let availableIds: string[] = []
    let selModel = ''
    let modelsLoading = false
    let lastProvider = ''
    function activeProvider(): string { return ($status && $status.provider) || 'gemini' }
    function provLabel(s: any): string {
        if (s && s.providers) { const p = s.providers.find((x: any) => x.id === s.provider); if (p) return p.label }
        return s && s.provider === 'groq' ? 'Groq' : s && s.provider === 'mistral' ? 'Mistral' : 'Gemini'
    }

    const SUGGESTIONS = [
        'How do I set up retractable gear?',
        'What channel should I use for the guns?',
        'Why is my engine not starting?',
        'Which port do I use for a smoke heater?',
    ]

    onMount(refreshStatus)

    // Load the active provider's models when the provider/key first appears
    // or the provider changes (e.g. switched in Settings).
    $: maybeReload(($status && $status.provider) || '', !!($status && $status.available))
    function maybeReload(prov: string, avail: boolean) {
        if (prov && avail && prov !== lastProvider) { lastProvider = prov; loadModels() }
    }

    async function loadModels() {
        const prov = activeProvider()
        if (!($status && $status.available)) { liveModels = []; availableIds = []; return }
        modelsLoading = true
        try { liveModels = await ListAssistantModels() } catch { liveModels = [] } finally { modelsLoading = false }
        const liveIds = liveModels.map(m => m.id)
        const allow = allowFor(prov)
        const allowed = allow || liveIds
        availableIds = allow ? allow.filter(id => liveIds.includes(id)) : liveIds
        const cur = ($status && $status.model) || ''
        if (cur && allowed.includes(cur)) {
            selModel = cur
        } else {
            selModel = availableIds[0] || allowed[0] || cur
            if (selModel && selModel !== cur) { await SetAssistantModel(selModel); await refreshStatus() }
        }
    }
    async function onModelChange() {
        await SetAssistantModel(selModel)
        await refreshStatus()
    }
    // Gemini: exactly the two allowed (mark unavailable).  Groq: the live list.
    $: modelOptions = buildModelOptions(($status && $status.provider) || 'gemini', liveModels, availableIds, ($status && $status.model) || '')
    function buildModelOptions(prov: string, live: { id: string; displayName: string }[], avail: string[], cur: string) {
        const allow = allowFor(prov)
        if (allow) {
            return allow.map(id => ({ id, label: (avail.length && !avail.includes(id)) ? `${id} (unavailable)` : id }))
        }
        let opts = live.map(m => ({ id: m.id, label: m.id }))
        if (cur && !opts.some(o => o.id === cur)) opts = [{ id: cur, label: cur }, ...opts]
        return opts
    }

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
    async function saveKey() {
        const k = keyInput.trim()
        if (!k) return
        keyInput = ''
        await setKey(activeProvider(), k)
        await loadModels()
    }
</script>

<div class="asst">
    <div class="asst-head">
        <span class="asst-title">🤖 Assistant</span>
        {#if $status && $status.available}
            <span class="asst-prov">{provLabel($status)}</span>
            <select class="asst-modelsel" bind:value={selModel} on:change={onModelChange}
                    disabled={modelsLoading} title="Model — saved automatically">
                {#each (modelOptions) as o}<option value={o.id}>{o.label}</option>{/each}
            </select>
        {:else if $status}
            <span class="asst-model">no key</span>
        {/if}
        <button class="small clear" on:click={clearChat} disabled={$busy || $messages.length === 0}
                title="Clear the conversation">Clear</button>
    </div>

    {#if $status && !$status.available}
        <div class="asst-nokey">
            <p class="nokey-title">⚙ Set up the AI assistant</p>
            <p>It needs a free API key. Choose a provider in <strong>View → Settings…</strong>
               (currently <strong>{provLabel($status)}</strong>), then get a key:</p>
            <ol class="nokey-steps">
                <li><strong>Gemini</strong> — open <code>aistudio.google.com/apikey</code>, sign in, click <em>Create API key</em>, copy it.</li>
                <li><strong>Groq</strong> — open <code>console.groq.com/keys</code>, sign in, create a key, copy it.</li>
                <li><strong>Mistral</strong> — open <code>console.mistral.ai</code>, sign in, create an API key, copy it.</li>
                <li>Paste it below (or in Settings). It's stored locally on this machine and never uploaded.</li>
            </ol>
            <div class="asst-keyrow">
                <input class="field-input" type="password"
                       placeholder={`Paste ${provLabel($status)} API key`}
                       bind:value={keyInput} on:keydown={(e) => { if (e.key === 'Enter') saveKey() }} />
                <button class="small primary" on:click={saveKey} disabled={!keyInput.trim()}>Save</button>
            </div>
        </div>
    {/if}

    <div class="asst-list" bind:this={listEl}>
        {#if $messages.length === 0 && $status && $status.available}
            <div class="asst-empty">
                <p>Ask about configuring your model — features, channels, ports, or why something isn't working. I advise and point you to the <strong>🪄 Setup Wizard</strong>; I don't change settings myself.</p>
                <div class="asst-sugg">
                    {#each (SUGGESTIONS) as s}
                        <button class="asst-chip" on:click={() => pick(s)} disabled={$busy}>{s}</button>
                    {/each}
                </div>
            </div>
        {/if}
        {#each ($messages) as m}
            <div class="asst-msg {m.role}" class:err={m.error}>
                {#if m.role === 'model' && !m.error}
                    <div class="asst-bubble md">{@html renderMarkdown(m.content)}</div>
                {:else}
                    <div class="asst-bubble">{m.content}</div>
                {/if}
            </div>
        {/each}
        {#if $busy}
            <div class="asst-msg model"><div class="asst-bubble thinking">● ● ●</div></div>
        {/if}
    </div>

    <div class="asst-input">
        <textarea rows="2" placeholder="Ask a question…  (Enter to send, Shift+Enter for newline)"
                  bind:value={input} on:keydown={onKey} disabled={$busy}></textarea>
        <button class="small primary send" on:click={send} disabled={$busy || !input.trim()}>Send</button>
    </div>
</div>

<style>
    .asst { display: flex; flex-direction: column; height: 100%; min-height: 0; background: var(--bg-surface); }
    .asst-head {
        display: flex; align-items: center; gap: 10px;
        padding: 8px 12px; border-bottom: 1px solid var(--border); background: var(--bg-raised);
        flex-shrink: 0;
    }
    .asst-title { font-size: 13px; font-weight: 700; color: var(--text-bright); }
    .asst-model { font-size: 10px; font-family: var(--font-mono); color: var(--text-dim);
        border: 1px solid var(--border); border-radius: 4px; padding: 1px 6px; }
    .asst-prov {
        font-size: 10px; font-weight: 600; color: var(--accent);
        text-transform: uppercase; letter-spacing: 0.3px;
    }
    .asst-modelsel {
        font-size: 10px; font-family: var(--font-mono);
        background: var(--bg-input); color: var(--text);
        border: 1px solid var(--border); border-radius: 4px; padding: 2px 4px;
        max-width: 190px;
    }
    .asst-head .clear { margin-left: auto; }

    .asst-nokey { padding: 12px; border-bottom: 1px solid var(--border); background: var(--bg-raised); }
    .asst-nokey p { margin: 0 0 8px; font-size: 11.5px; color: var(--text-dim); line-height: 1.5; }
    .nokey-title { font-size: 13px !important; font-weight: 700; color: var(--text-bright) !important; }
    .asst-nokey :global(strong) { color: var(--text-bright); }
    .asst-nokey :global(code) { font-family: var(--font-mono); font-size: 11px; color: var(--accent);
        background: var(--bg-input); padding: 1px 4px; border-radius: 3px; }
    .nokey-steps { margin: 0 0 10px; padding-left: 18px; font-size: 11.5px; color: var(--text-dim); line-height: 1.6; }
    .nokey-steps li { margin: 2px 0; }
    .asst-keyrow { display: flex; gap: 6px; }
    .asst-keyrow .field-input { flex: 1; }

    .asst-list { flex: 1; overflow-y: auto; padding: 12px; display: flex; flex-direction: column; gap: 10px; min-height: 0; }
    .asst-empty p { font-size: 12px; color: var(--text-dim); margin: 0 0 10px; line-height: 1.5; }
    .asst-sugg { display: flex; flex-direction: column; gap: 6px; }
    .asst-chip {
        text-align: left; font-size: 12px; padding: 7px 10px; cursor: pointer;
        background: var(--bg-raised); border: 1px solid var(--border); border-radius: 6px; color: var(--text);
    }
    .asst-chip:hover:not(:disabled) { border-color: var(--accent); color: var(--text-bright); }

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
