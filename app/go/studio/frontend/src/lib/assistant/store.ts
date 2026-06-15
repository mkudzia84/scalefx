// assistant/store — chat state for the Assistant dock pane.
//
// Advisory by design: the assistant is a standalone service (services/ai-assistant)
// that grounds the chosen model in the embedded textbook + the live config context
// (built here from the device model + effect drafts) and returns prose.  Studio is a
// thin REST client — no provider keys here, no tool mutates config.

import { writable, get } from 'svelte/store'
import { AssistantAsk, AssistantStatus, AssistantSummarize } from '../../../wailsjs/go/main/App'
import { buildAssistantContext } from './context'

export interface ChatMsg {
    role: 'user' | 'model'
    content: string
    error?: boolean
    summary?: boolean   // a compacted "earlier conversation" placeholder
}

// Conversation compaction: once the cumulative message text gets large, fold the
// older turns into one model-written summary and keep only the last few verbatim,
// so the per-request token cost (history) stops growing without losing context.
const COMPACT_CHARS = 16000  // total content size (~4k tokens) that triggers a compaction
const KEEP_RECENT   = 4      // most-recent messages kept verbatim (≈ 2 exchanges)

export interface ModelInfo {
    id: string
    provider: string
    label: string
}

export interface AssistantStatusInfo {
    available: boolean
    endpoint: string
    models: ModelInfo[]
    error?: string
}

export const messages = writable<ChatMsg[]>([])
export const busy = writable(false)
export const compacting = writable(false) // true while a summary is being generated
export const status = writable<AssistantStatusInfo | null>(null)
export const model = writable<string>('') // selected model id (sent with each chat)

export async function refreshStatus(): Promise<void> {
    try {
        const s = (await AssistantStatus()) as unknown as AssistantStatusInfo
        status.set(s)
        // Default the selection to the first model when unset or no longer offered.
        const ids = (s.models || []).map(m => m.id)
        const cur = get(model)
        if (!cur || !ids.includes(cur)) model.set(ids[0] || '')
    } catch (e) {
        status.set({ available: false, endpoint: '', models: [], error: String(e) })
    }
}

export async function ask(text: string): Promise<void> {
    const t = text.trim()
    if (!t || get(busy)) return
    messages.update(m => [...m, { role: 'user', content: t }])
    busy.set(true)
    try {
        const history = get(messages).map(m => ({ role: m.role, content: m.content }))
        const ctx = buildAssistantContext()
        const reply = await AssistantAsk(history as any, ctx, get(model))
        if (reply && reply.error) {
            messages.update(m => [...m, { role: 'model', content: reply.error, error: true }])
        } else {
            messages.update(m => [...m, { role: 'model', content: (reply && reply.text) || '(empty response)' }])
        }
    } catch (e) {
        messages.update(m => [...m, { role: 'model', content: String(e), error: true }])
    } finally {
        busy.set(false)
    }
    // After the turn settles, compact in the background if the history got large.
    void maybeCompact()
}

function historyChars(msgs: ChatMsg[]): number {
    return msgs.reduce((n, m) => n + (m.content ? m.content.length : 0), 0)
}

// maybeCompact folds the older turns into a single summary message when the
// conversation grows past COMPACT_CHARS, keeping the last KEEP_RECENT verbatim.
// It rolls any existing summary forward (so nothing is permanently lost) and
// fails safe — on any error the full history is left untouched.
async function maybeCompact(): Promise<void> {
    if (get(compacting) || get(busy)) return
    const all = get(messages)
    if (historyChars(all) < COMPACT_CHARS) return
    if (all.length <= KEEP_RECENT + 1) return   // not enough to fold meaningfully

    const older  = all.slice(0, all.length - KEEP_RECENT)
    const recent = all.slice(all.length - KEEP_RECENT)
    // Don't summarize a lone existing summary against itself.
    if (older.every(m => m.summary)) return

    compacting.set(true)
    try {
        const toFold = older.map(m => ({ role: m.role, content: m.content }))
        const res = await AssistantSummarize(toFold as any, get(model))
        const summary = res && (res as any).summary
        if (summary && String(summary).trim()) {
            const summaryMsg: ChatMsg = {
                role: 'model',
                content: '[Summary of the earlier conversation]\n' + String(summary).trim(),
                summary: true,
            }
            // Only replace if the user hasn't sent a new turn meanwhile.
            if (get(messages).length === all.length) {
                messages.set([summaryMsg, ...recent])
            }
        }
    } catch {
        /* keep the full history on failure */
    } finally {
        compacting.set(false)
    }
}

export function clearChat(): void {
    messages.set([])
}
