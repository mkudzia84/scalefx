// assistant/store — chat state for the Assistant dock pane.
//
// Advisory by design: AssistantAsk grounds the active provider (Gemini / Groq)
// in the embedded textbook + the live config context (built here from the
// device model + effect drafts) and returns prose.  No tool mutates config.

import { writable, get } from 'svelte/store'
import { AssistantAsk, AssistantStatus, SetAssistantKey, SetAssistantProvider } from '../../../wailsjs/go/main/App'
import { buildAssistantContext } from './context'

export interface ChatMsg {
    role: 'user' | 'model'
    content: string
    error?: boolean
}

export interface ProviderInfo {
    id: string
    label: string
    hasKey: boolean
    keySource: string
}

export interface AssistantStatusInfo {
    provider: string
    model: string
    available: boolean
    keySource: string
    providers: ProviderInfo[]
}

export const messages = writable<ChatMsg[]>([])
export const busy = writable(false)
export const status = writable<AssistantStatusInfo | null>(null)

export async function refreshStatus(): Promise<void> {
    try {
        status.set((await AssistantStatus()) as unknown as AssistantStatusInfo)
    } catch {
        status.set(null)
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
        const reply = await AssistantAsk(history as any, ctx)
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

export async function setKey(provider: string, key: string): Promise<void> {
    await SetAssistantKey(provider, key)
    await refreshStatus()
}

export async function setProvider(id: string): Promise<void> {
    await SetAssistantProvider(id)
    await refreshStatus()
}

export function clearChat(): void {
    messages.set([])
}
