---
name: scalefx-cli
description: Drive a connected ScaleFX board from the scalefx-cli tool — run board commands, subscribe to live telemetry/diagnostics, inspect topology/ports/config. Use when the user asks to run a CLI command, query the board, subscribe to events, or reproduce a wire issue on the CLI.
---

# ScaleFX CLI

`app/go/scalefx-cli.exe` (build: `cd app/go && go build -o scalefx-cli.exe ./cli/`). Single-threaded — the safe place to reproduce wire behaviour without Studio's concurrency.

## Run a command
```bash
app/go/scalefx-cli.exe -p COM15 -c "<command>"     # one-shot
app/go/scalefx-cli.exe -p COM15                     # interactive REPL
```
Common: `connect`, `init`, `status`, `system-info`, `topo-ports`, `slaves`, `file.list`, `sd.status`, `config.status`, `coredump`.

## Board-prefix convention (Rule 30)
Board commands REQUIRE their group prefix; bare names error with a "did you mean" hint:
- LightFX → `light:` (`light:servo`, `light:program`)
- GearControl → `gear:` (`gear:reset`)
- GunFX → `gun:` (`gun:trigger`)
- HubFX → `hub:` (`hub:slaves`, `hub:topo-ports`)

Universal commands stay bare: `connect`, `init`, `status`, `file.list`, `sd.status`, `config.save`, etc.

## Subscribe to live telemetry / diagnostics
```bash
app/go/scalefx-cli.exe -p COM15 -c "subscribe"
```
Streams async events + firmware `[LOG I]` diagnostics until disconnect — the way to read instrumentation (e.g. `[servo] …`, `[ll] …`, `[lightfx-selector] …`) while exercising the board from Studio or the radio. Live-view broadcasts (RC channels, servo status) are lossy by design (Rule 53); registered flow-control ACKs are not.

## Output formatting (Rule 19 extension + colour)
CLI renderers live in `app/go/engine/handlers/<mod>/`:
- `types.go` — JSON structs + pure `Decode*` (no I/O).
- `format.go` — `Handler.FormatXxx(*Xxx)` → renders to `h.E.Out`.
- `parsers.go` — CLI-only query-response renderers (`status`, `slaves`).
Colourized status output uses the shared ANSI helpers in `app/go/engine/handlers/termcolor` (or the package the landing-status formatter uses) — green=active/ok, yellow=warn/transition, red=error/fault, dim=idle. Mirror that palette when adding a new instrument.

## Reproducing wire issues
The CLI never trips keepalive/upload-exclusivity/thread-safety hazards (single-threaded, no live-view, no pollers). A bug that only shows in Studio is almost always one of Rules 53–56 (lossy-vs-reliable async, stream-upload exclusivity, native-hardware, thread-safe `Connection`). **Always validate wire changes against Studio**, not just the CLI.
