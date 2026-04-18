// ScaleFX Studio — Minimal YAML parser
//
// Supports the subset of YAML emitted by the firmware's declarative schema
// (`controllers/lib/sfx_config/yaml_schema.h`):
//
//   • Block mappings:   key: value (2-space indentation)
//   • Block sequences:  - item / - key: value lists
//   • Scalars:          strings (bare or double-quoted), ints, hex (0x..), bools
//   • Flow sequences:   [a, b, c] (numbers / bare strings)
//   • Comments:         # ... (whole line or trailing)
//   • Blank lines       ignored
//
// Unsupported (and not needed for board configs): anchors, tags, multi-line
// folded/literal scalars, flow mappings, complex keys.
//
// The parser returns a plain JS value tree where:
//   mapping  -> { [key]: value }
//   sequence -> [value, ...]
//   scalar   -> string | number | boolean | null

export type YamlValue =
    | string
    | number
    | boolean
    | null
    | YamlValue[]
    | { [key: string]: YamlValue }

export interface YamlParseError {
    line: number
    message: string
}

// ─── Tokenizer ───

interface Line {
    indent: number
    text: string   // content with indent + comments stripped
    raw: string    // original
    lineNo: number // 1-based
}

function tokenize(input: string): Line[] {
    const out: Line[] = []
    const rawLines = input.split(/\r?\n/)
    for (let i = 0; i < rawLines.length; i++) {
        const raw = rawLines[i]
        // strip trailing comment that isn't inside quotes
        const stripped = stripComment(raw)
        const trimmed = stripped.replace(/\s+$/, '')
        if (trimmed.trim() === '') continue
        const indent = countIndent(trimmed)
        out.push({ indent, text: trimmed.slice(indent), raw, lineNo: i + 1 })
    }
    return out
}

function stripComment(s: string): string {
    let inDq = false
    for (let i = 0; i < s.length; i++) {
        const c = s[i]
        if (c === '"') inDq = !inDq
        else if (c === '#' && !inDq && (i === 0 || /\s/.test(s[i - 1]))) {
            return s.slice(0, i)
        }
    }
    return s
}

function countIndent(s: string): number {
    let n = 0
    while (n < s.length && s[n] === ' ') n++
    return n
}

// ─── Parser ───

export function parseYaml(input: string): { value: YamlValue; errors: YamlParseError[] } {
    const errors: YamlParseError[] = []
    const lines = tokenize(input)

    if (lines.length === 0) return { value: {}, errors }

    const [value] = parseBlock(lines, 0, lines[0].indent, errors)
    return { value: value ?? {}, errors }
}

// Returns [parsedValue, nextIndex]. baseIndent is the indent at which the
// current block starts; lines with indent < baseIndent terminate the block.
function parseBlock(
    lines: Line[], start: number, baseIndent: number, errors: YamlParseError[],
): [YamlValue, number] {
    if (start >= lines.length) return [null, start]
    const first = lines[start]
    if (first.indent < baseIndent) return [null, start]

    if (first.text.startsWith('- ') || first.text === '-') {
        return parseSequence(lines, start, baseIndent, errors)
    }
    return parseMapping(lines, start, baseIndent, errors)
}

function parseMapping(
    lines: Line[], start: number, baseIndent: number, errors: YamlParseError[],
): [Record<string, YamlValue>, number] {
    const out: Record<string, YamlValue> = {}
    let i = start
    while (i < lines.length) {
        const line = lines[i]
        if (line.indent < baseIndent) break
        if (line.indent > baseIndent) {
            errors.push({ line: line.lineNo, message: `unexpected indent at "${line.text}"` })
            i++
            continue
        }
        if (line.text.startsWith('- ') || line.text === '-') break

        const colonIdx = findColon(line.text)
        if (colonIdx < 0) {
            errors.push({ line: line.lineNo, message: `expected "key: value" at "${line.text}"` })
            i++
            continue
        }
        const key = unquote(line.text.slice(0, colonIdx).trim())
        const rest = line.text.slice(colonIdx + 1).trim()
        i++

        if (rest === '' || rest === '|' || rest === '>') {
            // nested block
            const [child, next] = parseNested(lines, i, baseIndent, errors)
            out[key] = child
            i = next
        } else {
            out[key] = parseScalarOrFlow(rest, line.lineNo, errors)
        }
    }
    return [out, i]
}

function parseSequence(
    lines: Line[], start: number, baseIndent: number, errors: YamlParseError[],
): [YamlValue[], number] {
    const out: YamlValue[] = []
    let i = start
    while (i < lines.length) {
        const line = lines[i]
        if (line.indent < baseIndent) break
        if (line.indent > baseIndent) {
            errors.push({ line: line.lineNo, message: `unexpected indent in sequence at "${line.text}"` })
            i++
            continue
        }
        if (!(line.text.startsWith('- ') || line.text === '-')) break

        const afterDash = line.text === '-' ? '' : line.text.slice(2)

        if (afterDash === '') {
            // "- " then nested content
            i++
            const [child, next] = parseNested(lines, i, baseIndent, errors)
            out.push(child)
            i = next
        } else {
            const colonIdx = findColon(afterDash)
            if (colonIdx < 0) {
                // Simple scalar sequence item
                out.push(parseScalarOrFlow(afterDash, line.lineNo, errors))
                i++
            } else {
                // "- key: value" — start a mapping block. The mapping's
                // baseIndent is at the start of `key`, which is baseIndent + 2.
                const mapIndent = baseIndent + 2
                // Re-inject: replace current line as if it were at mapIndent.
                const synthetic: Line = {
                    indent: mapIndent, text: afterDash,
                    raw: line.raw, lineNo: line.lineNo,
                }
                const synthLines = [...lines]
                synthLines[i] = synthetic
                const [child, next] = parseMapping(synthLines, i, mapIndent, errors)
                out.push(child)
                i = next
            }
        }
    }
    return [out, i]
}

// Parses nested content. Normal form is indent > baseIndent. YAML also allows
// "compact" notation where a sequence nested under a mapping key sits at the
// SAME indent as the key — PyYAML, ruamel and the firmware's yaml_parser all
// accept this, and the firmware's yaml_schema.h emits it that way, so the
// parser has to as well. Only sequences qualify: a same-indent mapping line
// is always a sibling, never a child.
function parseNested(
    lines: Line[], start: number, baseIndent: number, errors: YamlParseError[],
): [YamlValue, number] {
    if (start >= lines.length) return ['', start]
    const next = lines[start]
    if (next.indent > baseIndent) return parseBlock(lines, start, next.indent, errors)
    if (next.indent === baseIndent &&
        (next.text.startsWith('- ') || next.text === '-')) {
        return parseSequence(lines, start, baseIndent, errors)
    }
    return ['', start]
}

// Find colon that terminates the key, ignoring ones inside quotes / brackets.
function findColon(s: string): number {
    let inDq = false, depth = 0
    for (let i = 0; i < s.length; i++) {
        const c = s[i]
        if (c === '"') inDq = !inDq
        else if (!inDq) {
            if (c === '[' || c === '{') depth++
            else if (c === ']' || c === '}') depth--
            else if (c === ':' && depth === 0) {
                if (i + 1 >= s.length || s[i + 1] === ' ' || s[i + 1] === '\t') return i
            }
        }
    }
    return -1
}

function unquote(s: string): string {
    if (s.length >= 2 && s[0] === '"' && s[s.length - 1] === '"') {
        return s.slice(1, -1).replace(/\\"/g, '"').replace(/\\\\/g, '\\')
    }
    return s
}

function parseScalarOrFlow(s: string, lineNo: number, errors: YamlParseError[]): YamlValue {
    const t = s.trim()
    if (t.startsWith('[') && t.endsWith(']')) {
        return parseFlowSeq(t, lineNo, errors)
    }
    return parseScalar(t)
}

function parseFlowSeq(s: string, lineNo: number, errors: YamlParseError[]): YamlValue[] {
    const inner = s.slice(1, -1).trim()
    if (inner === '') return []
    // simple comma split — we only support scalar items
    const parts: string[] = []
    let depth = 0, inDq = false, buf = ''
    for (let i = 0; i < inner.length; i++) {
        const c = inner[i]
        if (c === '"') { inDq = !inDq; buf += c; continue }
        if (!inDq) {
            if (c === '[' || c === '{') depth++
            else if (c === ']' || c === '}') depth--
            else if (c === ',' && depth === 0) {
                parts.push(buf.trim()); buf = ''; continue
            }
        }
        buf += c
    }
    if (buf.trim() !== '') parts.push(buf.trim())
    return parts.map(p => parseScalar(p))
}

function parseScalar(s: string): YamlValue {
    const t = s.trim()
    if (t === '' || t === '~' || t.toLowerCase() === 'null') return null
    if (t.toLowerCase() === 'true')  return true
    if (t.toLowerCase() === 'false') return false
    if (/^-?0x[0-9a-fA-F]+$/.test(t)) return parseInt(t, 16)
    if (/^-?\d+$/.test(t)) return parseInt(t, 10)
    if (/^-?\d*\.\d+$/.test(t)) return parseFloat(t)
    return unquote(t)
}

// ─── Accessor helpers ───
// Used by driver parseYaml() implementations to safely coerce values out of
// the untyped parse tree with sensible defaults.

export function asObj(v: YamlValue | undefined): Record<string, YamlValue> {
    return (v && typeof v === 'object' && !Array.isArray(v)) ? v as Record<string, YamlValue> : {}
}
export function asArr(v: YamlValue | undefined): YamlValue[] {
    return Array.isArray(v) ? v : []
}
export function asStr(v: YamlValue | undefined, dflt = ''): string {
    if (v == null) return dflt
    return typeof v === 'string' ? v : String(v)
}
export function asNum(v: YamlValue | undefined, dflt = 0): number {
    if (typeof v === 'number') return v
    if (typeof v === 'string') {
        const n = Number(v); return isNaN(n) ? dflt : n
    }
    if (typeof v === 'boolean') return v ? 1 : 0
    return dflt
}
export function asBool(v: YamlValue | undefined, dflt = false): boolean {
    if (typeof v === 'boolean') return v
    if (typeof v === 'number') return v !== 0
    if (typeof v === 'string') {
        const t = v.toLowerCase()
        if (t === 'true' || t === 'yes' || t === '1') return true
        if (t === 'false' || t === 'no' || t === '0') return false
    }
    return dflt
}
