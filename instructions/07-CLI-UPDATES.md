# CLI Update Guide

> **ACTION DOCUMENT:** How to add commands and controllers to the Go interactive CLI.

---

## CLI Architecture

The CLI uses a modular handler architecture. Each controller has its own handler package under `engine/handlers/`. The engine owns the connection, API client, command dispatch, and listener lifecycle. Both the CLI and GUI console embed the `Engine` struct to share behavior.

### Engine Architecture

```
engine/
├── engine.go          - Core Engine struct (connection, API, dispatch, listener)
├── types.go           - CmdEntry, CmdGroup, InitReadyInfo, ControllerColors
├── output.go          - Output interface + ANSI terminal implementation
├── helpers.go         - Shared utilities (Atoi, ParseBool, ServoSet, ServoConfig, RequireArgs)
├── parsers.go         - Common response parsers (shared across handlers)
├── parsers_core.go    - Core response parsers (INIT_READY, STATUS header, I2C, LOG)
└── handlers/
    ├── handlers.go       - RegisterDefaults() — registers all built-in groups
    ├── core/handler.go   - Core commands (connect, init, status, reboot, etc.)
    ├── gunfx/handler.go  - GunFX commands (trigger, servo, smoke)
    ├── lightfx/
    │   ├── handler.go    - LightFX commands (LED, sequences, servo, landing lights)
    │   └── parsers.go    - LightFX response parsers
    ├── gearcontrol/
    │   ├── handler.go    - GearControl commands (gear, servo, yaw, calibration)
    │   └── parsers.go    - GearControl response parsers
    ├── hubfx/
    │   ├── handler.go    - HubFX commands (slaves, audio, engine, storage, USB)
    │   ├── parsers.go    - HubFX response parsers
    │   └── format.go     - HubFX output formatting
    └── firmware/handler.go - Firmware commands (releases, flash)
```

### Key Types

```go
// CmdEntry defines a single CLI command.
type CmdEntry struct {
    Handler     func(args []string)
    Usage       string
    Description string
    RequireInit bool
}

// CmdGroup defines a group of related commands (one per controller type).
type CmdGroup struct {
    Name       string
    Controller string // empty = universal
    Color      Color
    Commands   map[string]CmdEntry
}
```

### Dynamic Detection

Controller type is detected via IDENTIFY on connect:

```yaml
trigger: "IDENTIFY response on connect (or INIT_READY fallback)"
location: "engine/handlers/core/handler.go"
flow:
  - "1. Send IDENTIFY (0xFE) — no state change on device"
  - "2. Parse board name, detect controller type"
  - "3. HubFX (autonomous hub): mark initialized, skip INIT"
  - "4. Slave controllers: send INIT to activate hardware"
  - "5. Legacy boards (no IDENTIFY): fall back to INIT"
result: "Controller-specific handler commands appear in help"
```

### Registration Pattern

All handlers register themselves via a `Register(eng *engine.Engine)` function:

```go
// In handlers/handlers.go — called by both CLI and GUI:
func RegisterDefaults(eng *engine.Engine) {
    core.Register(eng)
    gunfx.Register(eng)
    gearcontrol.Register(eng)
    lightfx.Register(eng)
    hubfx.Register(eng)
}

// Each handler's Register():
func Register(eng *engine.Engine) {
    h := &Handler{E: eng}
    eng.RegisterStatusParser(pcore.CtrlGunFX, h.parseGunFXStatus)
    eng.RegisterAsyncHandler(gfxp.PktSomeAsync, h.handleSomeAsync)
    eng.AddGroup(h.commands())
}
```

---

## File Locations

```yaml
Engine:
  "engine/engine.go": "Core Engine struct (connection, API, dispatch, listener lifecycle)"
  "engine/types.go": "CmdEntry, CmdGroup, InitReadyInfo, controller type constants"
  "engine/output.go": "Output interface, ANSI color helpers"
  "engine/helpers.go": "Shared utilities (Atoi, ParseBool, OnOff, ServoSet, ServoConfig)"
  "engine/parsers.go": "Common response parsers"
  "engine/parsers_core.go": "Core response parsers (INIT_READY, STATUS header)"

Handlers:
  "engine/handlers/handlers.go": "RegisterDefaults() — registers all built-in groups"
  "engine/handlers/core/handler.go": "Core and protocol commands (15 commands)"
  "engine/handlers/gunfx/handler.go": "GunFX controller commands + status parser"
  "engine/handlers/lightfx/handler.go": "LightFX controller commands"
  "engine/handlers/lightfx/parsers.go": "LightFX response parsers"
  "engine/handlers/gearcontrol/handler.go": "GearControl controller commands"
  "engine/handlers/gearcontrol/parsers.go": "GearControl response parsers"
  "engine/handlers/hubfx/handler.go": "HubFX hub commands (audio, engine, storage, USB)"
  "engine/handlers/hubfx/parsers.go": "HubFX response parsers"
  "engine/handlers/hubfx/format.go": "HubFX output formatting helpers"
  "engine/handlers/firmware/handler.go": "Firmware release commands"

Protocol:
  "protocol/core/core.go": "Core packet type constants, error codes, controller type strings"
  "protocol/gunfx/gunfx.go": "GunFX packet constants, error codes, command builders"
  "protocol/lightfx/lightfx.go": "LightFX packet constants, error codes, command builders"
  "protocol/gearcontrol/gearcontrol.go": "GearControl packet constants, error codes, command builders"
  "protocol/hubfx/hubfx.go": "HubFX packet constants, error codes, command builders"

API:
  "api/client.go": "API client that wraps protocol.Connection"
  "api/gunfx.go": "GunFxApi (typed command methods)"
  "api/lightfx.go": "LightFxApi (typed command methods)"
  "api/gearcontrol.go": "GearControlApi (typed command methods)"
  "api/hubfx.go": "HubFxApi (typed command methods)"
```

---

## Adding Command to Existing Controller

### Step 1: Add API Method

**FILE:** `api/gunfx.go` (or `lightfx.go`, `gearcontrol.go`, `hubfx.go`)

```go
// NewCommand sends the new command with the given parameters.
func (g *GunFxApi) NewCommand(param1 uint8, param2 uint16) api.ApiResult {
    return g.send(gfxp.PktNewCommand, gunfx.NewCommand(param1, param2))
}
```

### Step 2: Add Protocol Command Builder

**FILE:** `protocol/gunfx/gunfx.go`

```go
// NewCommand builds the NEW_COMMAND payload.
func NewCommand(param1 uint8, param2 uint16) []byte {
    buf := make([]byte, 3)
    buf[0] = param1
    binary.LittleEndian.PutUint16(buf[1:], param2)
    return buf
}
```

### Step 3: Add Packet Type Constant

**FILE:** `protocol/gunfx/gunfx.go`

```go
const PktNewCommand protocol.PacketType = 0x0F  // [param1:u8][param2:u16LE]
```

### Step 4: Add Handler Command

**FILE:** `engine/handlers/gunfx/handler.go`

Add to the `commands()` map:

```go
func (h *Handler) commands() *engine.CmdGroup {
    return &engine.CmdGroup{
        Name:       "GunFX",
        Controller: pcore.CtrlGunFX,
        Color:      engine.ColorRed,
        Commands: map[string]engine.CmdEntry{
            // ...existing commands...
            "newcmd": {h.cmdNewCmd, "newcmd <param1> [param2]", "Execute new command", true},
        },
    }
}
```

Add the handler method:

```go
func (h *Handler) cmdNewCmd(args []string) {
    if !h.E.RequireArgs(args, 1, "newcmd <param1> [param2]") {
        return
    }
    param1 := engine.Atoi(args[0])
    param2 := 0
    if len(args) > 1 {
        param2 = engine.Atoi(args[1])
    }
    if param1 < 0 || param1 > 100 {
        h.E.Out.Error("param1 must be 0-100")
        return
    }
    h.E.Ack(h.E.API.GunFx.NewCommand(byte(param1), uint16(param2)),
        fmt.Sprintf("New command: param1=%d, param2=%d", param1, param2))
}
```

### Step 5: Add Response Parser (if query command)

**FILE:** `engine/handlers/gunfx/handler.go` (or create `parsers.go` if not present)

```go
func (h *Handler) parseNewCmdResponse(payload []byte) {
    if len(payload) < 4 {
        h.E.Out.Error("Short response")
        return
    }
    value := binary.LittleEndian.Uint16(payload[0:2])
    h.E.Out.Data("Value", "%d", value)
}
```

---

## Adding New Controller Type

### Step 1: Create Protocol Package

**CREATE:** `protocol/newfx/newfx.go`

```go
package newfx

import "scalefx/protocol"

// Packet type constants (must match C++ NewFxPacket namespace).
const (
    PktCommand1 protocol.PacketType = 0xB0
    PktCommand2 protocol.PacketType = 0xB1
)
```

**ADD to:** `protocol/newfx/newfx.go`

```go
package newfx

import "encoding/binary"

func Command1(param uint8) []byte {
    return []byte{param}
}

func Command2(id uint8, value uint16) []byte {
    buf := make([]byte, 3)
    buf[0] = id
    binary.LittleEndian.PutUint16(buf[1:], value)
    return buf
}
```

### Step 2: Add Controller Type Constant

**FILE:** `protocol/core/core.go`

```go
const CtrlNewFX = "newfx"
```

### Step 3: Create API Client

**CREATE:** `api/newfx.go`

```go
package api

import (
    "scalefx/protocol/newfx"
)

type NewFxApi struct{ apiClient }

func (n *NewFxApi) Command1(param uint8) ApiResult {
    return n.send(newfx.PktCommand1, newfx.Command1(param))
}

func (n *NewFxApi) Command2(id uint8, value uint16) ApiResult {
    return n.send(newfx.PktCommand2, newfx.Command2(id, value))
}
```

Add to `api/client.go`:

```go
type Client struct {
    // ...existing fields...
    NewFx *NewFxApi
}
```

### Step 4: Create Handler Package

**CREATE:** `engine/handlers/newfx/handler.go`

```go
package newfx

import (
    "fmt"
    "scalefx/engine"
    pcore "scalefx/protocol/core"
)

type Handler struct {
    E *engine.Engine
}

func Register(eng *engine.Engine) {
    h := &Handler{E: eng}
    eng.RegisterStatusParser(pcore.CtrlNewFX, h.parseNewFXStatus)
    eng.AddGroup(h.commands())
}

func (h *Handler) commands() *engine.CmdGroup {
    return &engine.CmdGroup{
        Name:       "NewFX",
        Controller: pcore.CtrlNewFX,
        Color:      engine.ColorYellow,
        Commands: map[string]engine.CmdEntry{
            "newcmd1": {h.cmdCommand1, "newcmd1 <param>", "Execute command 1", true},
            "newcmd2": {h.cmdCommand2, "newcmd2 <id> <value>", "Execute command 2", true},
        },
    }
}

func (h *Handler) cmdCommand1(args []string) {
    if !h.E.RequireArgs(args, 1, "newcmd1 <param>") {
        return
    }
    param := engine.Atoi(args[0])
    h.E.Ack(h.E.API.NewFx.Command1(byte(param)),
        fmt.Sprintf("Command 1: param=%d", param))
}

func (h *Handler) cmdCommand2(args []string) {
    if !h.E.RequireArgs(args, 2, "newcmd2 <id> <value>") {
        return
    }
    id, value := engine.Atoi(args[0]), engine.Atoi(args[1])
    h.E.Ack(h.E.API.NewFx.Command2(byte(id), uint16(value)),
        fmt.Sprintf("Command 2: id=%d, value=%d", id, value))
}

func (h *Handler) parseNewFXStatus(data []byte) {
    h.E.Out.Header("NewFX")
    // ...parse fields from data...
}
```

### Step 5: Register Handler

**FILE:** `engine/handlers/handlers.go`

```go
import "scalefx/engine/handlers/newfx"

func RegisterDefaults(eng *engine.Engine) {
    core.Register(eng)
    gunfx.Register(eng)
    gearcontrol.Register(eng)
    lightfx.Register(eng)
    hubfx.Register(eng)
    newfx.Register(eng)  // ADD
}
```

### Step 6: Add Controller Detection

**FILE:** `engine/handlers/core/handler.go`

In the controller detection logic, add:

```go
case "newfx":
    eng.ControllerType = pcore.CtrlNewFX
```

### Step 7: Update Controller Maps

**FILE:** `engine/types.go`

```go
var ControllerColors = map[string]Color{
    // ...existing...
    core.CtrlNewFX: ColorYellow,
}

var ControllerLabels = map[string]string{
    // ...existing...
    core.CtrlNewFX: "NewFX",
}
```

---

## Handler Method Patterns

### ACK-based Command (most common)

```go
func (h *Handler) cmdFeature(args []string) {
    if !h.E.RequireArgs(args, 1, "feature <value>") {
        return
    }
    value := engine.Atoi(args[0])
    if value < 0 || value > 255 {
        h.E.Out.Error("Value must be 0-255")
        return
    }
    h.E.Ack(h.E.API.XxxFx.Feature(byte(value)),
        fmt.Sprintf("Feature set to %d", value))
}
```

### Subcommand Pattern

```go
func (h *Handler) cmdSmoke(args []string) {
    if !h.E.RequireArgs(args, 2, "smoke heat on|off") {
        return
    }
    if strings.ToLower(args[0]) != "heat" {
        h.E.Out.Error("Usage: smoke heat on|off")
        return
    }
    on := engine.ParseBool(args[1])
    h.E.Ack(h.E.API.GunFx.SmokeHeat(on),
        fmt.Sprintf("Smoke heater %s", engine.OnOff(on)))
}
```

### Query Command (returns data)

```go
func (h *Handler) cmdQuery(args []string) {
    res := h.E.API.XxxFx.QuerySomething()
    if !res.OK() {
        h.E.Out.Error("Query failed: %s", res.Error)
        return
    }
    data := res.Payload
    if len(data) < 4 {
        h.E.Out.Error("Short response")
        return
    }
    value := binary.LittleEndian.Uint16(data[0:2])
    h.E.Out.Data("Value", "%d", value)
}
```

---

## Output Methods

```go
// Success message (green)
h.E.Out.OK("Operation completed")

// Error message (red)
h.E.Out.Error("Something went wrong: %v", err)

// Info message (cyan)
h.E.Out.Info("Informational message")

// Warning message (yellow)
h.E.Out.Warn("Warning message")

// Labeled data value
h.E.Out.Data("Label", "value %d", n)

// Section header
h.E.Out.Header("Section Title")

// Raw formatted output
h.E.Out.Printf("  Custom output: %d\n", value)

// ACK helper (sends command, prints OK or NACK error)
h.E.Ack(apiResult, "Success message")
```

---

## Verification

```bash
# Build check
cd app/go && go build ./cli/

# Run CLI
app/go/scalefx-cli.exe -p COM5

# Test commands (direct)
> connect
> init
> help                    # Verify new commands appear
> trigger on 100          # Test GunFX command (when connected to GunFX)

# Test commands (hub slave routing)
> connect                 # Connect to HubFX
> init
> slave gfx.trigger on 100   # Routes via SLAVE_ROUTE envelope
> slave gc.deploy all         # Routes to GearControl via hub
```

---

## Checklist

```yaml
Adding_Command:
  - "[ ] Packet type constant added to protocol/xxxfx/xxxfx.go"
  - "[ ] Command builder added to protocol/xxxfx/xxxfx.go"
  - "[ ] API method added to api/xxxfx.go"
  - "[ ] CmdEntry added to handler commands() map"
  - "[ ] Handler method implemented with RequireArgs + validation"
  - "[ ] Response parser added (if query command)"
  - "[ ] Async handler registered (if async response)"
  - "[ ] go build ./cli/ passes"
  - "[ ] Command appears in 'help'"
  - "[ ] Command executes correctly"

Adding_Controller:
  - "[ ] Protocol package created: protocol/newfx/"
  - "[ ] Controller type constant added to protocol/core/core.go"
  - "[ ] API client created: api/newfx.go"
  - "[ ] Handler package created: engine/handlers/newfx/"
  - "[ ] Handler registered in engine/handlers/handlers.go"
  - "[ ] Controller detection added to handlers/core/handler.go"
  - "[ ] Controller maps updated in engine/types.go"
  - "[ ] Status parser registered"
  - "[ ] go build ./cli/ passes"
  - "[ ] Commands appear after init with controller"
```