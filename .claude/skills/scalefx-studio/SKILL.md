---
name: scalefx-studio
description: Build and run ScaleFX Studio, the Wails v2 + Svelte desktop GUI. Use when the user asks to build/run/rebuild Studio, regenerate Wails bindings, or verify a Studio frontend/backend change compiles.
---

# ScaleFX Studio (Wails v2 + Svelte)

Lives in `app/go/studio/` (Go backend `app*.go` + Wails) with the Svelte frontend in `app/go/studio/frontend/`.

## Verify a change compiles (the fast inner loop)
Most Studio edits are frontend-only. To check a Svelte/TS change builds:
```bash
cd app/go/studio/frontend && npm run build
```
Exit 0 = good. Ignore the recurring `GunFxPanel.svelte … Unused CSS selector ".rof-bar.overlap-error::after"` warning — it's pre-existing noise. Grep the output for real errors (`not assignable`, `Cannot find`, `[vite] … error`), not for the substring "error" (it matches CSS class names like `.banner.err`).

To check the Go backend compiles after editing `app/go/studio/*.go`:
```bash
cd app/go/studio && go build ./...
```

## Full desktop build / run
```bash
cd app/go/studio && wails build          # full production .exe
cd app/go/studio && wails dev            # hot-reload dev session
```
Prefer the VS Code tasks `Build ScaleFX Studio (GUI)` / `Run ScaleFX Studio (GUI)` when available (they set cwd). The user typically restarts Studio themselves after a build — don't assume a running instance picked up your change.

## Wails bindings (generated — don't hand-edit logic)
`app/go/studio/frontend/wailsjs/` mirrors the Go App methods + DTOs. After adding/changing an exported `App` method or a DTO struct, the bindings (`go/main/App.{js,d.ts}`, `go/models.ts`) need regenerating (a `wails build`/`generate` does it). These files churn with CRLF↔LF — when committing, check `git diff --numstat`; a file with no numeric delta is pure line-ending noise → `git checkout --` it. Only commit real content changes (e.g. a new DTO field).

## Design-system rules (don't reinvent)
Studio composes ONE design language in `app/go/studio/frontend/src/style.css`. Before adding UI, check the shared component/class catalog in **`instructions/23-STUDIO-WIDGET-CATALOG.md`** and the relevant copilot-instructions rules (34, 36, 38, 41, 44–50). Reuse `button`/`.field-input`/`.card`/`.form-row`/`.state-toggle`/`ServoIoWidget`/`ChannelToggleCluster`/`SoundRow` etc.; component `<style>` blocks do layout only, never re-skin controls. CSS vars only — never inline a hex.

## Reactivity trap (recurring bug)
A helper that reads a `$store` **inside its body** is invisible to Svelte's dependency tracker, so `{@const x = fn(arg)}` freezes at its first value. Use a reactive factory: `$: fn = makeFn($store)`. (Rule 44 codifies this for device-model lookups — see `ServoIoWidget` usage in `GunFxPanel`/`LandingPanel`.)
