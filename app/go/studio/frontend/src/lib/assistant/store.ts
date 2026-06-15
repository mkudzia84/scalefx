// assistant/store — chat state for the Assistant dock pane.
//
// Advisory by design: the assistant is a standalone service (services/ai-assistant)
// that grounds the chosen model in the embedded textbook + the live config context
// (built here from the device model + effect drafts) and returns prose.  Studio is a
// thin REST client — no provider keys here, no tool mutates config.

import { writable, get } from 'svelte/store'
import { AssistantAsk, AssistantStatus } from '../../../wailsjs/go/main/App'
import { buildAssistantContext } from './context'

export interface ChatMsg {
    role: 'user' | 'model'
    content: string
    error?: boolean
}

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
}

export function clearChat(): void {
    messages.set([])
}
