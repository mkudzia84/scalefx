// Frontend → backend diagnostic bridge.
//
// Goal: every JS-side error or important user action shows up in the
// same stream as Go-side events, so the agent troubleshooting via the
// terminal log + the user's GUI console see the same sequence of
// breadcrumbs.
//
// Hooks installed by `installDiagBridge()`:
//
//   1. window.onerror → LogFrontend('error', 'FE.UNCAUGHT', …) with
//      file/line/stack
//   2. window.addEventListener('unhandledrejection') → same
//   3. console.error wrapper → forwards args to LogFrontend
//      (the original console.error is still called so DevTools sees it)
//   4. A diag.* helper exported below for explicit instrumentation
//      from .svelte components (diag.info, diag.warn, …)

import { LogFrontend } from '../../wailsjs/go/main/App'

type Level = 'debug' | 'info' | 'warn' | 'error'

let installed = false

function safeStringify(v: unknown): string {
    if (v == null) return ''
    if (typeof v === 'string') return v
    if (v instanceof Error) {
        return `${v.name}: ${v.message}\n${v.stack ?? '(no stack)'}`
    }
    try {
        return JSON.stringify(v)
    } catch {
        return String(v)
    }
}

function send(level: Level, tag: string, msg: string, fields?: Record<string, unknown>) {
    try {
        // Wails returns a Promise we don't need to await — fire and forget.
        // Coerce undefined fields → null so JSON.Marshal on the Go side is happy.
        const f: Record<string, unknown> = {}
        if (fields) {
            for (const k of Object.keys(fields)) {
                const val = fields[k]
                f[k] = val === undefined ? null : val
            }
        }
        void LogFrontend(level, tag, msg, f)
    } catch {
        // Swallow — the bridge itself must never throw, otherwise it
        // would mask real frontend errors.
    }
}

export const diag = {
    debug(tag: string, msg: string, fields?: Record<string, unknown>) { send('debug', tag, msg, fields) },
    info (tag: string, msg: string, fields?: Record<string, unknown>) { send('info',  tag, msg, fields) },
    warn (tag: string, msg: string, fields?: Record<string, unknown>) { send('warn',  tag, msg, fields) },
    error(tag: string, msg: string, fields?: Record<string, unknown>) { send('error', tag, msg, fields) },
}

export function installDiagBridge() {
    if (installed) return
    installed = true

    // 1) Uncaught synchronous errors.
    window.addEventListener('error', (event) => {
        send('error', 'FE.UNCAUGHT', event.message || 'window.onerror', {
            filename: event.filename,
            lineno: event.lineno,
            colno: event.colno,
            stack: (event.error && (event.error as Error).stack) || null,
        })
    })

    // 2) Unhandled promise rejections.
    window.addEventListener('unhandledrejection', (event) => {
        send('error', 'FE.UNHANDLED_PROMISE', safeStringify(event.reason), {
            type: 'PromiseRejection',
        })
    })

    // 3) Wrap console.error so anything the JS code logs as an error
    //    also shows up in the diag stream.
    const origError = console.error.bind(console)
    console.error = (...args: unknown[]) => {
        try {
            const msg = args.map(safeStringify).join(' ')
            send('error', 'FE.CONSOLE', msg)
        } catch { /* ignore */ }
        origError(...args)
    }

    // 4) Wrap console.warn similarly so frontend warnings also surface.
    const origWarn = console.warn.bind(console)
    console.warn = (...args: unknown[]) => {
        try {
            const msg = args.map(safeStringify).join(' ')
            send('warn', 'FE.CONSOLE', msg)
        } catch { /* ignore */ }
        origWarn(...args)
    }

    // 5) Wails connection-loss safety net: if the runtime drops, the
    //    `LogFrontend` call would throw. We catch above; nothing else
    //    to do here.

    diag.info('FE', 'diagnostic bridge installed', {
        userAgent: navigator.userAgent,
        viewport: `${window.innerWidth}x${window.innerHeight}`,
    })
}
