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

    // Svelte flush watchdog (2026-07-02 fresh-board freeze). If a component
    // update THROWS during a flush and the invoking context swallows it
    // (svelte-hmr's dev proxy does; some event bridges do too), Svelte 3
    // leaves `update_scheduled=true` with no flush pending — every component
    // in the app stops updating forever while stores/timers keep running
    // (the observed "UI locks on connect"). Svelte 3.59's flush() resets its
    // own state on throw, so a manual flush() from a plain task drains any
    // stranded dirty components — a cheap no-op when healthy, a self-heal +
    // culprit log when wedged.
    void import('svelte/internal').then(({ flush }) => {
        setInterval(() => {
            try {
                flush()
            } catch (e) {
                send('error', 'FE.FLUSH', `component update threw during flush: ${safeStringify(e)}`, {
                    stack: (e && (e as Error).stack) || null,
                })
            }
        }, 1000)
    })

    // Long-task observer — any main-thread stall >200 ms surfaces in the
    // diag stream (kept from the 2026-07-02 fresh-board freeze hunt; near
    // zero cost and names the culprit if a renderer stall ever recurs).
    try {
        const po = new PerformanceObserver((list) => {
            for (const e of list.getEntries()) {
                if (e.duration >= 200) {
                    send('warn', 'FE.LONGTASK', `main-thread task ${Math.round(e.duration)}ms`)
                }
            }
        })
        po.observe({ entryTypes: ['longtask'] })
    } catch { /* longtask unsupported */ }

    // 1) Uncaught synchronous errors.
    window.addEventListener('error', (event) => {
        send('error', 'FE.UNCAUGHT', event.message || 'window.onerror', {
            filename: event.filename,
            lineno: event.lineno,
            colno: event.colno,
            stack: (event.error && (event.error as Error).stack) || null,
        })
    })
    // Belt-and-suspenders: property-form handler too (some WebView2 builds
    // route one but not the other).
    window.onerror = (msg, src, line, col, err) => {
        send('error', 'FE.UNCAUGHT2', String(msg), {
            src, line, col, stack: (err && err.stack) || null,
        })
        return false
    }

    // 2) Unhandled promise rejections (addEventListener + property form).
    window.addEventListener('unhandledrejection', (event) => {
        send('error', 'FE.UNHANDLED_PROMISE', safeStringify(event.reason), {
            stack: (event.reason && (event.reason as Error).stack) || null,
        })
    })
    // (Verified 2026-07-02: this WebView2 delivers both unhandledrejection
    // and window.onerror to these handlers — a silent freeze therefore means
    // the error was SWALLOWED upstream, which is what the flush watchdog
    // above exists to expose.)

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

    // 6) Click tracer (2026-07-26 "hanging UI" hunt): log every click at the
    //    CAPTURE phase with a compact CSS path of the real target.  When the
    //    UI "hangs" (tabs don't respond), this shows whether the click even
    //    reaches the control or an invisible overlay swallows it — and it
    //    keeps working even when Svelte's update pipeline is wedged, because
    //    it's a plain DOM listener.  User-driven → negligible volume.
    const cssPath = (el: Element | null): string => {
        const parts: string[] = []
        for (let n = el, i = 0; n && i < 5; n = n.parentElement, i++) {
            let s = n.tagName.toLowerCase()
            if (n.id) s += `#${n.id}`
            else if (typeof n.className === 'string' && n.className.trim())
                s += '.' + n.className.trim().split(/\s+/).slice(0, 3).join('.')
            parts.unshift(s)
        }
        return parts.join(' > ')
    }
    document.addEventListener('click', (ev) => {
        send('debug', 'FE.CLICK', cssPath(ev.target as Element), {
            x: ev.clientX, y: ev.clientY,
            top: cssPath(document.elementFromPoint(ev.clientX, ev.clientY)),
        })
    }, { capture: true, passive: true })

    diag.info('FE', 'diagnostic bridge installed', {
        userAgent: navigator.userAgent,
        viewport: `${window.innerWidth}x${window.innerHeight}`,
    })
}
