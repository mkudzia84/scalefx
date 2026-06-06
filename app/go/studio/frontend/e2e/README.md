# Studio E2E (Playwright)

Full-browser end-to-end tests: the **real built Studio SPA** runs in Chromium
with a **mocked Wails backend** (no Go process, no connected board), so specs
drive the actual component tree + reactivity and assert rendered output — the
layer the Vitest unit/component tests don't fully exercise.

This is **not** in the pre-merge gate (it needs a browser binary). Run it
locally.

## Setup (once)

```bash
cd app/go/studio/frontend
npm install                       # brings @playwright/test
npx playwright install chromium   # ~120 MB browser binary
```

## Run

```bash
npm run e2e
```

Playwright auto-starts the dev server (`vite dev` on 127.0.0.1:4173) via the
`webServer` block in `playwright.config.ts`. If that auto-spawn times out
(some environments leave a port lingering / bind to IPv6), start the server
yourself first — `reuseExistingServer` picks it up:

```bash
npm run dev -- --port 4173 --strictPort --host 127.0.0.1   # terminal 1
npm run e2e                                                  # terminal 2
```

## How it works

- `wails-mock.ts` — `installWailsMock(page, responses)` injects `window.runtime`
  (no-op event/log shims) + `window.go.main.App.<Method>` (resolves
  `responses[Method]`, else `null`) **before** the app's scripts run. The app's
  `onMount` is try/caught, so it boots regardless of which methods you stub.
- `smoke.spec.ts` — proves the harness: app boots + `.app-layout` renders + no
  uncaught page error.

## Extending — the high-value flow

Add per-feature specs that stub the config getters and assert the real render.
Example (the GUID-redesign dropdown regression this harness exists to catch):

```ts
import { test, expect } from '@playwright/test'
import { installWailsMock } from './wails-mock'

test('a loaded landing config resolves the servo dropdown selection', async ({ page }) => {
  await installWailsMock(page, {
    GetConnectionInfo: { connected: true, controllerType: 'hubfx', port: 'COM15' },
    DeviceCapabilities: 0xffff,
    // a hub-local servo port in the model + the same ref in the config:
    RefreshDeviceModel: { ports: [/* servo ref guid:"" */], claims: [] },
    GetLandingConfig: { schemaVersion: 1, lights: [/* servo port guid:"" */] },
  })
  await page.goto('/')
  // navigate to Lighting → Retractable Lights, assert the <select> shows the option
})
```

Mock only what a spec asserts on; keep responses JSON-serialisable (they cross
into the page context).
