# Protocol Extension Guide

> **ACTION DOCUMENT:** Step-by-step guide for adding commands to an existing controller.

---

## Quick Reference

```yaml
Files_To_Modify:
  Always:
    - "lib/sfx_serial/serial/xxxfx/xxxfx.h"            # Packet type, error codes, handler logic
    - "controllers/xxxfx/pico/src/*.ino"     # Callback implementation
    - "tests/framework/packets.py"           # Python constant
    - "tests/framework/commands.py"          # Python builder
    - "tests/cli/handlers/xxxfx.py"          # CLI command
    - "controllers/xxxfx/pico/README.md"     # Documentation
  
  If_New_Error_Codes:
    - "lib/sfx_serial/serial/xxxfx/xxxfx.h"             # C++ error constant (in module's error namespace)
    - "tests/framework/packets.py"           # Python error constant
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

### Step 5: Add Python Packet Constant

**File:** `tests/framework/packets.py`

**FIND:**
```python
class GunFxPacket:
    """GunFX packet types (0x01-0x2F)"""
    TRIGGER_ON = 0x01
    # ...existing...
```

**ADD:**
```python
    AMMO_SET = 0x25
```

**ALSO ADD error if defined:**
```python
class GunFxError:
    # ...existing...
    AMMO_INVALID = 0x15
```

---

### Step 6: Add Python Command Builder

**File:** `tests/framework/commands.py`

**FIND:**
```python
class GunFxCommands:
    # ...existing methods...
```

**ADD:**
```python
    @staticmethod
    def ammo_set(count: int) -> bytes:
        """Build AMMO_SET packet.
        
        Args:
            count: Ammunition count (0-9999)
        """
        payload = struct.pack('<H', count)  # little-endian uint16
        return build_packet(GunFxPacket.AMMO_SET, payload)
```

---

### Step 7: Add Test

**File:** `tests/gunfx/test_ammo.py` (NEW FILE)

```python
"""GunFX ammo command tests."""
import pytest
from tests.framework import (
    ScaleFXConnection, CommandBuilder, GunFxCommands,
    GunFxPacket, GunFxError, CoreError
)
from tests.framework.protocol import build_packet


class TestAmmoSet:
    """Tests for AMMO_SET command."""
    
    def test_valid_count(self, connection):
        """Test setting valid ammo count."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = GunFxCommands.ammo_set(500)
        success, response = connection.send_expect_ack(packet)
        
        assert success, f"Expected ACK, got: {response}"
    
    def test_zero_count(self, connection):
        """Test setting ammo to zero."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = GunFxCommands.ammo_set(0)
        success, response = connection.send_expect_ack(packet)
        
        assert success
    
    def test_max_count(self, connection):
        """Test setting maximum ammo count."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = GunFxCommands.ammo_set(9999)
        success, response = connection.send_expect_ack(packet)
        
        assert success
    
    def test_invalid_count(self, connection):
        """Test setting ammo count over limit."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = GunFxCommands.ammo_set(10000)
        success, response = connection.send_expect_ack(packet)
        
        assert not success
        assert response.error_code == GunFxError.AMMO_INVALID
    
    def test_missing_payload(self, connection):
        """Test AMMO_SET with empty payload."""
        connection.send_and_wait(CommandBuilder.init())
        
        packet = build_packet(GunFxPacket.AMMO_SET, b'')
        success, response = connection.send_expect_ack(packet)
        
        assert not success
        assert response.error_code == CoreError.MISSING_PARAMETER
```

---

### Step 8: Add CLI Command

**File:** `tests/cli/handlers/gunfx.py`

**ACTION 8.1 - Add to get_commands() dict:**
```python
'gunfx.ammo': (self.cmd_gunfx_ammo, CommandInfo(
    'gunfx.ammo', 'gunfx.ammo set <count>',
    'Set ammunition count (0-9999)',
    requires_init=True)),
```

**ACTION 8.2 - Add handler method:**
```python
def cmd_gunfx_ammo(self, args: List[str]):
    """GunFX ammunition control."""
    if not args or args[0].lower() != 'set':
        self.print_error("Usage: gunfx.ammo set <count>")
        return
    
    if len(args) < 2:
        self.print_error("Usage: gunfx.ammo set <count>")
        return
    
    try:
        count = int(args[1])
        if count < 0 or count > 9999:
            self.print_error("Count must be 0-9999")
            return
        
        packet = GunFxCommands.ammo_set(count)
        success, response = self.conn.send_expect_ack(packet)
        
        if success:
            self.print_success(f"Ammo set to {count}")
        else:
            self.print_response(response)
    except ValueError:
        self.print_error("Invalid count value")
```

---

### Step 9: Update Documentation

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
python -m platformio run -e pico -d controllers/gunfx/pico

# 2. Check Python syntax
python -m py_compile tests/framework/packets.py
python -m py_compile tests/framework/commands.py
python -m py_compile tests/cli/handlers/gunfx.py

# 3. Flash and test
python scripts/build_and_flash.py gunfx
pytest tests/gunfx/test_ammo.py -v

# 4. CLI smoke test
python -m tests.cli.interactive
# > connect
# > init
# > help          (verify gunfx.ammo appears)
# > gunfx.ammo set 100
```

---

## Common Payload Patterns

```yaml
No_Payload:
  cpp: "// No payload parsing needed"
  python: "return build_packet(TYPE, b'')"

Single_Uint8:
  cpp: "uint8_t val = payload[0];"
  python: "payload = struct.pack('<B', val)"

Single_Uint16:
  cpp: "uint16_t val = payload[0] | (payload[1] << 8);"
  python: "payload = struct.pack('<H', val)"

Single_Uint32:
  cpp: "uint32_t val = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);"
  python: "payload = struct.pack('<I', val)"

Mixed_Types:
  example: "[channel:u8][brightness:u8][duration:u16]"
  cpp: |
    uint8_t channel = payload[0];
    uint8_t brightness = payload[1];
    uint16_t duration = payload[2] | (payload[3] << 8);
  python: "payload = struct.pack('<BBH', channel, brightness, duration)"

String:
  cpp: |
    char name[len + 1];
    memcpy(name, payload, len);
    name[len] = '\0';
  python: "payload = name.encode('utf-8')"
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

  Python_and_CLI:
    - [ ] Python packet constant added
    - [ ] Python error constants added (if any)
    - [ ] Python command builder added
    - [ ] Test file created
    - [ ] CLI command added to handlers/xxxfx.py
    - [ ] CLI handler method added
  
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
