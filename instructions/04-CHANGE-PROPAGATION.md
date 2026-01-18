# Change Propagation Checklist

> **REFERENCE DOCUMENT:** Use this to verify all affected files are updated after any change.

---

## Quick Matrix

```yaml
Change_Type_Matrix:
  New_Command:
    serial_protocol.h: REQUIRED
    serial_error.h: IF_NEW_ERRORS
    serial_xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    packets.py: REQUIRED
    commands.py: REQUIRED
    interactive.py: REQUIRED
    README.md: REQUIRED
    
  Modify_Command_Payload:
    serial_protocol.h: NO
    serial_error.h: IF_NEW_ERRORS
    serial_xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    packets.py: IF_CONSTANTS_CHANGED
    commands.py: REQUIRED
    interactive.py: REQUIRED
    README.md: REQUIRED
    
  New_Error_Code:
    serial_protocol.h: NO
    serial_error.h: REQUIRED
    serial_xxxfx.h: NO
    xxxfx_pico.ino: MAYBE
    packets.py: REQUIRED
    commands.py: NO
    interactive.py: NO
    README.md: REQUIRED
    
  Bug_Fix_In_Handler:
    serial_protocol.h: NO
    serial_error.h: NO
    serial_xxxfx.h: MAYBE
    xxxfx_pico.ino: REQUIRED
    packets.py: NO
    commands.py: NO
    interactive.py: NO
    README.md: NO
    
  New_Controller:
    serial_protocol.h: REQUIRED
    serial_error.h: REQUIRED
    serial_newfx.h: CREATE_NEW
    newfx_pico.ino: CREATE_NEW
    packets.py: REQUIRED
    commands.py: REQUIRED
    interactive.py: REQUIRED
    README.md: CREATE_NEW
```

---

## Detailed Checklists

### Adding a New Command

```yaml
Step_1_Serial_Library:
  location: "controllers/lib/serial/"
  actions:
    - file: "serial_protocol.h"
      action: "Add packet type constant in module namespace"
      example: "constexpr uint8_t COMMAND_NAME = 0xNN;"
    
    - file: "serial_error.h"
      action: "Add error codes if command can fail with new reasons"
      condition: "IF new error scenarios exist"
    
    - file: "serial_xxxfx.h"
      action: |
        1. Add callback typedef
        2. Add onXxx() registration method
        3. Add case in handle() switch
        4. Add private callback member

Step_2_Controller_Firmware:
  location: "controllers/xxxfx/pico/src/"
  actions:
    - file: "xxxfx_pico.ino"
      action: |
        1. Add handler function with signature matching callback
        2. Register callback in setup()
        3. Add any required state variables

Step_3_Python_Framework:
  location: "tests/"
  actions:
    - file: "framework/packets.py"
      action: |
        1. Add packet constant to XxxPacket class
        2. Add error constants to XxxError class (if any)
        3. Update name() function for new errors
    
    - file: "framework/commands.py"
      action: "Add static method to XxxCommands class"
    
    - file: "xxxfx/test_<feature>.py"
      action: "Create test file with at least:"
      tests:
        - "test_valid_parameters"
        - "test_edge_cases"
        - "test_missing_parameters"
        - "test_invalid_parameters"

Step_4_CLI:
  location: "tests/cli/"
  actions:
    - file: "interactive.py"
      action: |
        1. Add CommandInfo to xxxfx_commands dict
        2. Add handler method cmd_xxxfx_<name>()
        3. Add import if needed

Step_5_Documentation:
  location: "controllers/xxxfx/pico/"
  actions:
    - file: "README.md"
      action: |
        1. Add row to protocol table
        2. Document payload format
        3. Add any new error codes
```

---

### Modifying an Existing Command

```yaml
Backward_Compatibility:
  questions:
    - "Will existing masters work with new slaves?"
    - "Will existing slaves work with new masters?"
    - "Is the payload format changing?"
  
  if_breaking_change:
    - "Increment major version"
    - "Document migration path"
    - "Consider deprecation period"

Files_To_Update:
  - file: "serial_xxxfx.h"
    changes:
      - "Update handle() case"
      - "Update callback typedef if signature changed"
  
  - file: "xxxfx_pico.ino"
    changes:
      - "Update handler function"
  
  - file: "commands.py"
    changes:
      - "Update command builder method"
  
  - file: "test_*.py"
    changes:
      - "Update test assertions"
      - "Add tests for new behavior"
  
  - file: "interactive.py"
    changes:
      - "Update CLI handler"
      - "Update usage string"
  
  - file: "README.md"
    changes:
      - "Update payload format in table"
      - "Update version history"
```

---

### Adding a New Error Code

```yaml
Serial_Library:
  - file: "serial_error.h"
    action: |
      1. Add constant in appropriate namespace
      2. Add case to name() function

Python_Framework:
  - file: "packets.py"
    action: |
      1. Add constant to XxxError class
      2. Add case to name() static method

Documentation:
  - file: "README.md"
    action: "Add to error code table"

Optional:
  - file: "test_*.py"
    action: "Add test that triggers the new error"
```

---

### Creating a New Controller

> **Full guide:** [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md)

```yaml
Summary_Checklist:
  Serial_Library:
    - "[ ] Reserve packet type range (0x60-0xEF available)"
    - "[ ] Create serial_newfx.h"
    - "[ ] Add error namespace to serial_error.h"
    - "[ ] Update serial.h umbrella include"
  
  Controller:
    - "[ ] Create directory structure"
    - "[ ] Create platformio.ini"
    - "[ ] Create newfx_pico.ino"
    - "[ ] Create scripts/build_and_flash.py"
  
  Python:
    - "[ ] Add NewFxPacket class to packets.py"
    - "[ ] Add NewFxError class to packets.py"
    - "[ ] Add NewFxCommands class to commands.py"
    - "[ ] Create tests/newfx/ directory"
    - "[ ] Create test files"
  
  CLI:
    - "[ ] Add CTRL_NEWFX constant"
    - "[ ] Create newfx_commands registry"
    - "[ ] Update get_available_commands()"
    - "[ ] Update _parse_init_ready()"
    - "[ ] Add handler methods"
  
  Documentation:
    - "[ ] Create README.md with full protocol docs"
```

---

## File Location Reference

```yaml
C++_Serial_Library:
  path: "controllers/lib/serial/"
  files:
    serial.h: "Umbrella header"
    serial_protocol.h: "Packet type constants"
    serial_error.h: "Error code constants"
    serial_command_handler.h: "ICommandHandler, CommandRouter"
    serial_gunfx.h: "GunFxSlave, GunFxMaster"
    serial_lightfx.h: "LightFxSlave, LightFxMaster"

Controller_Firmware:
  pattern: "controllers/{name}/pico/"
  files:
    "src/{name}_pico.ino": "Main firmware"
    "platformio.ini": "Build configuration"
    "scripts/build_and_flash.py": "Automated build/flash"
    "README.md": "Protocol documentation"

Python_Framework:
  path: "tests/"
  files:
    "framework/__init__.py": "Public exports"
    "framework/connection.py": "ScaleFXConnection"
    "framework/protocol.py": "COBS, CRC, packet helpers"
    "framework/packets.py": "Constants (mirror C++)"
    "framework/commands.py": "Command builders"
    "cli/interactive.py": "Interactive CLI"
    "conftest.py": "pytest fixtures"
```

---

## Verification Commands

```bash
# Build verification
cd controllers/gunfx/pico && pio run
cd controllers/lightfx/pico && pio run
cd controllers/noop/pico && pio run

# Python syntax
python -m py_compile tests/framework/packets.py
python -m py_compile tests/framework/commands.py
python -m py_compile tests/cli/interactive.py

# Run tests (requires hardware)
pytest tests/gunfx/ -v
pytest tests/lightfx/ -v
pytest tests/noop/ -v

# CLI smoke test
python -m tests.cli.interactive
```

---

## Common Errors and Fixes

```yaml
Error: "Unknown command type"
  Cause: "Packet constant mismatch between C++ and Python"
  Fix:
    - "Verify serial_protocol.h constant value"
    - "Verify packets.py constant value"
    - "Ensure they are identical"

Error: "NACK with MISSING_PARAMETER"
  Cause: "Payload too short"
  Fix:
    - "Check handle() length validation"
    - "Check Python struct.pack format string"
    - "Count bytes: B=1, H=2, I=4"

Error: "CLI command not appearing"
  Cause: "Command not in registry or wrong controller type"
  Fix:
    - "Verify command added to xxxfx_commands dict"
    - "Verify controller detection in _parse_init_ready()"
    - "Verify get_available_commands() includes it"

Error: "Values appear swapped/corrupted"
  Cause: "Endianness mismatch"
  Fix:
    - "C++: val = payload[0] | (payload[1] << 8)  // little-endian"
    - "Python: struct.pack('<H', val)  // < = little-endian"
```

---

## Version Bump Checklist

```yaml
When_Releasing:
  Controller_Firmware:
    - "[ ] Update FIRMWARE_VERSION define"
    - "[ ] Increment BUILD_NUMBER"
    - "[ ] Update version history in README.md"
  
  Serial_Library:
    condition: "IF library changed"
    actions:
      - "[ ] Update version in README"
      - "[ ] Update library.json"
  
  Python_Framework:
    condition: "IF Python code changed"
    actions:
      - "[ ] Update version in __init__.py (if applicable)"
```
