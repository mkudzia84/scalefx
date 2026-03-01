# Change Propagation Checklist

> **REFERENCE DOCUMENT:** Use this to verify all affected files are updated after any change.

---

## Quick Matrix

```yaml
Change_Type_Matrix:
  New_Command:
    serial_core.h: IF_NEW_PACKET_TYPE
    serial_error.h: IF_NEW_ERRORS
    serial_xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    packets.py: REQUIRED
    commands.py: REQUIRED
    test_*.py: REQUIRED          # ALWAYS add tests for new commands
    cli/handlers/xxxfx.py: REQUIRED
    README.md: REQUIRED
    
  Modify_Command_Payload:
    serial_core.h: NO
    serial_error.h: IF_NEW_ERRORS
    serial_xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    packets.py: IF_CONSTANTS_CHANGED
    commands.py: REQUIRED
    test_*.py: REQUIRED          # ALWAYS update tests for payload changes
    cli/handlers/xxxfx.py: REQUIRED
    README.md: REQUIRED
    
  New_Error_Code:
    serial_core.h: NO
    serial_error.h: REQUIRED
    serial_xxxfx.h: NO
    xxxfx_pico.ino: MAYBE
    packets.py: REQUIRED
    commands.py: NO
    test_*.py: RECOMMENDED       # Test error conditions
    cli/handlers/xxxfx.py: NO
    README.md: REQUIRED
    
  Bug_Fix_In_Handler:
    serial_core.h: NO
    serial_error.h: NO
    serial_xxxfx.h: MAYBE
    xxxfx_pico.ino: REQUIRED
    packets.py: NO
    commands.py: NO
    test_*.py: REQUIRED          # Add regression test for the fix
    cli/handlers/xxxfx.py: NO
    README.md: NO
    
  New_Controller:
    serial_core.h: REQUIRED
    serial_error.h: REQUIRED
    serial_newfx.h: CREATE_NEW
    newfx_pico.ino: CREATE_NEW
    packets.py: REQUIRED
    commands.py: REQUIRED
    test_*.py: REQUIRED          # Create tests/newfx/ directory
    cli/handlers/newfx.py: CREATE_NEW
    cli/interactive.py: REQUIRED  # Register new handler
    README.md: CREATE_NEW
```

> **CRITICAL:** Tests are not optional. ALWAYS update tests when protocol is changed or new features are added.

> **CRITICAL:** ALWAYS update the CLI handler (`tests/cli/handlers/xxxfx.py`) when new commands are added. The CLI is the primary tool for manual testing and debugging.

---

## Detailed Checklists

### Adding a New Command

```yaml
Step_1_Serial_Library:
  location: "controllers/lib/serial/"
  actions:
    - file: "serial_core.h"
      action: "Add packet type constant in module namespace (if not already defined in serial_xxxfx.h)"
      example: "constexpr uint8_t COMMAND_NAME = 0xNN;"
    
    - file: "serial_error.h"
      action: "Add error codes if command can fail with new reasons"
      condition: "IF new error scenarios exist"
    
    - file: "serial_xxxfx.h"
      action: |
        1. Add packet type constant in XxxFxPacket namespace
        2. Add callback typedef
        3. Add onXxx() registration method
        4. Add case in tryProcess() switch using SFX_* macros
        5. Add private callback member
        6. Add validation to XxxFxSpec namespace if needed

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
    - file: "handlers/xxxfx.py"
      action: |
        1. Add command method to handler class
        2. Add CommandInfo to get_commands() dict
        3. Add import for new command builder if needed

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
    - "Will existing clients work with new servers?"
    - "Will existing servers work with new clients?"
    - "Is the payload format changing?"
  
  if_breaking_change:
    - "Increment major version"
    - "Document migration path"
    - "Consider deprecation period"

Files_To_Update:
  - file: "serial_xxxfx.h"
    changes:
      - "Update tryProcess() case (SFX_* macros)"
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
  
  - file: "cli/handlers/xxxfx.py"
    changes:
      - "Update CLI handler method"
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
    - "[ ] Register coreServer AND newfxServer with commandRouter (both required!)"
  
  Python:
    - "[ ] Add NewFxPacket class to packets.py"
    - "[ ] Add NewFxError class to packets.py"
    - "[ ] Add NewFxCommands class to commands.py"
    - "[ ] Create tests/newfx/ directory"
    - "[ ] Create test files"
  
  CLI:
    - "[ ] Create tests/cli/handlers/newfx.py with CommandHandlerBase subclass"
    - "[ ] Register handler in tests/cli/interactive.py"
    - "[ ] Add controller detection in handlers/core.py"
  
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
    serial_core.h: "Packet types, CoreProtocol, CoreCommandServer, SFX_* macros"
    serial_error.h: "Error code constants"
    serial_command_handler.h: "ICommandHandler, CommandRouter"
    serial_gunfx.h: "GunFxServer, GunFxClient, GunFxSpec"
    serial_lightfx.h: "LightFxServer, LightFxClient, LightFxSpec"

Controller_Firmware:
  pattern: "controllers/{name}/pico/"
  files:
    "src/{name}_pico.ino": "Main firmware"
    "platformio.ini": "Build configuration"
    "README.md": "Protocol documentation"

Scripts:
  path: "scripts/"
  files:
    "build_and_flash.py": "Centralized build/flash for all Pico controllers"

Python_Framework:
  path: "tests/"
  files:
    "framework/__init__.py": "Public exports"
    "framework/connection.py": "ScaleFXConnection"
    "framework/protocol.py": "COBS, CRC, packet helpers"
    "framework/packets.py": "Constants (mirror C++)"
    "framework/commands.py": "Command builders"
    "cli/base.py": "CommandInfo, OutputMixin, ControllerType, base classes"
    "cli/parsers.py": "Response packet parsing utilities"
    "cli/interactive.py": "Main CLI class (composes handlers)"
    "cli/handlers/core.py": "Core/protocol commands"
    "cli/handlers/gunfx.py": "GunFX commands"
    "cli/handlers/lightfx.py": "LightFX commands"
    "conftest.py": "pytest fixtures"
```

---

## Verification Commands

```bash
# Build verification
python -m platformio run -e pico -d controllers/gunfx/pico
python -m platformio run -e pico -d controllers/lightfx/pico
python -m platformio run -e pico -d controllers/noop/pico

# Build and flash (centralized script)
python scripts/build_and_flash.py gunfx
python scripts/build_and_flash.py lightfx
python scripts/build_and_flash.py noop

# Python syntax
python -m py_compile tests/framework/packets.py
python -m py_compile tests/framework/commands.py
python -m py_compile tests/cli/interactive.py
python -m py_compile tests/cli/handlers/gunfx.py
python -m py_compile tests/cli/handlers/lightfx.py

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
Error: "INIT returns NACK INVALID_COMMAND"
  Cause: "coreServer not registered with commandRouter"
  Fix:
    - "Ensure commandRouter.addHandler(&coreServer) is called BEFORE module handler"
    - "Both coreServer and xxxfxServer must be added to the router"

Error: "Unknown command type"
  Cause: "Packet constant mismatch between C++ and Python"
  Fix:
    - "Verify serial_core.h / serial_xxxfx.h constant value"
    - "Verify packets.py constant value"
    - "Ensure they are identical"

Error: "NACK with MISSING_PARAMETER"
  Cause: "Payload too short"
  Fix:
    - "Check tryProcess() SFX_REQUIRE_LEN() value"
    - "Check Python struct.pack format string"
    - "Count bytes: B=1, H=2, I=4"

Error: "CLI command not appearing"
  Cause: "Command not in handler or wrong controller type"
  Fix:
    - "Verify command added to get_commands() in handlers/xxxfx.py"
    - "Verify controller detection in handlers/core.py"
    - "Verify handler registered in interactive.py"

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
