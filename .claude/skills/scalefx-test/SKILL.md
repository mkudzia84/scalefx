---
name: scalefx-test
description: Run the ScaleFX test suites (Go unit, Go integration against connected HubFX, native C++ doctest) and the pre-merge gate. Use when the user asks to run tests, check the pre-merge gate, or before merging to main.
---

# ScaleFX tests & pre-merge gate

One entrypoint: `tools/run-tests.ps1` (PowerShell). Run from the repo root.

## Modes
```powershell
.\tools\run-tests.ps1                 # unit-only (~10 s) — Go unit suites; no hardware needed (CI-safe)
.\tools\run-tests.ps1 -Integration   # unit + integration; integration SKIPS cleanly without HW
.\tools\run-tests.ps1 -Premerge       # FULL GATE: unit + integration (needs HW) + native C++ + firmware build
```
VS Code task equivalents: `Test: Run Suite (Default | Integration | Premerge)`.

## Pre-merge gate (Rule 52) — required before merging to `main`
`.\tools\run-tests.ps1 -Premerge` MUST exit 0 (`READY TO MERGE`) before opening a PR against `main` or fast-forwarding into it. It gates on:
1. every `tests/host/go_unit/*` suite passes,
2. every `tests/host/go_integration/*` suite passes against the connected HubFX (`SCALEFX_HUBFX_PORT` env var, or CH343 `1A86:55D3` auto-detect),
3. native C++ doctest (`tests/native/`, `[env:native]`),
4. `scalefx-flash build hubfx --no-clean` succeeds.

Missing hardware fails `-Premerge` fast — you must be at the HW dev machine to merge to `main`. **Close/disconnect Studio first** so the integration tests can open the serial port.

## Test layout (Rules 21 / 51)
- `tests/host/go_unit/<name>/` — fast Go tests (own `go.mod`, `replace scalefx => ../../../../app/go`).
- `tests/host/go_integration/<name>/` — hardware-required Go tests; each MUST `t.Skip` on `testing.Short()` AND on missing port (so they skip cleanly without HW, never bomb CI).
- `tests/native/` — C++ host-compilable doctest units.
- `tests/hw/<name>/` — physical bring-up sketches.
- Production code (`app/go/`, `controllers/`) must NOT import from `tests/`.

A test that no longer **builds** is a regression of the same severity as a firmware compile error (Rule 51) — when a refactor renames/archives a package a test imports, move or delete that test in the **same** commit.

## After a protocol change
`cd app/go && go build ./...` is the primary sync check (Go mirrors the C++ wire format). The `roles_protocol_test` / `<mod>_protocol_test` unit suites assert the packet/error constants — run them after touching `serial/<mod>/<mod>.h`.
