# CLI Update Guide

> **ACTION DOCUMENT:** How to add commands to the Go interactive CLI (`scalefx-cli`).

> **Run the CLI / drive a board:** use the **scalefx-cli skill** — it knows the connect/subscribe/role-drive flows. This doc is about *adding* commands.

---

## CLI Architecture

The CLI is `app/go/console/` on top of `app/go/client/` (the typed API) on top of `app/go/protocol/` (the wire mirror). There is **no string-command "engine"** — the old `app/go/engine/` + `engine/handlers/<mod>/{handler,parsers,types,format}.go` stack was archived 2026-05-28. Do not reference it.

```
app/go/
├── protocol/<mod>/<mod>.go   - wire mirror: packet consts, CmdXxx builders, DecodeXxx (source of truth)
├── client/
│   ├── client.go             - Client: owns protocol.Connection + every typed sub-API
│   ├── <mod>.go              - typed API (Gun, Gear, Landing, Audio, Storage, Topology, …)
│   ├── roletarget.go         - RoleTarget: GUID-transparent role drive/query (Rule 58)
│   └── events.go             - async telemetry stream (OnRole / OnXxx)
└── console/
    ├── registry.go           - central command table + categories (register() / lookup())
    ├── session.go            - App struct (holds *client.Client as a.c), REPL, requireClient()
    ├── helpers.go            - arg parsers (parseU8, parseOnOff, parseKeyVals, …)
    ├── term.go               - output helpers (Ok, Note, Hdr, Phase, colour helpers)
    └── cmd_<mod>.go          - ONE file per wire-domain; self-registers its commands in init()
```

### The registry — self-registration, no master switch

Each `cmd_<mod>.go` owns one wire-domain and registers its commands in its own `init()`. `registry.go` collects them into a global table (it panics on duplicate names so init ordering is loud).

```go
// console/registry.go
type command struct {
    Name         string
    Usage        string
    Help         string
    Run          func(a *App, args []string) error
    Category     category
    RequiresConn bool   // command hidden until connected
    RequiresCap  uint32 // bitmask of CoreCapability bits; shown only if the board advertises one
}
func register(c *command) { /* adds to the global map; panics on duplicate */ }
```

`RequiresCap` gates a command's visibility in `help` on the connected board's capability mask (a UX layer — the wire dispatch still NACKs cleanly if invoked anyway). `RequiresConn: false` = Session commands available pre-connect.

### Capability detection

On connect, the client sends IDENTIFY (no state change), decodes the board name + capability word, and INITs expanders. `help` then shows only the commands whose `RequiresCap` bit the board advertises; compiled-but-disabled features appear in a separate "currently disabled" section (`disabledCommands`).

---

## Adding a Command to an Existing Domain

Edit the matching `console/cmd_<mod>.go`. Two parts: a `register(...)` line in `init()`, and a `cmd*` func that calls the typed `client` API.

```go
// console/cmd_gun.go
func init() {
    // …existing registrations…
    register(&command{
        Name:         "gun-ammo",
        Usage:        "gun-ammo <id> <count>",
        Help:         "set ammo count (0-9999)",
        Category:     catGun,
        RequiresConn: true,
        RequiresCap:  core.CapGunFx,
        Run:          cmdGunAmmo,
    })
}

func cmdGunAmmo(a *App, args []string) error {
    if err := a.requireClient(); err != nil {   // guards "not connected"
        return err
    }
    if len(args) != 2 {
        return fmt.Errorf("usage: gun-ammo <id> <count>")
    }
    id, err := parseU8(args[0])
    if err != nil {
        return err
    }
    count, err := strconv.ParseUint(args[1], 0, 16)
    if err != nil {
        return fmt.Errorf("count: %w", err)
    }
    if err := a.c.Gun.SetAmmo(id, uint16(count)); err != nil {  // typed API call
        return err
    }
    Ok("gun[%d] ammo = %d", id, count)
    return nil
}
```

The typed `SetAmmo` lives in `client/gunfx.go` and wraps `sendExpectACK` / `sendForResp` (see [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) steps 3–4). A CLI command NEVER builds packets directly — it goes through `a.c.<SubApi>.<Method>`.

### Query command (returns decoded data)

```go
func cmdGunStatus(a *App, _ []string) error {
    if err := a.requireClient(); err != nil {
        return err
    }
    st, err := a.c.Gun.Status()   // client decodes the typed RESP for you
    if err != nil {
        return err
    }
    Hdr("Guns")
    for _, g := range st {
        fmt.Fprintf(out, "  [%d] firing=%v smoke=%v\n", g.ID, g.Firing, g.SmokeArmed)
    }
    return nil
}
```

### Output helpers (`term.go`)

```go
Ok("done: %d", n)        // success line (green)
Note("(nothing to show)")// dim aside
Hdr("Section")           // section header
Phase("firing")          // coloured state token
cRed(...) / cMagenta(...)// inline colour
// errors are returned from the cmd func; the REPL prints them.
```

---

## Adding a New Domain

1. **Wire mirror** — `app/go/protocol/<mod>/<mod>.go`: packet constants + `CmdXxx` builders + `DecodeXxx` (mirrors the firmware protocol header, the source of truth).
2. **Capability + controller type** — add a `Cap<Mod>` bit and/or `Ctrl<Mod>` constant to `app/go/protocol/core/core.go` if the domain is gated.
3. **Typed API** — `app/go/client/<mod>.go`: a struct with a `*Client` back-ref + methods wrapping `sendExpectACK` / `sendForResp`. Wire it into `client.go`'s `Client` struct so `a.c.<Mod>` resolves.
4. **CLI file** — `app/go/console/cmd_<mod>.go`: register commands in `init()` + the `cmd*` funcs. Add a `category` constant to `registry.go` if the domain needs its own help section.
5. **Roles** (Rule 58) — if you're exposing an expander role rather than a hub effect, you usually don't add a new client sub-API: drive it through `a.c.Role(guid).<RoleVerb>` (`client/roletarget.go`). Adding the transparent path for a new role is a one-line `RoleTarget` wrapper + a role codec in `protocol/roles/`.

---

## Verification

```bash
cd app/go && go build ./...        # compiles console + client + protocol — the sync check

app/go/scalefx-cli.exe -p COM5     # or use the scalefx-cli skill
# > connect
# > help                           # new command appears (if the board advertises RequiresCap)
# > gun-ammo 0 500
```

---

## Checklist

```yaml
Adding_Command:
  - "[ ] protocol/<mod>/<mod>.go: packet const + CmdXxx (+ DecodeXxx if query)"
  - "[ ] client/<mod>.go: typed method (sendExpectACK / sendForResp)"
  - "[ ] console/cmd_<mod>.go: register(&command{…}) in init() + cmd* func"
  - "[ ] cmd* func: requireClient → parse args → call a.c.<Mod>.<Method> → Ok(...)"
  - "[ ] RequiresCap set if the domain is capability-gated"
  - "[ ] cd app/go && go build ./... passes"
  - "[ ] Command appears in 'help' and runs"

Adding_Domain:
  - "[ ] protocol/<mod>/<mod>.go created"
  - "[ ] Cap<Mod> / Ctrl<Mod> in protocol/core/core.go (if gated)"
  - "[ ] client/<mod>.go typed API + wired into client.go Client struct"
  - "[ ] console/cmd_<mod>.go created; category constant in registry.go if needed"
  - "[ ] cd app/go && go build ./... passes"
```
