# Studio Diagnostics

Instrumentation that lets you (or the agent) see exactly what the
Studio is doing, in order, when something breaks. Three sinks fire in
parallel for every event:

| Sink | Path | Best for |
|------|------|----------|
| **stdout** | terminal that ran `studio.exe` / `wails dev` | dev-loop iteration, terminal-savvy users |
| **on-disk log** | `%TEMP%\scalefx-studio.log` (Windows) or `/tmp/scalefx-studio.log` | the agent (Studio's a Windows GUI binary, so its stdout is silent in production) |
| **GUI console** | the Console panel inside Studio | the user, copy/paste into a chat or bug report |

Every diag event also fires a structured `diag:event` Wails event so a
custom Diag pane could render a live timeline.

## Format

```
[20:54:18.123] INFO  CONN     Connecting to tcp://localhost:9000
[20:54:18.456] INFO  CONN     connection state {"connected":true,"controller":"lightfx", … }
[20:54:18.500] DEBUG RPC      Connect ← done {"duration_ms":372}
[20:54:28.500] DEBUG HB       heartbeat {"goroutines":12,"heap_kb":3140,"connected":true,"controller":"lightfx", … }
```

`HH:MM:SS.mmm  LEVEL  TAG       message  {fields-as-json}`

Tags (informally — pick whatever's useful when adding new ones):

| Tag | Meaning |
|-----|---------|
| `APP`     | process lifecycle (startup, shutdown) |
| `CONN`    | serial / TCP connection lifecycle |
| `CMD`     | console commands typed by the user |
| `RPC`     | enter/exit of a Wails-bound method |
| `RX`      | unsolicited / async packet observed |
| `CFG`     | config download / upload / reload |
| `FW`      | firmware build + flash |
| `PORTS`   | port-list deltas from the watcher |
| `HB`      | periodic heartbeat |
| `DIAG`    | the diagnostic system itself (toggles, dumps) |
| `PANIC`   | a Wails method panicked — stack trace attached |
| `FE.*`    | events sourced from the frontend (FE.UNCAUGHT, FE.CONSOLE, FE.CONN, …) |

## Levels

| Level | Use |
|-------|-----|
| `DEBUG` | per-RPC enter/exit, heartbeat, async packet flow. **Off by default** in stdout / GUI; **always written** to the log file so a postmortem keeps the trace. |
| `INFO`  | user-visible state changes (connect, disconnect, save, reload) |
| `WARN`  | recoverable issues, slow RPCs (>500 ms), unexpected disconnect |
| `ERROR` | failed operations, panics, JS exceptions |

## Toggles + commands

In the Studio Console (or the CLI when connected through the engine):

```
/diag                # short summary (debug state, ring size)
/diag debug on       # turn on DEBUG-level logging
/diag debug off      # back to INFO
/diag dump           # dump the recent ring buffer (200 events) as JSON
/diag clear          # clear the ring after a repro
```

The `dump` form renders the ring as a `<pre>` block so it's easy to
select and copy into a chat.

Frontend code can also call `diag.info|warn|error|debug(tag, msg, fields?)`
imported from `lib/diag.ts` — those go through the same pipeline and
show up in stdout/log/GUI together with the Go-side breadcrumbs.

## What's automatically captured

### Backend (Go)

- Process startup / shutdown (`APP`)
- Diagnostic log path (so the agent knows where to `tail`)
- Every Wails RPC: `RPC <name> → enter` / `← done` with duration; `slow` warning >500 ms
- Panics in any Wails method: full stack trace under `PANIC`, then the panic re-raised
- Connect/Disconnect/Reconnect lifecycle with port + controller name + caps + build #
- All async packets received from the device (first STATUS at INFO, every other one at DEBUG)
- Port-watcher list deltas: `added=[…] removed=[…] total=N` whenever the OS-enumerated + virtual-board list changes
- 10 s heartbeat: connection state, goroutine count, heap size

### Frontend (JS/Svelte)

`lib/diag.ts:installDiagBridge()` runs on `App.svelte` mount and
hooks:

- `window.onerror` → `FE.UNCAUGHT` with filename, lineno, colno, stack
- `window.addEventListener('unhandledrejection')` → `FE.UNHANDLED_PROMISE`
- Wraps `console.error` → `FE.CONSOLE` (still calls original so DevTools sees it)
- Wraps `console.warn` → `FE.CONSOLE`
- Logs the user-agent + viewport on bridge install

Plus a few user-action breadcrumbs:

- `connection:changed` event → INFO with the new state
- Unexpected disconnect → WARN

## Usage with the Claude Code agent

When troubleshooting a Studio bug:

1. Reproduce the bug in Studio
2. (If you want detail) type `/diag debug on` in the Console
3. Reproduce again
4. Either:
   - Paste the GUI console output into chat, or
   - From the agent's terminal: `cat %TEMP%\scalefx-studio.log | tail -100`
   - Or in the GUI: type `/diag dump`, copy the JSON

The agent gets a full breadcrumb of every Wails call, every panel
transition, every async packet, every JS exception — in a single time-
ordered stream tagged for grepping.

## Adding more instrumentation

Inside any App method:

```go
func (a *App) MyMethod(arg string) error {
    defer a.diag.Around("MyMethod", map[string]any{"arg": arg})()
    a.diag.Info("CFG", "doing the thing with %s", arg)
    if err := step(); err != nil {
        a.diag.Warn("CFG", "step failed: %v", err)
        return err
    }
    return nil
}
```

`Around` adds enter / exit / panic-recovery / slow-warning. The
explicit `Info`/`Warn`/`Error` calls add domain breadcrumbs.

Inside any `.svelte` component:

```ts
import { diag } from './lib/diag'
// …
diag.info('FE.LIGHTFX', 'user dragged master brightness', { value: 75 })
```
