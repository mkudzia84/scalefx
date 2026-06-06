import { defineConfig, devices } from '@playwright/test'

// Full-browser E2E for ScaleFX Studio's frontend. The built SPA is served by
// `vite preview`; a fake Wails runtime (e2e/wails-mock.ts) is injected so the
// app boots without the Go backend / a connected board. Specs drive the real
// component tree and assert rendered output (the layer Vitest unit/component
// tests don't fully exercise).
//
// NOT in the pre-merge gate by default (needs a browser binary —
// `npx playwright install chromium`). Run locally with `npm run e2e`.
export default defineConfig({
  testDir: './e2e',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: 'list',
  use: {
    baseURL: 'http://127.0.0.1:4173',
    trace: 'on-first-retry',
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
  ],
  // Serve via the vite dev server (starts in ~1 s — no build wait). The mock
  // (e2e/wails-mock.ts) supplies window.go/window.runtime either way.
  webServer: {
    command: 'npm run dev -- --port 4173 --strictPort --host 127.0.0.1',
    url: 'http://127.0.0.1:4173',
    reuseExistingServer: !process.env.CI,
    timeout: 60_000,
  },
})
