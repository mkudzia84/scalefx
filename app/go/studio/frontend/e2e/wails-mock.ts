import type { Page } from '@playwright/test'

// installWailsMock injects a fake Wails runtime + bound-method surface BEFORE
// the app's scripts run, so the built Studio SPA boots in a plain browser with
// no Go backend / board.
//
//   - window.runtime.*  → no-op event/log shims (EventsOn returns an
//     unsubscribe fn so the app's listener wiring works).
//   - window.go.main.App.<Method>(...) → resolves the per-test value from
//     `responses[Method]`, else null. The app's onMount is try/caught, so it
//     boots regardless of which methods you stub.
//
// `responses` must be JSON-serialisable (it crosses into the page context).
// Stub only what a spec asserts on, e.g.:
//   installWailsMock(page, { GetLandingConfig: { schemaVersion: 1, lights: [...] } })
export async function installWailsMock(
  page: Page,
  responses: Record<string, unknown> = {},
): Promise<void> {
  await page.addInitScript((resp) => {
    const noop = () => {}
    const unsub = () => noop
    ;(window as any).runtime = {
      EventsOn: unsub,
      EventsOnMultiple: unsub,
      EventsOnce: noop,
      EventsEmit: noop,
      EventsOff: noop,
      LogPrint: noop, LogTrace: noop, LogDebug: noop, LogInfo: noop,
      LogWarning: noop, LogError: noop, LogFatal: noop,
      WindowReload: noop, WindowReloadApp: noop, Quit: noop,
      BrowserOpenURL: noop,
      ClipboardSetText: () => Promise.resolve(true),
      ClipboardGetText: () => Promise.resolve(''),
      WindowSetTitle: noop, Environment: () => Promise.resolve({ buildType: 'dev' }),
    }
    const handler: ProxyHandler<object> = {
      get: (_t, name: string) => (..._args: unknown[]) => {
        if (Object.prototype.hasOwnProperty.call(resp, name)) {
          return Promise.resolve((resp as Record<string, unknown>)[name])
        }
        return Promise.resolve(null)
      },
    }
    ;(window as any).go = { main: { App: new Proxy({}, handler) } }
  }, responses)
}
