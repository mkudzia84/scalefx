import { test, expect } from '@playwright/test'
import { installWailsMock } from './wails-mock'

// Smoke E2E — proves the harness: the real built SPA boots in a browser with
// a mocked Wails backend and renders its shell without a fatal error. Extend
// with per-flow specs (e.g. stub GetLandingConfig with a hub-local servo ref
// and assert the LandingFx servo dropdown resolves the selection — the GUID
// redesign regression).
test.describe('Studio shell', () => {
  test('boots with a mocked Wails backend and renders the app layout', async ({ page }) => {
    const errors: string[] = []
    page.on('pageerror', (e) => errors.push(String(e)))

    await installWailsMock(page, {
      GetConnectionInfo: { connected: false, controllerType: '', port: '' },
      DeviceCapabilities: 0,
    })
    await page.goto('/')

    await expect(page).toHaveTitle(/ScaleFX Studio/i)
    await expect(page.locator('.app-layout')).toBeVisible()
    // The mount must not throw an uncaught error (the boot path is try/caught,
    // so a clean boot = no pageerror).
    expect(errors, errors.join('\n')).toHaveLength(0)
  })
})
