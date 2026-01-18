# ScaleFX Agent Instructions

> **FOR AI AGENTS:** Start here. These instructions are optimized for automated code generation and modification.

## Quick Navigation

```yaml
Task: "Add new command to existing controller"
  → Read: 03-PROTOCOL-EXTENSION.md
  → Checklist: 04-CHANGE-PROPAGATION.md#adding-a-new-command

Task: "Create new controller type"
  → Read: 02-NEW-CONTROLLER.md
  → Checklist: 04-CHANGE-PROPAGATION.md#creating-a-new-controller

Task: "Fix bug in protocol handler"
  → Read: 01-ARCHITECTURE.md (understand pattern)
  → Checklist: 04-CHANGE-PROPAGATION.md#modifying-an-existing-command

Task: "Run/write tests"
  → Read: 06-TEST-SUITE.md

Task: "Build and deploy firmware"
  → Read: 05-BUILD-AND-FLASH.md

Task: "Update CLI"
  → Read: 07-CLI-UPDATES.md
```

---

## Critical Constants

```yaml
Protocol:
  format: "Binary COBS with CRC-8"
  crc_polynomial: 0x07
  baud_rate: 115200
  packet_structure: "[type:u8][len:u8][payload:0-64][crc8:u8]"
  endianness: "little-endian"

Packet_Ranges:
  - range: "0x01-0x2F"
    module: "GunFX"
    status: "USED"
  - range: "0x30-0x3F"
    module: "Reserved"
    status: "RESERVED"
  - range: "0x40-0x5F"
    module: "LightFX"
    status: "USED"
  - range: "0x60-0xEF"
    module: "Available"
    status: "FREE"
  - range: "0xF0-0xFF"
    module: "Core"
    status: "RESERVED"

Controllers:
  - id: "gunfx"
    path: "controllers/gunfx/pico/"
    packet_range: "0x01-0x2F"
  - id: "lightfx"
    path: "controllers/lightfx/pico/"
    packet_range: "0x40-0x5F"
  - id: "noop"
    path: "controllers/noop/pico/"
    packet_range: "CORE_ONLY"
```

---

## File Location Index

```yaml
Serial_Library:
  root: "controllers/lib/serial/"
  files:
    - name: "serial.h"
      purpose: "Umbrella header (include this)"
    - name: "serial_protocol.h"
      purpose: "Packet type constants, CoreProtocol class"
      modify_when: "Adding packet types"
    - name: "serial_error.h"
      purpose: "Error codes for all modules"
      modify_when: "Adding error codes"
    - name: "serial_command_handler.h"
      purpose: "ICommandHandler interface, CommandRouter"
      modify_when: "Never (stable interface)"
    - name: "serial_gunfx.h"
      purpose: "GunFxSlave, GunFxMaster classes"
      modify_when: "Adding GunFX commands"
    - name: "serial_lightfx.h"
      purpose: "LightFxSlave, LightFxMaster classes"
      modify_when: "Adding LightFX commands"

Python_Framework:
  root: "tests/"
  files:
    - name: "framework/packets.py"
      purpose: "Packet constants (must mirror serial_protocol.h)"
      modify_when: "Adding packet types or error codes"
    - name: "framework/commands.py"
      purpose: "Command builder functions"
      modify_when: "Adding commands"
    - name: "framework/connection.py"
      purpose: "ScaleFXConnection class"
      modify_when: "Rarely"
    - name: "cli/interactive.py"
      purpose: "Interactive CLI"
      modify_when: "Adding commands or controllers"

Controllers:
  pattern: "controllers/{name}/pico/"
  files:
    - name: "src/{name}_pico.ino"
      purpose: "Main firmware file"
    - name: "platformio.ini"
      purpose: "Build configuration"
    - name: "scripts/build_and_flash.py"
      purpose: "Automated build/flash"
    - name: "README.md"
      purpose: "Protocol documentation"
```

---

## Decision Trees

### When Adding a Command

```
START: Need to add command to controller

Q1: Is packet type constant defined?
├─ NO → Add to serial_protocol.h in correct namespace
└─ YES → Continue

Q2: Are new error codes needed?
├─ YES → Add to serial_error.h in module namespace
│        Add to tests/framework/packets.py
└─ NO → Continue

Q3: Is callback type defined in serial_xxxfx.h?
├─ NO → Add callback typedef
│        Add registration method: void onXxx(Callback cb)
│        Add private member: Callback _onXxx
│        Add case in handle() switch
└─ YES → Continue

Q4: Is command implemented in firmware?
├─ NO → Add callback implementation in xxxfx_pico.ino
│        Register callback in setup()
└─ YES → Continue

Q5: Is Python command builder defined?
├─ NO → Add to tests/framework/commands.py
│        Add packet constant to tests/framework/packets.py
└─ YES → Continue

Q6: Is test written?
├─ NO → Add test file in tests/xxxfx/
└─ YES → Continue

Q7: Is CLI updated?
├─ NO → Add to tests/cli/interactive.py
│        - Add CommandInfo to registry
│        - Add handler method
└─ YES → Continue

Q8: Is documentation updated?
├─ NO → Update controllers/xxxfx/pico/README.md
└─ YES → DONE
```

---

## Mandatory File Sync Points

**CRITICAL:** These file pairs must stay in sync:

```yaml
Sync_Groups:
  - name: "Packet Constants"
    primary: "lib/serial/serial_protocol.h"
    mirrors:
      - "tests/framework/packets.py"
    rule: "Same values, same names (snake_case in Python)"

  - name: "Error Codes"
    primary: "lib/serial/serial_error.h"
    mirrors:
      - "tests/framework/packets.py"
    rule: "Same values, same names"

  - name: "Command Interface"
    primary: "lib/serial/serial_xxxfx.h"
    mirrors:
      - "tests/framework/commands.py"
      - "tests/cli/interactive.py"
    rule: "Python must expose same commands"
```

---

## Common Patterns

### Callback Registration Pattern (C++)

```cpp
// In serial_xxxfx.h
using CommandCallback = std::function<uint8_t(uint16_t param1, uint8_t param2)>;

void onCommand(CommandCallback cb) { _onCommand = cb; }

bool handle(uint8_t type, const uint8_t* payload, uint8_t len) override {
    if (type == XxxPacket::COMMAND && _onCommand) {
        uint16_t p1 = payload[0] | (payload[1] << 8);  // little-endian
        uint8_t p2 = payload[2];
        uint8_t err = _onCommand(p1, p2);
        (err == 0) ? sendAck() : sendNack(err);
        return true;
    }
    return false;
}

private:
    CommandCallback _onCommand;
```

### Command Builder Pattern (Python)

```python
# In commands.py
@staticmethod
def command_name(param1: int, param2: int) -> bytes:
    """Build COMMAND_NAME packet."""
    payload = struct.pack('<HB', param1, param2)  # little-endian
    return build_packet(XxxPacket.COMMAND_NAME, payload)
```

### CLI Handler Pattern

```python
# In interactive.py
'xxx.command': (self.cmd_xxx_command, CommandInfo(
    'xxx.command', 'xxx.command <p1> <p2>',
    'Description here', requires_init=True, controller=self.CTRL_XXX)),

def cmd_xxx_command(self, args: List[str]):
    if len(args) < 2:
        self.print_error("Usage: xxx.command <p1> <p2>")
        return
    try:
        p1, p2 = int(args[0]), int(args[1])
        packet = XxxCommands.command_name(p1, p2)
        success, response = self.conn.send_expect_ack(packet)
        self.print_response(response)
    except ValueError:
        self.print_error("Invalid parameters")
```

---

## Validation Checklist

Before completing any task, verify:

```yaml
Compilation:
  - [ ] "pio run" succeeds for modified controller
  - [ ] No Python syntax errors: "python -m py_compile <file>"

Constants_Match:
  - [ ] Packet types in C++ match Python
  - [ ] Error codes in C++ match Python
  - [ ] Endianness consistent (little-endian)

Tests_Pass:
  - [ ] Existing tests still pass
  - [ ] New functionality has test coverage

CLI_Works:
  - [ ] New commands appear in "help"
  - [ ] Commands execute correctly

Documentation:
  - [ ] README.md updated with new protocol entries
  - [ ] Payload format documented
  - [ ] Error codes documented
```

---

## Document Index

| Doc | When to Use |
|-----|-------------|
| [01-ARCHITECTURE.md](01-ARCHITECTURE.md) | Understand system design, packet format, class hierarchy |
| [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) | Create entirely new controller type |
| [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) | Add commands to existing controller |
| [04-CHANGE-PROPAGATION.md](04-CHANGE-PROPAGATION.md) | Ensure all affected files updated |
| [05-BUILD-AND-FLASH.md](05-BUILD-AND-FLASH.md) | Build firmware, flash to device |
| [06-TEST-SUITE.md](06-TEST-SUITE.md) | Run tests, add test coverage |
| [07-CLI-UPDATES.md](07-CLI-UPDATES.md) | Update interactive CLI |
