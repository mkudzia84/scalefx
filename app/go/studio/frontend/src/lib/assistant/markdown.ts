// markdown — a compact, dependency-free markdown->HTML renderer for assistant
// chat output.  Safe by construction: ALL source text is HTML-escaped first, so
// nothing the model emits (or a prompt-injection tries to emit) can inject live
// HTML/script; only a fixed set of tags is produced.  Supports: headings,
// bold/italic, inline code, fenced code blocks, links, blockquotes (red safety
// callouts), unordered/ordered lists, and GitHub-style tables.

function escapeHtml(s: string): string {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
}

// Inline formatting for ALREADY-block-split text: escape, protect inline code,
// then emphasis + links, then restore code.
function inlineMd(s: string): string {
    let t = escapeHtml(s)
    const codes: string[] = []
    t = t.replace(/`([^`]+)`/g, (_m, c) => {
        codes.push(c)
        return ` I${codes.length - 1} `
    })
    t = t
        .replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>')
        .replace(/\*([^*\n]+)\*/g, '<em>$1</em>')
        .replace(/(^|\s)_([^_\n]+)_(?=\s|$)/g, '$1<em>$2</em>')
        .replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>')
    t = t.replace(/ I(\d+) /g, (_m, i) => `<code class="md-code">${codes[Number(i)]}</code>`)
    return t
}

// ── Tables ──
function splitRow(line: string): string[] {
    let s = line.trim()
    if (s.startsWith('|')) s = s.slice(1)
    if (s.endsWith('|')) s = s.slice(0, -1)
    return s.split('|').map(c => c.trim())
}
function isTableSep(line: string): boolean {
    if (!line.includes('|')) return false
    const cells = splitRow(line)
    return cells.length > 0 && cells.every(c => /^:?-{1,}:?$/.test(c))
}
function parseAligns(sep: string): string[] {
    return splitRow(sep).map(c => {
        const l = c.startsWith(':'), r = c.endsWith(':')
        return l && r ? 'center' : r ? 'right' : l ? 'left' : ''
    })
}
function renderTable(header: string[], aligns: string[], rows: string[][]): string {
    const al = (i: number) => (aligns[i] ? ` style="text-align:${aligns[i]}"` : '')
    const th = header.map((c, i) => `<th${al(i)}>${inlineMd(c)}</th>`).join('')
    const body = rows.map(r => `<tr>${header.map((_c, i) => `<td${al(i)}>${inlineMd(r[i] ?? '')}</td>`).join('')}</tr>`).join('')
    return `<table class="md-table"><thead><tr>${th}</tr></thead><tbody>${body}</tbody></table>`
}

export function renderMarkdown(src: string): string {
    if (!src) return ''

    // 1. Pull fenced code blocks out so their contents are never parsed.
    const blocks: string[] = []
    let text = src.replace(/```[ \t]*[\w+.-]*\n?([\s\S]*?)```/g, (_m, code) => {
        blocks.push(String(code).replace(/\n+$/, ''))
        return `\n B${blocks.length - 1} \n`
    })

    // 2. Pull GitHub-style tables (header row, |---| separator, then rows).
    const tables: string[] = []
    text = text.replace(
        /(^[^\n]*\|[^\n]*)\n([^\n]*\|[^\n]*)\n((?:[^\n]*\|[^\n]*(?:\n|$))*)/gm,
        (m, header, sep, bodyText) => {
            if (!isTableSep(sep)) return m
            const rows = String(bodyText).split('\n').filter(r => r.trim() && r.includes('|')).map(splitRow)
            tables.push(renderTable(splitRow(header), parseAligns(sep), rows))
            return `\n T${tables.length - 1} \n`
        }
    )

    const out: string[] = []
    let list: 'ul' | 'ol' | null = null
    let para: string[] = []
    let quote: string[] = []

    const flushPara = () => {
        if (para.length) { out.push(`<p>${para.map(inlineMd).join('<br>')}</p>`); para = [] }
    }
    const flushQuote = () => {
        if (quote.length) { out.push(`<blockquote class="md-quote">${quote.map(inlineMd).join('<br>')}</blockquote>`); quote = [] }
    }
    const closeList = () => { if (list) { out.push(`</${list}>`); list = null } }

    for (const line of text.split('\n')) {
        const bm = line.match(/^ B(\d+) $/)
        if (bm) {
            flushPara(); flushQuote(); closeList()
            out.push(`<pre class="md-pre"><code>${escapeHtml(blocks[Number(bm[1])])}</code></pre>`)
            continue
        }
        const tm = line.match(/^ T(\d+) $/)
        if (tm) {
            flushPara(); flushQuote(); closeList()
            out.push(tables[Number(tm[1])])
            continue
        }
        if (!line.trim()) { flushPara(); flushQuote(); closeList(); continue }

        // Blockquote — used for safety reminders, rendered as a red caution.
        const bq = line.match(/^>\s?(.*)$/)
        if (bq) { flushPara(); closeList(); quote.push(bq[1]); continue }

        const h = line.match(/^(#{1,6})\s+(.*)$/)
        if (h) {
            flushPara(); flushQuote(); closeList()
            const lvl = h[1].length
            out.push(`<h${lvl} class="md-h">${inlineMd(h[2])}</h${lvl}>`)
            continue
        }

        const ul = line.match(/^[ \t]*[-*+]\s+(.*)$/)
        if (ul) {
            flushPara(); flushQuote()
            if (list !== 'ul') { closeList(); out.push('<ul class="md-list">'); list = 'ul' }
            out.push(`<li>${inlineMd(ul[1])}</li>`)
            continue
        }
        const ol = line.match(/^[ \t]*\d+[.)]\s+(.*)$/)
        if (ol) {
            flushPara(); flushQuote()
            if (list !== 'ol') { closeList(); out.push('<ol class="md-list">'); list = 'ol' }
            out.push(`<li>${inlineMd(ol[1])}</li>`)
            continue
        }

        flushQuote(); closeList()
        para.push(line)
    }
    flushPara(); flushQuote(); closeList()
    return out.join('\n')
}
