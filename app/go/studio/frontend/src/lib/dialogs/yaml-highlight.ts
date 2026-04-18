// ScaleFX Studio — minimal YAML syntax highlighter.
// Tokenizes a single line into HTML <span class="tok-…"> chunks for the
// SaveConfigDialog YAML overlay. Not a full YAML parser — just enough to
// colorize keys, strings, numbers, booleans, null, comments, and list dashes.

const ESC: Record<string, string> = {
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
}

function esc(s: string): string {
    return s.replace(/[&<>"']/g, (c) => ESC[c])
}

function span(cls: string, text: string): string {
    if (!text) return ''
    return `<span class="tok-${cls}">${esc(text)}</span>`
}

// Highlight a single value (right-hand side of `key: value` or list item).
function highlightValue(v: string): string {
    if (v === '') return ''
    const trimmed = v.trim()
    if (trimmed === '') return esc(v)

    // Quoted string.
    if ((trimmed.startsWith('"') && trimmed.endsWith('"') && trimmed.length >= 2) ||
        (trimmed.startsWith("'") && trimmed.endsWith("'") && trimmed.length >= 2)) {
        const lead = v.slice(0, v.indexOf(trimmed[0]))
        const tail = v.slice(v.lastIndexOf(trimmed[trimmed.length - 1]) + 1)
        return esc(lead) + span('string', trimmed) + esc(tail)
    }

    // Boolean / null literals (YAML 1.1 + 1.2 common forms).
    if (/^(true|false|yes|no|on|off)$/i.test(trimmed)) {
        const lead = v.slice(0, v.indexOf(trimmed))
        const tail = v.slice(v.indexOf(trimmed) + trimmed.length)
        return esc(lead) + span('bool', trimmed) + esc(tail)
    }
    if (/^(null|~)$/i.test(trimmed)) {
        const lead = v.slice(0, v.indexOf(trimmed))
        const tail = v.slice(v.indexOf(trimmed) + trimmed.length)
        return esc(lead) + span('null', trimmed) + esc(tail)
    }

    // Number (int, float, hex).
    if (/^-?(\d+\.?\d*|\.\d+|0x[0-9a-fA-F]+)$/.test(trimmed)) {
        const lead = v.slice(0, v.indexOf(trimmed))
        const tail = v.slice(v.indexOf(trimmed) + trimmed.length)
        return esc(lead) + span('number', trimmed) + esc(tail)
    }

    // Flow sequence / mapping fall-through — leave plain but escape.
    return esc(v)
}

// Highlight one line. Order matters: comments first, then list dash, then key.
function highlightLine(line: string): string {
    if (line === '') return ''

    // Pull off leading whitespace verbatim.
    const indentMatch = line.match(/^(\s*)/)
    const indent = indentMatch ? indentMatch[1] : ''
    const rest = line.slice(indent.length)
    if (rest === '') return esc(indent)

    // Comment-only line.
    if (rest.startsWith('#')) {
        return esc(indent) + span('comment', rest)
    }

    // List-item dash: "- " (possibly followed by inline key/value or bare scalar).
    if (rest.startsWith('- ') || rest === '-') {
        const dash = rest === '-' ? '-' : '- '
        const after = rest.slice(dash.length)
        return esc(indent) + span('dash', dash.trimEnd()) + (dash.endsWith(' ') ? ' ' : '') + highlightLine(after)
    }

    // Strip trailing inline comment so it can be colored separately.
    let body = rest
    let trailingComment = ''
    // Find an unquoted '#' that begins a comment. Walk the string.
    {
        let inSingle = false
        let inDouble = false
        for (let i = 0; i < body.length; i++) {
            const c = body[i]
            if (c === "'" && !inDouble) inSingle = !inSingle
            else if (c === '"' && !inSingle) inDouble = !inDouble
            else if (c === '#' && !inSingle && !inDouble && (i === 0 || /\s/.test(body[i - 1]))) {
                trailingComment = body.slice(i)
                body = body.slice(0, i)
                break
            }
        }
    }

    // key: value (where value may be empty for nested mappings).
    const kv = body.match(/^([^:\s][^:]*?)(\s*:\s*)(.*)$/)
    if (kv) {
        const [, key, sep, val] = kv
        const valueHtml = val === '' ? '' : highlightValue(val.replace(/\s+$/, '')) +
            esc(val.match(/\s+$/)?.[0] ?? '')
        const commentHtml = trailingComment ? span('comment', trailingComment) : ''
        return esc(indent) + span('key', key) + esc(sep) + valueHtml + commentHtml
    }

    // Bare scalar line (no key).
    return esc(indent) + highlightValue(body) + (trailingComment ? span('comment', trailingComment) : '')
}

/** Return YAML text as HTML with token spans. Preserves newlines. */
export function highlightYaml(text: string): string {
    if (text === '') return ''
    return text.split('\n').map(highlightLine).join('\n')
}
