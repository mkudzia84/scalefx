<!-- FaqView — the non-LLM FAQ tab.  Deterministic, offline, no API key: it
     renders the curated Q&A parsed from the embedded textbook (40-faq.md).
     Searchable accordion; answers render through the shared markdown renderer. -->
<script lang="ts">
    import { onMount } from 'svelte'
    import { AssistantFAQ } from '../../../wailsjs/go/main/App'
    import { renderMarkdown } from './markdown'

    interface FaqItem { question: string; answer: string }
    let items: FaqItem[] = []
    let query = ''
    let openIdx = -1
    let loaded = false

    onMount(async () => {
        try { items = (await AssistantFAQ()) as FaqItem[] } catch { items = [] }
        loaded = true
    })

    $: searching = query.trim().length > 0
    $: filtered = filterItems(items, query)
    function filterItems(list: FaqItem[], q: string) {
        const t = q.trim().toLowerCase()
        return list
            .map((item, i) => ({ item, i }))
            .filter(({ item }) => !t || `${item.question} ${item.answer}`.toLowerCase().includes(t))
    }
    function toggle(i: number) { openIdx = openIdx === i ? -1 : i }
</script>

<div class="faq">
    <div class="faq-search">
        <input class="field-input" type="text" placeholder="Search the FAQ…" bind:value={query} />
    </div>

    <div class="faq-list">
        {#if loaded && filtered.length === 0}
            <p class="faq-empty">{items.length === 0 ? 'FAQ unavailable.' : 'No matching questions.'}</p>
        {/if}
        {#each (filtered) as f (f.i)}
            <div class="faq-item">
                <button class="faq-q" class:open={searching || openIdx === f.i} on:click={() => toggle(f.i)}>
                    <span class="faq-caret">{searching || openIdx === f.i ? '▾' : '▸'}</span>
                    <span class="faq-qtext">{f.item.question}</span>
                </button>
                {#if searching || openIdx === f.i}
                    <div class="faq-a">{@html renderMarkdown(f.item.answer)}</div>
                {/if}
            </div>
        {/each}
    </div>

    <div class="faq-foot">Curated answers from the assistant service — no AI request, not rate-limited.</div>
</div>

<style>
    .faq { display: flex; flex-direction: column; height: 100%; min-height: 0; background: var(--bg-surface); }
    .faq-search { padding: 8px 12px; border-bottom: 1px solid var(--border); background: var(--bg-raised); flex-shrink: 0; }
    .faq-search .field-input { width: 100%; box-sizing: border-box; }
    .faq-list { flex: 1; overflow-y: auto; padding: 6px 8px; min-height: 0; }
    .faq-empty { color: var(--text-dim); font-size: 12px; padding: 12px; }

    .faq-item { border-bottom: 1px solid var(--border); }
    .faq-q {
        width: 100%; text-align: left; background: none; border: none; cursor: pointer;
        display: flex; gap: 7px; align-items: flex-start;
        padding: 9px 8px; font-size: 12.5px; color: var(--text); line-height: 1.4;
    }
    .faq-q:hover { color: var(--text-bright); }
    .faq-q.open { color: var(--text-bright); font-weight: 600; }
    .faq-caret { color: var(--text-dim); flex-shrink: 0; font-size: 10px; margin-top: 2px; }
    .faq-qtext { flex: 1; }

    .faq-a { padding: 0 8px 12px 22px; font-size: 12.5px; line-height: 1.55; color: var(--text); }
    .faq-a :global(p) { margin: 0 0 8px; }
    .faq-a :global(p:last-child) { margin-bottom: 0; }
    .faq-a :global(ul), .faq-a :global(ol) { margin: 4px 0 8px; padding-left: 18px; }
    .faq-a :global(li) { margin: 2px 0; }
    .faq-a :global(strong) { font-weight: 700; color: var(--text-bright); }
    .faq-a :global(a) { color: var(--accent); text-decoration: underline; }
    .faq-a :global(code.md-code) {
        font-family: var(--font-mono); font-size: 11.5px; color: var(--success, #4ec9b0);
        background: color-mix(in srgb, var(--success, #4ec9b0) 12%, var(--bg-input)); padding: 1px 5px; border-radius: 3px;
    }
    .faq-a :global(blockquote.md-quote) {
        margin: 6px 0; padding: 5px 10px; border-left: 3px solid var(--error);
        background: color-mix(in srgb, var(--error) 10%, transparent); color: var(--error); border-radius: 0 4px 4px 0;
    }

    .faq-foot { padding: 6px 12px; border-top: 1px solid var(--border); background: var(--bg-raised);
        font-size: 10.5px; color: var(--text-dim); flex-shrink: 0; }
</style>
