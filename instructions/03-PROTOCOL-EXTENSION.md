# Protocol Extension Guide

> **ACTION DOCUMENT:** Step-by-step guide for adding commands to an existing controller.

---

## Quick Reference

```yaml
Files_To_Modify:
  Always:
    - "lib/serial/serial_protocol.h"     # Packet type constant
    - "lib/serial/serial_xxxfx.h"        # Handler logic
    - "controllers/xxxfx/pico/src/*.ino" # Callback implementation
    - "tests/framework/packets.py"       # Python constant
    - "tests/framework/commands.py"      # Python builder
    - "tests/cli/interactive.py"         # CLI command
    - "controllers/xxxfx/pico/README.md" # Documentation
  
  If_New_Error_Codes:
    - "lib/serial/serial_error.h"        # C++ error constant
    - "tests/framework/packets.py"       # Python error constant
```

---

## Example: Adding AMMO_SET to GunFX

### Step 1: Define Packet Type

**File:** `controllers/lib/serial/serial_protocol.h`

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

**File:** `controllers/lib/serial/serial_error.h`

**FIND:**
```cpp
namespace GunFxError {
    // ...existing...
```

**ADD:**
```cpp
    constexpr uint8_t AMMO_INVALID      = 0x15;
    
    // Also update name() function:
    case AMMO_INVALID: return "AMMO_INVALID";
```

---

### Step 3: Add Callback to Slave Class

**File:** `controllers/lib/serial/serial_gunfx.h`

**ACTION 3.1 - Add callback typedef:**
```cpp
using AmmoSetCallback = std::function<uint8_t(uint16_t count)>;
```

**ACTION 3.2 - Add registration method:**
```cpp
void onAmmoSet(AmmoSetCallback cb) { _onAmmoSet = cb; }
```

**ACTION 3.3 - Add case in handle():**
```cpp
case GunFxPacket::AMMO_SET:
    if (len < 2) {
        result = SerialError::MISSING_PARAMETER;
    } else if (_onAmmoSet) {
        uint16_t count = payload[0] | (payload[1] << 8);
        result = _onAmmoSet(count);
    }
    break;
```

**ACTION 3.4 - Add private member:**
```cpp
private:
    AmmoSetCallback _onAmmoSet;
```

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
gunfxSlave.onAmmoSet(handleAmmoSet);
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

**File:** `tests/cli/interactive.py`

**ACTION 8.1 - Add to gunfx_commands registry:**
```python
'gunfx.ammo': (self.cmd_gunfx_ammo, CommandInfo(
    'gunfx.ammo', 'gunfx.ammo set <count>',
    'Set ammunition count (0-9999)',
    requires_init=True, controller=self.CTRL_GUNFX)),
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
cd controllers/gunfx/pico
pio run

# 2. Check Python syntax
python -m py_compile tests/framework/packets.py
python -m py_compile tests/framework/commands.py
python -m py_compile tests/cli/interactive.py

# 3. Flash and test
python scripts/build_and_flash.py
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
  - [ ] Packet type added to serial_protocol.h
  - [ ] Error codes added to serial_error.h (if any)
  - [ ] Callback type defined in serial_xxxfx.h
  - [ ] Registration method added to slave class
  - [ ] Handler case added to handle() switch
  - [ ] Private callback member added
  - [ ] Callback implemented in firmware
  - [ ] Callback registered in setup()
  - [ ] Python packet constant added
  - [ ] Python error constants added (if any)
  - [ ] Python command builder added
  - [ ] Test file created
  - [ ] CLI command added to registry
  - [ ] CLI handler method added
  - [ ] README.md updated with protocol entry
  - [ ] All compile checks pass
```
