# Protocol Extension Guide

> **ACTION DOCUMENT:** Step-by-step guide for adding commands to an existing controller.

---

## Quick Reference

```yaml
Files_To_Modify:
  Always:
    - "lib/sfx_serial/serial/xxxfx/xxxfx.h"            # Packet type, error codes, handler logic
    - "controllers/xxxfx/pico/src/*.ino"     # Callback implementation
    - "app/go/protocol/xxxfx/types.go"       # Go packet/error constants
    - "app/go/protocol/xxxfx/commands.go"    # Go command builder
    - "app/go/engine/handlers/handler_xxxfx.go" # Go CLI command
    - "app/win32/ScaleFXSerial/PacketTypes.cs"  # C# packet constant
    - "app/win32/ScaleFXSerial/Commands/XxxFxCommands.cs" # C# command builder
    - "controllers/xxxfx/pico/README.md"     # Documentation
  
  If_New_Error_Codes:
    - "lib/sfx_serial/serial/xxxfx/xxxfx.h"             # C++ error constant (in module's error namespace)
    - "app/go/protocol/xxxfx/types.go"       # Go error constant
    - "app/win32/ScaleFXSerial/ErrorCodes.cs" # C# error constant
```

---

## Example: Adding AMMO_SET to GunFX

### Step 1: Define Packet Type

**File:** `controllers/lib/sfx_serial/serial/gunfx/gunfx.h`

**FIND:**
```cpp
namespace GunFxPacket {
    constexpr uint8_t TRIGGER_ON        = 0x01;
    constexpr uint8_t TRIGGER_OFF       = 0x02;
    // ...existing...
```

**ADD:**
```cpp
    constexpr uint8_t AMMO_SET          = 0x25;  // Set ammo count
```

---

### Step 2: Add Error Code (if needed)

**File:** `controllers/lib/sfx_serial/serial/gunfx/gunfx.h`

Error codes are defined in the module's own header file, not a separate error file.

**FIND:**
```cpp
namespace GunFxError {
    // ...existing error constants...
```

**ADD:**
```cpp
    constexpr uint8_t AMMO_INVALID      = 0x25;
    
    // Also update getMessage() function:
    case AMMO_INVALID: return "AMMO_INVALID";
```

---

### Step 3: Add Callback to Server Class

**File:** `controllers/lib/sfx_serial/serial/gunfx/gunfx.h`

**ACTION 3.1 - Add callback typedef:**
```cpp
using AmmoSetCallback = std::function<uint8_t(uint16_t count)>;
```

**ACTION 3.2 - Add registration method:**
```cpp
void onAmmoSet(AmmoSetCallback cb) { _onAmmoSet = cb; }
```

**ACTION 3.3 - Add case in handleModulePacket() using SFX_* macros:**

The `BusServer` base class handles `tryProcess()` by checking the packet type against
`moduleRangeLow()..moduleRangeHigh()` and delegating to `handleModulePacket()`. You only
need to add the case in `handleModulePacket()`:

```cpp
case GunFxPacket::AMMO_SET: {
    SFX_REQUIRE_LEN(2);
    uint16_t count = CoreProtocol::getU16LE(payload);
    SFX_VALIDATE(GunFxSpec::isValidAmmoCount(count), GunFxError::AMMO_INVALID);
    SFX_DISPATCH(_onAmmoSet, count);
}
```

> **Note:** `SFX_REQUIRE_LEN`, `SFX_VALIDATE`, and `SFX_DISPATCH` macros are defined in `core/core.h`. See `01-ARCHITECTURE.md` for details.

**ACTION 3.4 - Add private member:**
```cpp
private:
    AmmoSetCallback _onAmmoSet;
```

---

### Step 3b: Add Client Method (for HubFX)

**File:** `controllers/lib/sfx_serial/serial/gunfx/gunfx.h` (GunFxClient class)

All client methods MUST return `CommandResult`, never `bool`. Choose the pattern based on the response category (see Response Category Decision below).

**For Category 1 (Instant) — this example:**
```cpp
CommandResult ammoSet(uint16_t count) {
    uint8_t payload[2];
    CoreProtocol::putU16LE(payload, count);
    return sendCommand(GunFxPacket::AMMO_SET, payload, 2);
}
```

> **Note:** For Category 2 (Query) commands, you also need to add `onModulePacket()` handling that resolves the tag. For Category 3 (Long-Running), consider whether the tag should be resolved on a progress/completion packet. See the Response Category Decision section at the end of this document.

---

### Step 4: Implement in Firmware

**File:** `controllers/gunfx/pico/src/gunfx_pico.ino`

**ACTION 4.1 - Add state variable:**
```cpp
uint16_t ammo_count = 0;
```

**ACTION 4.2 - Add handler function:**
```cpp
uint8_t handleAmmoSet(uint16_t count) {
    if (count > 9999) {
        return GunFxError::AMMO_INVALID;
    }
    ammo_count = count;
    return GunFxError::OK;
}
```

**ACTION 4.3 - Register in setup():**
```cpp
gunfxServer.onAmmoSet(handleAmmoSet);
```

---

### Step 5: Add Go CLI Support

**File:** `app/go/protocol/gunfx/types.go`

**ADD packet constant:**
```go
AmmoSet = 0x25
```

**ADD error constant (if defined):**
```go
ErrAmmoInvalid = 0x15
```

Also update `PacketTypeName()` switch and error name maps.

**File:** `app/go/protocol/gunfx/commands.go`

**ADD command builder:**
```go
func AmmoSet(count uint16) []byte {
    payload := make([]byte, 2)
    binary.LittleEndian.PutUint16(payload, count)
    return payload
}
```

**File:** `app/go/engine/handlers/handler_gunfx.go`

Add CLI command handler and register in command list.

---

### Step 6: Add C# Library Support

**File:** `app/win32/ScaleFXSerial/PacketTypes.cs`

**ADD:**
```csharp
public const byte AmmoSet = 0x25;
```

**File:** `app/win32/ScaleFXSerial/ErrorCodes.cs` (if new errors):
```csharp
public const byte GunFxAmmoInvalid = 0x15;
```

**File:** `app/win32/ScaleFXSerial/Commands/GunFxCommands.cs`

**ADD:**
```csharp
public static byte[] AmmoSet(ushort count)
{
    var payload = new byte[2];
    Endian.PutU16LE(payload, 0, count);
    return CoreCommands.BuildPacket(GunFxPacketTypes.AmmoSet, payload);
}
```

---

### Step 7: Update Documentation

**File:** `controllers/gunfx/pico/README.md`

**ADD to protocol table:**
```markdown
| 0x25 | AMMO_SET | `[count:u16]` | Set ammunition count |
```

**ADD to error codes:**
```markdown
| 0x15 | AMMO_INVALID | Ammo count exceeds 9999 |
```

---

## Validation

```bash
# 1. Build firmware
app/go/scalefx-flash.exe build gunfx --no-clean

# 2. Build Go CLI
cd app/go && go build ./cli/

# 3. Build C# library
dotnet build app/win32/ScaleFXSerial/

# 4. CLI smoke test
app/go/scalefx-cli.exe -p COM5
# > init
# > help          (verify gunfx.ammo appears)
# > gunfx.ammo set 100
```

---

## Common Payload Patterns

```yaml
No_Payload:
  cpp: "// No payload parsing needed"
  go: "payload := []byte{}"

Single_Uint8:
  cpp: "uint8_t val = payload[0];"
  go: "payload := []byte{val}"

Single_Uint16:
  cpp: "uint16_t val = payload[0] | (payload[1] << 8);"
  go: "binary.LittleEndian.PutUint16(payload, val)"

Single_Uint32:
  cpp: "uint32_t val = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);"
  go: "binary.LittleEndian.PutUint32(payload, val)"

Mixed_Types:
  example: "[channel:u8][brightness:u8][duration:u16]"
  cpp: |
    uint8_t channel = payload[0];
    uint8_t brightness = payload[1];
    uint16_t duration = payload[2] | (payload[3] << 8);
  go: |
    payload := make([]byte, 4)
    payload[0] = channel
    payload[1] = brightness
    binary.LittleEndian.PutUint16(payload[2:], duration)

String:
  cpp: |
    char name[len + 1];
    memcpy(name, payload, len);
    name[len] = '\0';
  go: "payload := []byte(name)"
```

---

## Checklist

```yaml
Before_Marking_Complete:
  Server_Side:
    - [ ] Packet type added to xxxfx/xxxfx.h (e.g., GunFxPacket namespace)
    - [ ] Error codes added to xxxfx/xxxfx.h (e.g., GunFxError namespace, if any)
    - [ ] Callback type defined in xxxfx/xxxfx.h
    - [ ] Registration method added to server class
    - [ ] Handler case added to handleModulePacket() switch using SFX_* macros
    - [ ] Private callback member added
    - [ ] Validation added to XxxFxSpec namespace (if needed)
    - [ ] Callback implemented in firmware
    - [ ] Callback registered in setup()
  
  Client_Side:
    - [ ] Client method added returning CommandResult (never bool)
    - [ ] Response category determined (instant / query / long-running)
    - [ ] "IF query: response packet type defined"
    - [ ] "IF query: onModulePacket() resolves tag (implicit ACK)"
    - [ ] "IF long-running: completion signal documented"

  Go_CLI_and_CSharp:
    - [ ] Go packet constant added
    - [ ] Go error constants added (if any)
    - [ ] Go command builder added
    - [ ] Go CLI command added to handler_xxxfx.go
    - [ ] Go CLI handler method added
    - [ ] C# packet constant added to PacketTypes.cs
    - [ ] C# error constants added to ErrorCodes.cs (if any)
    - [ ] C# command builder added to Commands/XxxFxCommands.cs
  
  Documentation:
    - [ ] README.md updated with protocol entry
    - [ ] Response category noted in protocol table
    - [ ] All compile checks pass
```

---

## Response Category Decision

When adding a new command, determine which response category it belongs to. This affects both server handler and client implementation. See `01-ARCHITECTURE.md` § Client Response Handling Design for full details.

### Decision Flow

```
Q1: Does the command complete before the server handler returns?
├─ YES → Q2: Does the response carry data beyond ACK?
│         ├─ NO  → Category 1: INSTANT (use SFX_DISPATCH → auto ACK)
│         └─ YES → Category 2: QUERY (send data response, client resolves tag as implicit ACK)
└─ NO  → Category 3: LONG-RUNNING (SFX_DISPATCH → immediate ACK, monitor via STATUS/async)
```

### Category 1: Instant Command (most common)

Use `SFX_DISPATCH` in server `handleModulePacket()`. Client gets ACK automatically.

**Server:** Standard `SFX_DISPATCH` pattern (ACK/NACK handled by macro).
**Client:** Just call `sendCommand()` — tag resolved automatically by `BusClient::handlePacket()`.

```cpp
// Server
case XxxPacket::MY_CMD: {
    SFX_REQUIRE_LEN(2);
    SFX_DISPATCH(_myCallback, payload[0], payload[1]);
}

// Client
CommandResult myCommand(uint8_t a, uint8_t b) {
    uint8_t payload[2] = { a, b };
    return sendCommand(XxxPacket::MY_CMD, payload, 2);
}
```

### Category 2: Query Command (data response)

Server sends a typed response packet. Client MUST resolve the tag in `onModulePacket()`.

**Server:** Handle manually (no `SFX_DISPATCH`), call a `sendXxxResponse()` method.
**Client:** Override `onModulePacket()`, parse data, fire callback, resolve tag.

```cpp
// Server — handleModulePacket()
case XxxPacket::QUERY_STATUS:
    if (len >= 1 && _queryCallback) {
        MyStatusData data;
        _queryCallback(payload[0], data);         // Fill data via firmware
        sendQueryResponse(data);                  // Send response with _currentTag
    }
    return CommandHandleResult::Handled;          // No SFX_DISPATCH!

// Client — onModulePacket() MUST resolve tag
case XxxPacket::QUERY_STATUS_RESP:
    if (len >= expectedLen) {
        // Parse, fire callback
    }
    if (tag != CoreProtocol::TAG_ASYNC) {
        _lastCommandResult = CommandResult::Ack();
        _resultQueue.resolve(tag, _lastCommandResult);  // CRITICAL
    }
    break;
```

### Category 3: Long-Running Command

Server sends immediate ACK, operation runs asynchronously. Client monitors via STATUS or async packets.

**Server:** Standard `SFX_DISPATCH` (immediate ACK). Optionally store the tag for progress updates.
**Client:** `sendCommand()` returns quickly. Use STATUS polling or async callbacks to detect completion.

```cpp
// Server — stores tag for async progress packets
case XxxPacket::LONG_OP: {
    SFX_REQUIRE_LEN(1);
    _operationTag = _currentTag;                  // Store for progress updates
    SFX_DISPATCH(_longOpCallback, payload[0]);    // Immediate ACK
}

// Client — optionally resolve tag on final progress packet
case XxxPacket::LONG_OP_PROGRESS:
    if (len >= expectedLen) {
        // Parse progress, fire callback
        if (progress.finished && tag != CoreProtocol::TAG_ASYNC) {
            _resultQueue.resolve(tag, CommandResult::Ack());
        }
    }
    break;
```
