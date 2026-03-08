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
  baud_rate: 1000000
  packet_structure: "[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]"
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
  - range: "0x60-0x7F"
    module: "GearControl"
    status: "USED"
  - range: "0x80-0xEF"
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
  - id: "gearcontrol"
    path: "controllers/gearcontrol/pico/"
    packet_range: "0x60-0x7F"
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
    - name: "serial_core.h"
      purpose: "CoreProtocol (COBS/CRC/endian), SerialError, CommandResult, ICommandHandler, CommandRouter, SFX_* macros"
      modify_when: "Adding generic error codes, handler macros, or modifying core protocol"
    - name: "serial_core.cpp"
      purpose: "CoreProtocol implementations, CorePayload encode/decode"
      modify_when: "Rarely — protocol-level changes only"
    - name: "serial_bus_server.h"
      purpose: "BusServer base class + CoreCommandServer"
      modify_when: "Rarely — base class for all server handlers"
    - name: "serial_bus_server.cpp"
      purpose: "BusServer + CoreCommandServer implementations"
      modify_when: "Rarely"
    - name: "serial_bus_client.h"
      purpose: "BusClient base class (extends SerialBus)"
      modify_when: "Rarely — base class for all client controllers"
    - name: "serial_bus_client.cpp"
      purpose: "BusClient implementation"
      modify_when: "Rarely"
    - name: "serial_bus.h"
      purpose: "SerialBus (client-only, COBS over USB CDC)"
      modify_when: "Never (stable transport layer)"
    - name: "serial_result_queue.h"
      purpose: "ResultQueue — tag-correlated command/response matching"
      modify_when: "Never (stable infrastructure)"
    - name: "serial_gunfx.h"
      purpose: "GunFxServer, GunFxClient, GunFxPacket, GunFxError, GunFxSpec"
      modify_when: "Adding GunFX commands or error codes"
    - name: "serial_lightfx.h"
      purpose: "LightFxServer, LightFxClient, LightFxPacket, LightFxError"
      modify_when: "Adding LightFX commands or error codes"
    - name: "serial_gearcontrol.h"
      purpose: "GearControlServer, GearControlClient, GearControlPacket, GearControlError"
      modify_when: "Adding GearControl commands or error codes"

Python_Framework:
  root: "tests/"
  files:
    - name: "framework/packets.py"
      purpose: "Packet constants (must mirror serial_core.h)"
      modify_when: "Adding packet types or error codes"
    - name: "framework/commands.py"
      purpose: "Command builder functions"
      modify_when: "Adding commands"
    - name: "framework/connection.py"
      purpose: "ScaleFXConnection class"
      modify_when: "Rarely"
    - name: "cli/base.py"
      purpose: "CommandInfo, OutputMixin, ControllerType, base classes"
      modify_when: "Adding controller types or output helpers"
    - name: "cli/parsers.py"
      purpose: "Response packet parsing utilities"
      modify_when: "Adding response packet types"
    - name: "cli/interactive.py"
      purpose: "Main CLI class (composes handlers)"
      modify_when: "Adding controllers or core features"
    - name: "cli/handlers/core.py"
      purpose: "Core/protocol commands (connect, init, status)"
      modify_when: "Modifying core commands"
    - name: "cli/handlers/gunfx.py"
      purpose: "GunFX commands (trigger, servo, smoke)"
      modify_when: "Adding GunFX CLI commands"
    - name: "cli/handlers/lightfx.py"
      purpose: "LightFX commands (led, servo, power)"
      modify_when: "Adding LightFX CLI commands"
    - name: "cli/handlers/gearcontrol.py"
      purpose: "GearControl commands (gear, servo, yaw)"
      modify_when: "Adding GearControl CLI commands"

Controllers:
  pattern: "controllers/{name}/pico/"
  files:
    - name: "src/{name}_pico.ino"
      purpose: "Main firmware file"
    - name: "platformio.ini"
      purpose: "Build configuration"
    - name: "README.md"
      purpose: "Protocol documentation"

Scripts:
  root: "scripts/"
  files:
    - name: "build_and_flash.py"
      purpose: "Centralized build/flash for all Pico controllers"
      usage: "python scripts/build_and_flash.py <controller>"
```

---

## Decision Trees

### When Adding a Command

```
START: Need to add command to controller

Q1: Is packet type constant defined?
├─ NO → Add to serial_xxxfx.h in correct namespace (e.g., GunFxPacket in serial_gunfx.h)
└─ YES → Continue

Q2: What response category? (See 03-PROTOCOL-EXTENSION.md § Response Category Decision)
├─ INSTANT → Server uses SFX_DISPATCH, client gets auto-ACK
├─ QUERY   → Server sends data response, client resolves tag in onModulePacket()
└─ LONG-RUNNING → Server sends immediate ACK, client monitors via STATUS/async

Q3: Are new error codes needed?
├─ YES → Add to serial_xxxfx.h in module error namespace (e.g., GunFxError)
│        Add to tests/framework/packets.py
└─ NO → Continue

Q4: Is callback type defined in serial_xxxfx.h?
├─ NO → Add callback typedef
│        Add registration method: void onXxx(Callback cb)
│        Add private member: Callback _onXxx
│        Add case in handleModulePacket() switch using SFX_* macros
└─ YES → Continue

Q5: Is client method defined in serial_xxxfx.h?
├─ NO → Add method returning CommandResult (NEVER bool)
│        IF QUERY: add response type + onModulePacket() tag resolution
│        IF LONG-RUNNING: document completion signal
└─ YES → Continue

Q6: Is command implemented in firmware?
├─ NO → Add callback implementation in xxxfx_pico.ino
│        Register callback in setup()
└─ YES → Continue

Q7: Is Python command builder defined?
├─ NO → Add to tests/framework/commands.py
│        Add packet constant to tests/framework/packets.py
└─ YES → Continue

Q8: Is test written?
├─ NO → Add test file in tests/xxxfx/
└─ YES → Continue

Q9: Is CLI updated?
├─ NO → Add to tests/cli/handlers/xxxfx.py
│        - Add command method to handler class
│        - Add CommandInfo to get_commands()
└─ YES → Continue

Q10: Is documentation updated?
├─ NO → Update controllers/xxxfx/pico/README.md
└─ YES → DONE
```

---

## Mandatory File Sync Points

**CRITICAL:** These file pairs must stay in sync:

```yaml
Sync_Groups:
  - name: "Packet Constants"
    primary: "lib/serial/serial_core.h"
    mirrors:
      - "tests/framework/packets.py"
    rule: "Same values, same names (snake_case in Python)"

  - name: "Error Codes (Generic)"
    primary: "lib/serial/serial_core.h (SerialError namespace)"
    mirrors:
      - "tests/framework/packets.py (CoreError class)"
    rule: "Same values, same names"

  - name: "Error Codes (Module)"
    primary: "lib/serial/serial_xxxfx.h (XxxError namespace)"
    mirrors:
      - "tests/framework/packets.py (XxxError class)"
    rule: "Same values, same names"

  - name: "Command Interface"
    primary: "lib/serial/serial_xxxfx.h"
    mirrors:
      - "tests/framework/commands.py"
      - "tests/cli/handlers/xxxfx.py"
    rule: "Python must expose same commands"
```

---

## Common Patterns

### Indicator LED Standard

All Pico server controllers use identical indicator LED behavior on GP13/GP14, managed automatically by `PicoServer` via `IndicatorLedManager`:

```yaml
Indicator_LEDs:
  LED_0:
    pin: GP13
    purpose: "Connection status"
    states:
      waiting: "Blink 500ms (power on, no INIT yet)"
      connected: "Solid ON"
      lost: "OFF (watchdog triggered)"
  LED_1:
    pin: GP14
    purpose: "Error status"
    states:
      normal: "OFF"
      error: "Blink 200ms (module-specific error detected)"

PicoServer_Pattern:
  setup: "server.begin('XxxFX', FIRMWARE_VERSION, BUILD_NUMBER)  // LEDs auto-initialized"
  loop: "server.indicators().setErrorCondition(hasError)  // Set before server.loop()"
  auto: "server.loop()  // Updates indicators automatically"
```

**Error condition (LED 1) varies by controller:**
- **GunFX:** No runtime error conditions (LED 1 always OFF)
- **LightFX:** No runtime error conditions (LED 1 always OFF)
- **GearControl:** Any gear in ERROR state → LED 1 blinks

### Rich STATUS Pattern

Every controller provides board-specific status via `PicoServer`:

```cpp
// In setup(): Register module status callback
server.core().onStatusData([](uint8_t* buf, size_t maxLen) -> size_t {
    buf[0] = myFlag;
    CoreProtocol::putU16LE(&buf[1], myServo);
    return 3;  // bytes written
});

// Free RAM is updated automatically by server.loop()
```

STATUS response = 12-byte core header + module callback data.
See PROTOCOL.md for wire format per controller.

### Client Response Handling Pattern

All client methods return `CommandResult`, never `bool`. Three response categories determine how tags are resolved:

```yaml
Response_Categories:
  Instant:
    server: "SFX_DISPATCH → ACK/NACK"
    client: "sendCommand() — tag auto-resolved by BusClient::handlePacket()"
    example: "ledBrightness(), servoMove(), triggerOn()"

  Query:
    server: "Custom response packet (no SFX_DISPATCH)"
    client: "sendCommand() + onModulePacket() resolves tag manually"
    contract: "parse → fire callback → resolve tag (if tag != TAG_ASYNC)"
    example: "ledSeqStatus(), ledStatus(), requestStatus()"

  Long_Running:
    server: "SFX_DISPATCH → immediate ACK (command accepted)"
    client: "sendCommand() returns ACK quickly, poll STATUS or await async for completion"
    example: "gearDeploy(), gearCalibrate(), landingLightDeploy()"
```

**`onModulePacket()` template for query/long-running responses:**
```cpp
case XxxPacket::RESPONSE_TYPE:
    if (len >= expectedLen) {
        // 1. Parse data
        // 2. Fire callback
    }
    if (tag != CoreProtocol::TAG_ASYNC) {
        _lastCommandResult = CommandResult::Ack();
        _resultQueue.resolve(tag, _lastCommandResult);  // implicit ACK
    }
    break;
```

See `01-ARCHITECTURE.md` § Client Response Handling Design for complete details.

### Callback Registration Pattern (C++)

```cpp
// In serial_xxxfx.h - case in handleModulePacket() using SFX_* macros
case XxxPacket::COMMAND: {
    SFX_REQUIRE_LEN(3);
    uint16_t p1 = CoreProtocol::getU16LE(payload);
    uint8_t p2 = payload[2];
    SFX_VALIDATE(XxxSpec::isValid(p1), XxxError::INVALID_PARAM);
    SFX_DISPATCH(_onCommand, p1, p2);
}
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
# In handlers/xxxfx.py
class XxxFxCommandHandler(CommandHandlerBase):
    def get_commands(self) -> Dict[str, Tuple[Callable, CommandInfo]]:
        return {
            'xxx.command': (self.cmd_xxx_command, CommandInfo(
                'xxx.command', 'xxx.command <p1> <p2>',
                'Description here', requires_init=True)),
        }
    
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

Client_Response_Handling:
  - [ ] All client methods return CommandResult (never bool)
  - [ ] Response category determined (instant / query / long-running)
  - [ ] "IF query: onModulePacket() resolves tag as implicit ACK"
  - [ ] "IF long-running: completion signal documented"

Units_Explicit:  # MANDATORY for all physical measurements
  - [ ] Method names include unit suffix (e.g., busVoltage_mV, current_mA)
  - [ ] Struct fields include unit suffix (e.g., voltage_mV, power_mW)
  - [ ] Wire format comments annotate units at pack/unpack sites
  - [ ] No implicit conversions at call sites
  - [ ] Datasheet section refs on hardware register constants

Tests_Updated:  # ALWAYS update tests when protocol/features change
  - [ ] Existing tests still pass
  - [ ] New functionality has test coverage
  - [ ] Test file created/updated in tests/xxxfx/

CLI_Updated:  # ALWAYS update CLI when new commands are added
  - [ ] New commands added to tests/cli/handlers/xxxfx.py
  - [ ] Commands appear in "help" output
  - [ ] Commands execute correctly

Documentation:
  - [ ] README.md updated with new protocol entries
  - [ ] Payload format documented
  - [ ] Error codes documented

Versioning:  # MANDATORY for firmware changes
  - [ ] BUILD_NUMBER incremented (every firmware change)
  - [ ] FIRMWARE_VERSION bumped if appropriate (semver)
  - [ ] Version history updated in controller README.md
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
