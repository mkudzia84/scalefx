// console_colour.ts — colourise a plain diag/log line into c-* span markup
// for the Studio console, matching the CLI palette (term.go). CLI command
// output already arrives as ANSI→span HTML; this handles the diag/log STREAM
// ([LANDING]/[GUNFX]/[servo]/[LOG I] …) which arrives as plain escaped text.
//
// Single-pass alternation so a token is never double-wrapped. Extracted +
// unit-tested (console_colour.test.ts).

export function tagColour(tag: string): string {
    const t = tag.toUpperCase()
    if (t.includes('LOG E') || t.includes('ERR') || t.includes('FAIL')) return 'c-red'
    if (t.includes('LOG W') || t.includes('WARN')) return 'c-yellow'
    if (t.includes('GUN')) return 'c-red'
    if (t.includes('LL') || t.includes('LANDING')) return 'c-blue'
    if (t.includes('ENG')) return 'c-green'
    if (t.includes('GAP')) return 'c-yellow'
    return 'c-cyan'
}

const PHASE_GREEN = /^(on|running|active|armed|firing|deployed|ready|ok|enabled|connected|true)$/i
const PHASE_YELLOW = /^(starting|stopping|deploying|retracting|warning|pending|busy)$/i
const PHASE_RED = /^(error|critical|fault|failed|disabled|collision)$/i
const PHASE_DIM = /^(off|stopped|retracted|idle|unconfigured|none|false|null)$/i

export function phaseColour(w: string): string {
    if (PHASE_RED.test(w)) return 'c-red'
    if (PHASE_GREEN.test(w)) return 'c-green'
    if (PHASE_YELLOW.test(w)) return 'c-yellow'
    if (PHASE_DIM.test(w)) return 'c-dim'
    return ''
}

export function colourize(text: string): string {
    return text.replace(
        // 1 tag  2 timestamp  3 hex  4 key=  5 value  6 phase-word
        /(\[[^\]<>]+\])|(@\d+ms\b)|(0x[0-9A-Fa-f]+)|([A-Za-z_][\w.]*=)("?-?\d[\w.]*"?|[0-9A-Fa-f]{4}\b)|([A-Za-z_]+)/g,
        (m, tag, ts, hex, key, val, word) => {
            if (tag)  return `<span class="${tagColour(tag)}">${tag}</span>`
            if (ts)   return `<span class="c-dim">${ts}</span>`
            if (hex)  return `<span class="c-magenta">${hex}</span>`
            if (key)  return `${key}<span class="c-cyan">${val}</span>`
            if (word) { const c = phaseColour(word); return c ? `<span class="${c}">${word}</span>` : word }
            return m
        },
    )
}
