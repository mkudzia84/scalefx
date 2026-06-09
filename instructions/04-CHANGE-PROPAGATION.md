# Change Propagation Checklist

> **Status:** active reference &middot; **Read when:** verifying every file is updated after a protocol/command change
> **TL;DR:** the protocol lives in two synchronized halves (firmware policy + Go mirror); this doc is the per-change-type matrix of which files to touch, with `cd app/go && go build ./...` as the primary sync check.

> **REFERENCE DOCUMENT:** Use this to verify all affected files are updated after any change.

The protocol lives in two synchronized halves: the firmware policy (`controllers/hubfx/esp32s3/src/effects/<mod>/` for effects, `controllers/lib/sfx_board/` for roles/infra) and the Go mirror (`app/go/protocol/<mod>/<mod>.go` — the **source of truth**). Never change one side alone (Rules 1, 2, 19).

`<mod>` below is the effect/subsystem name (gunfx, gear, landing, audio, engine, storage, config, roles, …).

---

## Quick Matrix

```yaml
Change_Type_Matrix:
  New_Command:
    effects/<mod>/<mod>_protocol.h:   REQUIRED   # packet const (+ error code if any)
    effects/<mod>/<mod>_service.{h,ipp}: REQUIRED # ownsType() + handle() case
    protocol/<mod>/<mod>.go:          REQUIRED   # packet const + CmdXxx builder (mirror)
    client/<mod>.go:                  REQUIRED   # typed method (sendExpectACK/sendForResp)
    console/cmd_<mod>.go:             REQUIRED   # self-registering CLI command
    protocol/<mod>/<mod>.go (DecodeXxx): IF_QUERY # decoder for a typed RESP
    CLAUDE.md dispatch map:           REQUIRED   # claim the new byte
    README (board, if any):           IF_PRESENT

  Modify_Command_Payload:
    effects/<mod>/<mod>_service.{h,ipp}: REQUIRED
    protocol/<mod>/<mod>.go:          REQUIRED   # update CmdXxx / DecodeXxx
    client/<mod>.go:                  IF_SIGNATURE_CHANGED
    console/cmd_<mod>.go:             IF_SIGNATURE_CHANGED
    FIRMWARE_VERSION:                 REQUIRED   # MAJOR if field type/size changed, MINOR if appended (Rule 11)

  New_Error_Code:
    effects/<mod>/<mod>_protocol.h:   REQUIRED   # constant in the module's error block + getMessage()
    protocol/<mod>/<mod>.go:          REQUIRED   # same-valued mirror + name map
    (error_collisions_test must stay green — one namespace per range, CLAUDE.md)

  Bug_Fix_In_Handler:
    effects/<mod>/<mod>_service.ipp:  REQUIRED
    others:                           NO

  New_Service_Policy / New_Board:
    see: 02-NEW-CONTROLLER.md
```

> **CRITICAL:** ALWAYS update the Go side when packet types/commands change — `cd app/go && go build ./...` is the primary protocol-sync check.

---

## Detailed Checklists

### Adding a New Command

```yaml
Step_1_Firmware:
  location: "controllers/hubfx/esp32s3/src/effects/<mod>/  (effects)
             or controllers/lib/sfx_board/  (roles/infra)"
  actions:
    - file: "<mod>_protocol.h"
      action: |
        1. Add packet const at the next free byte (validate vs CLAUDE.md dispatch map
           AND grep the real ownsType() predicates — the map drifts)
        2. Add error code in the module's error block + getMessage() case (if needed)
    - file: "<mod>_service.h / .ipp"
      action: |
        1. Add the byte to ownsType()
        2. Add the case to handle() — SFX_REQUIRE_LEN + SFX_VALIDATE, do work,
           return Ack()/Nack(err)  (or send a typed RESP + return Handled for a query)

Step_2_Go_Source_Of_Truth:
  location: "app/go/protocol/<mod>/<mod>.go"
  action: |
    1. Add packet const (same value as firmware)
    2. Add CmdXxx builder (protocol.BuildPacket(..., 0))
    3. Add DecodeXxx (if the command returns a typed RESP)
    4. Mirror any new error const + name map

Step_3_Go_Typed_API:
  location: "app/go/client/<mod>.go"
  action: "Add a method wrapping sendExpectACK (instant) or sendForResp (query)"

Step_4_Go_CLI:
  location: "app/go/console/cmd_<mod>.go"
  action: |
    1. register(&command{Name, Usage, Help, Category, RequiresConn, RequiresCap, Run}) in init()
    2. Implement the cmd* func: requireClient, parse args, call the typed client API

Step_5_Documentation:
  location: "CLAUDE.md / .github/copilot-instructions.md"
  action: "Append the new byte to the dispatch map (Rule 0). Board README row if applicable."
```

### Modifying an Existing Command

```yaml
Backward_Compatibility:
  rule: "Rule 11 — append-only. New optional fields default to a no-op; the firmware
         checks `len` to detect them. Old masters keep working."
  breaking (→ bump MAJOR + update both sides simultaneously):
    - "Changing a field's type/size (u16→u32)"
    - "Reordering fields"
    - "Removing a field or packet type"
    - "Changing the semantic meaning of an existing field"
Files:
  - "<mod>_service.ipp           — update the handle() case"
  - "protocol/<mod>/<mod>.go     — update CmdXxx / DecodeXxx"
  - "client/<mod>.go + console/cmd_<mod>.go — only if the method signature changed"
  - "FIRMWARE_VERSION + README version history"
```

### Adding a New Error Code

```yaml
Firmware: "<mod>_protocol.h — constant in the module's error block (per CLAUDE.md ranges) + getMessage() case"
Go:       "protocol/<mod>/<mod>.go — same-valued constant + register in the error name map"
Guard:    "tests/host/go_unit/error_collisions_test asserts no two modules register the
           same code under different names — keep every error in its CLAUDE.md range"
```

### Creating a New Service Policy / Board

> **Full guide:** [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md)

```yaml
Summary:
  Firmware:
    - "[ ] Define the *ServicePolicy (SystemServicePolicy concept): kCapabilityBits, begin, ownsType, handle, update"
    - "[ ] <mod>_protocol.h with packet + error constants (claim bytes vs dispatch map)"
    - "[ ] Compose the policy into the hub's BoardServer<…> pack (or declare a BoardOf<…> board)"
  Go:
    - "[ ] protocol/<mod>/<mod>.go (constants + Cmd/Decode)"
    - "[ ] client/<mod>.go typed API"
    - "[ ] console/cmd_<mod>.go self-registering commands"
  Docs:
    - "[ ] Dispatch map in CLAUDE.md, board README"
```

---

## File Location Reference

```yaml
Firmware:
  Board_framework:  "controllers/lib/sfx_board/server/  (board_server.h, board_of.h,
                     board_service.h, port_service.h, role_service.h, role_registry.h,
                     effect_clock.h)"
  Wire_protocol:    "controllers/lib/sfx_serial/serial/  (core/core.h, wire.h, packet_reader.h,
                     diag_log.h, client/bus.h, client/result_queue.h)"
  Roles:            "controllers/lib/sfx_board/roles/  (servo_actuator_role.h, heater_role.h, …)"
  Hub_effects:      "controllers/hubfx/esp32s3/src/effects/<mod>/  (<mod>_protocol.h,
                     <mod>_service.h/.ipp, …)"
  Hub_entry:        "controllers/hubfx/esp32s3/src/hubfx_esp32s3.cpp  (BoardServer<…> pack + setup)"
  Pico_boards:      "controllers/{lightfx,gearcontrol}/pico/src/*_pico.ino"
  Platform:         "controllers/lib/sfx_platform/platform/sfx_platform.h"
  Audio / Storage / Config: "controllers/lib/sfx_audio/  sfx_storage/  sfx_config/"

Go (app/go/):
  protocol/wire.go:            "CRC-8/16, COBS, BuildPacket / ParsePacket, endian helpers"
  protocol/connection.go:      "Serial connection, tag-correlated send/recv, async filters (Rules 53,56)"
  protocol/core/core.go:       "Core packet types, error codes, capability flags, controller-type strings"
  protocol/<mod>/<mod>.go:     "Per-module wire mirror — packet consts, CmdXxx, DecodeXxx, errors (SOURCE OF TRUTH)"
  protocol/roles/*.go:         "Role codecs (opaque-forwarded by the hub, Rule 58)"
  client/client.go:            "Client — owns the Connection + every typed sub-API"
  client/<mod>.go:             "Typed API (Gun, Gear, Landing, Audio, Storage, Topology, …)"
  client/roletarget.go:        "RoleTarget — GUID-transparent role drive/query (Rule 58)"
  client/events.go:            "Async telemetry stream (OnRole / OnXxx subscriptions)"
  console/registry.go:         "Central command registry (register() in each cmd_*.go init())"
  console/cmd_<mod>.go:        "Self-registering CLI commands for one wire-domain"
  console/session.go:          "REPL / dispatch loop"

Archived (do NOT reference): "app/go/api/  app/go/engine/  engine/handlers/<mod>/  — removed 2026-05-28"
```

---

## Verification Commands

```bash
# Build firmware (via scalefx-flash; see the scalefx-flash skill)
app/go/scalefx-flash.exe build hubfx       --no-clean
app/go/scalefx-flash.exe build lightfx     --no-clean
app/go/scalefx-flash.exe build gearcontrol --no-clean

# Go protocol-sync check + tools build
cd app/go && go build ./...

# Pre-merge gate (unit + integration vs connected HubFX + hub build); see scalefx-test skill
./tools/run-tests.ps1 -Premerge

# CLI smoke test (scalefx-cli skill)
app/go/scalefx-cli.exe -p COM5
```

---

## Common Errors and Fixes

```yaml
Error: "Command silently NACKs MISSING_PARAM (collision) — the byte you added is swallowed"
  Cause: "Another policy's ownsType() already claims the byte; first-owner-wins, your handle() never runs"
  Fix:   "Grep the real ownsType() predicates; append at a truly-free byte (CLAUDE.md dispatch map drifts)"

Error: "go build fails: undefined CmdXxx / type mismatch"
  Cause: "protocol mirror not updated, or signature drift between client and protocol"
  Fix:   "Update protocol/<mod>/<mod>.go first (source of truth), then client + console"

Error: "Wrong error NAME shown for a code"
  Cause: "Two modules registered the same error code under different names (Go init() last-wins)"
  Fix:   "Keep every error in its CLAUDE.md range; run error_collisions_test"

Error: "Values appear swapped/corrupted"
  Cause: "Endianness mismatch"
  Fix:   "C++: SfxWire::getU16LE/putU16LE   Go: binary.LittleEndian.*"

Error: "CLI command not appearing"
  Cause: "Not registered, or RequiresCap gates it off for this board"
  Fix:   "Verify register(...) in init(); check the board advertises the RequiresCap bit"
```

---

## Version Bump Checklist

```yaml
When_Releasing:
  Firmware:
    - "[ ] Update FIRMWARE_VERSION (MAJOR wire-breaking / MINOR additive / PATCH logic-only)"
    - "[ ] BUILD_NUMBER auto-increments on flash — trust the INIT_READY buildNum, not the source"
    - "[ ] Release notes from git log of the controller dir + lib/ (Rule 22)"
```
