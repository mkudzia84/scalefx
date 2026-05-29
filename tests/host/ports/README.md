# tests/host/ports/

Shared helpers used by `tests/host/go_integration/*` to discover and
own a HubFX board for the duration of a test.

- `hubfx_port.go` — `OpenHubFX(t *testing.T) *client.Client` that
  resolves the port via env var → CH343 auto-detect → `t.Skip`.

Importable from any `go_integration/*` package via the standard
`replace scalefx => ../../../../app/go` chain.
