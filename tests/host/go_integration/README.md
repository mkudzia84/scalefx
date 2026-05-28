# tests/host/go_integration/

Go integration tests that drive a real connected HubFX over USB
serial. Each test:

- Honours `testing.Short()` — `go test -short` skips the whole tree.
- Resolves the port via `tests/host/ports` (env var
  `SCALEFX_HUBFX_PORT` first, then CH343 auto-detect).
- Calls `t.Skip(...)` cleanly when no hardware is reachable.
- Cleans up after itself (deletes uploaded files, stops audio,
  resets config).

See **Rule 51** in [.github/copilot-instructions.md](../../../.github/copilot-instructions.md).
