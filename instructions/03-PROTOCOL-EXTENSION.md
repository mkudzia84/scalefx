# Protocol Extension Guide

> **ACTION DOCUMENT:** Step-by-step guide for adding a command to an existing hub effect / subsystem.

A command lives in exactly one `*ServicePolicy`'s `handle()` on the firmware side and is mirrored down the Go stack. The wire format is owned by `app/go/protocol/<mod>/<mod>.go` (the **source of truth** for the master protocol).

---

## Quick Reference

```yaml
Files_To_Modify:
  Firmware:
    - "controllers/hubfx/esp32s3/src/effects/<mod>/<mod>_protocol.h"   # packet type + error codes
    - "controllers/hubfx/esp32s3/src/effects/<mod>/<mod>_service.h/.ipp" # ownsType() + handle() case
  Go (source of truth → typed API → CLI):
    - "app/go/protocol/<mod>/<mod>.go"   # packet const + CmdXxx builder + DecodeXxx (mirror)
    - "app/go/client/<mod>.go"           # typed method (sendExpectACK / sendForResp)
    - "app/go/console/cmd_<mod>.go"      # self-registering CLI command
  Docs:
    - "CLAUDE.md dispatch map"           # claim the new byte at the next free value
```

A library role/infra command follows the same shape but the firmware policy lives under `controllers/lib/sfx_board/` and the role codec under `app/go/protocol/roles/`.

---

## Example: Adding `GUN_AMMO_SET` to GunFX

### Step 1: Claim the packet byte (firmware protocol header)

**File:** `controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_protocol.h`

GunFX owns `0xCC–0xD2` + `0xE2–0xE5`. Pick the next free value **after validating against the dispatch map in `CLAUDE.md` AND grepping the real `ownsType()` predicates** (the map drifts). Add the constant and, if needed, an error code:

```cpp
namespace GunPacket {
    // …existing…
    constexpr uint8_t GUN_AMMO_SET = 0xE6;   // [id:u8][count:u16LE]  — next free
}

namespace GunError {
    // …existing…
    constexpr uint8_t AMMO_OUT_OF_RANGE = 0x36;   // in the GunError 0x30-0x3F block
    // add to getMessage(): case AMMO_OUT_OF_RANGE: return "Ammo count out of range";
}
```

### Step 2: Own + handle it (firmware service policy)

**File:** `gunfx_service.h` — add the byte to `ownsType()`:

```cpp
bool ownsType(uint8_t type) const {
    return type == GunPacket::GUN_FIRE_ONCE
        // …existing…
        || type == GunPacket::GUN_AMMO_SET;   // ← add
}
```

**File:** `gunfx_service.ipp` — add the case to `handle()`. Length-check + validate with the `SFX_*` macros, do the work, return an ACK/NACK (Category 1: Instant). For a query, send a typed RESP and `return CommandHandleResult::Handled` instead (see § Response Category below).

```cpp
case GunPacket::GUN_AMMO_SET: {
    SFX_REQUIRE_LEN(3);
    uint8_t  id    = payload[0];
    uint16_t count = SfxWire::getU16LE(payload + 1);
    SFX_VALIDATE(count <= 9999, GunError::AMMO_OUT_OF_RANGE);
    GunUnit* g = findById(id);
    SFX_VALIDATE(g != nullptr, GunError::UNKNOWN_ID);
    g->setAmmo(count);
    return CommandHandleResult::Ack();
}
```

> `SFX_REQUIRE_LEN` / `SFX_VALIDATE` are in `serial/core/core.h`; `SfxWire::getU16LE` / `putU16LE` are the endian-safe accessors. See [01-ARCHITECTURE.md](01-ARCHITECTURE.md) § Handler Macros.

### Step 3: Mirror the wire format (Go source of truth)

**File:** `app/go/protocol/gunfx/gunfx.go` — add the packet constant, the `CmdXxx` builder, and (for queries) a `DecodeXxx`. This file mirrors the firmware protocol header byte-for-byte (Rule 2).

```go
const AmmoSet protocol.PacketType = 0xE6  // [id:u8][count:u16LE]

// CmdAmmoSet builds a GUN_AMMO_SET packet.  (tag 0 — Connection re-tags on send.)
func CmdAmmoSet(id byte, count uint16) []byte {
    body := make([]byte, 3)
    body[0] = id
    binary.LittleEndian.PutUint16(body[1:], count)
    return protocol.BuildPacket(AmmoSet, body, 0)
}
```

If you added an error code, mirror it (same value) in the module's Go error constants + name map so `error_collisions_test` stays green.

### Step 4: Typed client method (Go API)

**File:** `app/go/client/gunfx.go` — a thin wrapper over `sendExpectACK` (instant) or `sendForResp` (query):

```go
// SetAmmo sets the ammo count on gun `id` (0-9999).
func (g *Gun) SetAmmo(id byte, count uint16) error {
    return g.c.sendExpectACK(gunfx.CmdAmmoSet(id, count))
}
```

### Step 5: CLI command (self-registering)

**File:** `app/go/console/cmd_gun.go` — register in `init()`, implement the `cmd*` func calling the typed API:

```go
func init() {
    // …existing…
    register(&command{Name: "gun-ammo", Usage: "gun-ammo <id> <count>",
        Help: "set ammo count (0-9999)", Category: catGun,
        RequiresConn: true, RequiresCap: core.CapGunFx, Run: cmdGunAmmo})
}

func cmdGunAmmo(a *App, args []string) error {
    if err := a.requireClient(); err != nil { return err }
    if len(args) != 2 { return fmt.Errorf("usage: gun-ammo <id> <count>") }
    id, err := parseU8(args[0]); if err != nil { return err }
    count, err := strconv.ParseUint(args[1], 0, 16); if err != nil { return err }
    if err := a.c.Gun.SetAmmo(id, uint16(count)); err != nil { return err }
    Ok("gun[%d] ammo = %d", id, count)
    return nil
}
```

See [07-CLI-UPDATES.md](07-CLI-UPDATES.md) for the full CLI pattern.

### Step 6: Documentation

Append the new byte to the dispatch map in `CLAUDE.md` / `.github/copilot-instructions.md` (Rule 0 — docs are code). Add a protocol-table row in the relevant board README if it has one.

---

## Validation

```bash
# 1. Build firmware
app/go/scalefx-flash.exe build hubfx --no-clean

# 2. Compile the Go side — the primary protocol-sync check
cd app/go && go build ./...

# 3. CLI smoke test (see the scalefx-cli skill)
app/go/scalefx-cli.exe -p COM5
# > connect
# > gun-ammo 0 500
```

---

## Common Payload Patterns

```yaml
Single_Uint8:
  cpp: "uint8_t v = payload[0];"
  go:  "body := []byte{v}"
Single_Uint16:
  cpp: "uint16_t v = SfxWire::getU16LE(payload);"
  go:  "binary.LittleEndian.PutUint16(body, v)"
Single_Uint32:
  cpp: "uint32_t v = SfxWire::getU32LE(payload);"
  go:  "binary.LittleEndian.PutUint32(body, v)"
Mixed [id:u8][value:u16LE]:
  cpp: |
    uint8_t  id = payload[0];
    uint16_t v  = SfxWire::getU16LE(payload + 1);
  go: |
    body := make([]byte, 3)
    body[0] = id
    binary.LittleEndian.PutUint16(body[1:], v)
```

Everything is little-endian (Rule 4).

---

## Response Category Decision

The category is decided in the policy's `handle()`. See [01-ARCHITECTURE.md](01-ARCHITECTURE.md) § Three Response Categories.

```
Q1: Does the command complete before handle() returns?
├─ YES → Q2: Does the response carry data beyond ACK?
│         ├─ NO  → Category 1 INSTANT  — return Ack() / Nack(err); Go: sendExpectACK
│         └─ YES → Category 2 QUERY    — send typed RESP, return Handled; Go: sendForResp
└─ NO  → Category 3 LONG-RUNNING — return Ack() now, finish across update() ticks;
          completion arrives via STATUS broadcast or an async event (Go events stream)
```

### Category 2 (Query) — firmware sends a typed RESP

```cpp
case GunPacket::GUN_STATUS_REQ:
    handleStatusReq();                       // builds + sends GUN_STATUS_RESP (echoes tag)
    return CommandHandleResult::Handled;     // no ACK — the RESP IS the ack
```
```go
resp, err := g.c.sendForResp(gunfx.CmdStatusReq(), gunfx.StatusResp)
// then gunfx.DecodeStatus(resp.Payload)
```

### Category 3 (Long-Running)

`handle()` ACKs immediately ("accepted") and the operation runs across `update()` ticks. Don't block. Surface completion via the STATUS tail or an async event packet that the Go `events.go` stream decodes (never through a blocking command result).

---

## Checklist

```yaml
Before_Marking_Complete:
  Firmware:
    - [ ] Packet const in <mod>_protocol.h (next free byte, validated vs dispatch map)
    - [ ] Error code in the module's error block (if any) + getMessage() case
    - [ ] Byte added to ownsType()
    - [ ] Case added to handle() with SFX_REQUIRE_LEN / SFX_VALIDATE
  Go:
    - [ ] Packet const + CmdXxx builder (+ DecodeXxx if query) in protocol/<mod>/<mod>.go
    - [ ] Error const mirrored same-valued (if any)
    - [ ] Typed method in client/<mod>.go (sendExpectACK / sendForResp)
    - [ ] CLI command registered in console/cmd_<mod>.go init() + cmd* func
  Build:
    - [ ] scalefx-flash build hubfx --no-clean passes
    - [ ] cd app/go && go build ./... passes
  Docs:
    - [ ] Dispatch map in CLAUDE.md updated
```
