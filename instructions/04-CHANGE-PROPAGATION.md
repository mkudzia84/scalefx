# Change Propagation Checklist

> **REFERENCE DOCUMENT:** Use this to verify all affected files are updated after any change.

---

## Quick Matrix

```yaml
Change_Type_Matrix:
  New_Command:
    core/core.h: IF_NEW_GENERIC_ERROR
    xxxfx/xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    packets.py: REQUIRED
    commands.py: REQUIRED
    test_*.py: REQUIRED          # ALWAYS add tests for new commands
    cli/handlers/xxxfx.py: REQUIRED
    packets.go: REQUIRED         # Mirror packet constant + PacketTypeName()
    commands.go: REQUIRED        # Add command builder function
    handler_xxxfx.go: REQUIRED   # Add CLI command + register
    parsers.go: IF_RESPONSE_DATA # Add response parser
    PacketTypes.cs: REQUIRED     # Mirror packet constant
    XxxFxCommands.cs: REQUIRED   # Add command builder method
    README.md: REQUIRED
    
  Modify_Command_Payload:
    core/core.h: NO
    xxxfx/xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    packets.py: IF_CONSTANTS_CHANGED
    commands.py: REQUIRED
    test_*.py: REQUIRED          # ALWAYS update tests for payload changes
    cli/handlers/xxxfx.py: REQUIRED
    commands.go: REQUIRED        # Update command builder
    parsers.go: IF_RESPONSE      # Update response parser
    handler_xxxfx.go: REQUIRED   # Update CLI handler
    XxxFxCommands.cs: REQUIRED   # Update command builder
    README.md: REQUIRED
    FIRMWARE_VERSION: REQUIRED   # MAJOR if field type/size changed, MINOR if appended
    
  New_Error_Code:
    core/core.h: IF_GENERIC_ERROR
    xxxfx/xxxfx.h: IF_MODULE_ERROR
    xxxfx_pico.ino: MAYBE
    packets.py: REQUIRED
    packets.go: REQUIRED         # Add error constant + name map entry
    ErrorCodes.cs: REQUIRED      # Add error constant
    commands.py: NO
    test_*.py: RECOMMENDED       # Test error conditions
    cli/handlers/xxxfx.py: NO
    README.md: REQUIRED
    
  Bug_Fix_In_Handler:
    core/core.h: NO
    xxxfx/xxxfx.h: MAYBE
    xxxfx_pico.ino: REQUIRED
    packets.py: NO
    commands.py: NO
    test_*.py: REQUIRED          # Add regression test for the fix
    cli/handlers/xxxfx.py: NO
    README.md: NO
    
  New_Controller:
    core/core.h: IF_NEW_GENERIC_ERROR
    newfx/newfx.h: CREATE_NEW
    newfx_pico.ino: CREATE_NEW
    packets.py: REQUIRED
    commands.py: REQUIRED
    test_*.py: REQUIRED          # Create tests/newfx/ directory
    cli/handlers/newfx.py: CREATE_NEW
    cli/interactive.py: REQUIRED  # Register new handler
    packets.go: REQUIRED          # Add packet/error constants
    commands.go: REQUIRED         # Add command builders
    handler_newfx.go: CREATE_NEW  # Add CLI handler + register
    parsers.go: REQUIRED          # Add response parsers
    PacketTypes.cs: REQUIRED      # Add packet constants
    ErrorCodes.cs: REQUIRED       # Add error constants
    Commands/NewFxCommands.cs: CREATE_NEW
    build_and_flash.py: REQUIRED  # Add to CONTROLLERS list
    README.md: CREATE_NEW
```

> **CRITICAL:** Tests are not optional. ALWAYS update tests when protocol is changed or new features are added.

> **CRITICAL:** ALWAYS update ALL THREE CLIs (Python handler, Go CLI, C# library) when new commands or packet types are added. See Rule 19 in `copilot-instructions.md`.

---

## Detailed Checklists

### Adding a New Command

```yaml
Step_1_Serial_Library:
  location: "controllers/lib/sfx_serial/serial/"
  actions:
    - file: "xxxfx/xxxfx.h (Server class)"
      action: |
        1. Add packet type constant in XxxFxPacket namespace
        2. Add error codes in XxxFxError namespace (if needed)
        3. Add callback typedef
        4. Add onXxx() registration method
        5. Add case in handleModulePacket() switch using SFX_* macros
        6. Add private callback member
        7. Add validation to XxxFxSpec namespace if needed

    - file: "xxxfx/xxxfx.h (Client class)"
      action: |
        1. Add client method returning CommandResult (NEVER bool)
        2. Determine response category (instant / query / long-running)
        3. IF QUERY: add response packet type constant
        4. IF QUERY: add onModulePacket() case that resolves tag
        5. IF LONG-RUNNING: document completion signal strategy

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
        1. Add command method to handler class using _send_ack() for ACK-based commands
        2. Add CommandInfo to get_commands() dict
        3. Add import for new command builder if needed
        4. IF query command: add to EXCLUDE set in hubfx.py _build_slave_registry()

Step_5_Go_CLI:
  location: "tools/cli/"
  actions:
    - file: "packets.go"
      action: |
        1. Add packet type constant
        2. Add to PacketTypeName() switch
        3. Add error constants and error name map entries (if any)

    - file: "commands.go"
      action: "Add command builder function"

    - file: "handler_xxxfx.go"
      action: |
        1. Add command function to handler
        2. Add CommandInfo to command list
        3. Use appropriate send pattern (SendExpectACK for instant, custom for query)

    - file: "parsers.go"
      action: "Add response parser if command returns data"

Step_6_CSharp_Library:
  location: "app/win32/ScaleFXSerial/"
  actions:
    - file: "PacketTypes.cs"
      action: "Add packet type constant"

    - file: "ErrorCodes.cs"
      action: "Add error constants (if any)"

    - file: "Commands/XxxFxCommands.cs"
      action: "Add command builder method"

Step_7_Documentation:
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
    - "Did any field change type or size (e.g., u16→u32)? → BREAKING"
  
  if_breaking_change:
    - "Increment MAJOR version"
    - "Document migration path"
    - "Update all parsers (C++, Python, Go, and C#) simultaneously"
    - "Consider deprecation period"
  
  breaking_change_examples:
    - "Changing a field's type (u16→u32, u8→u16)"
    - "Reordering fields in a payload"
    - "Removing a field or packet type"
    - "Changing the semantic meaning of an existing field"

Files_To_Update:
  - file: "xxxfx/xxxfx.h"
    changes:
      - "Update handleModulePacket() case (SFX_* macros)"
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
  
  - file: "tools/cli/commands.go"
    changes:
      - "Update command builder function"

  - file: "tools/cli/handler_xxxfx.go"
    changes:
      - "Update CLI command handler"

  - file: "tools/cli/parsers.go"
    changes:
      - "Update response parser (if applicable)"

  - file: "ScaleFXSerial/Commands/XxxFxCommands.cs"
    changes:
      - "Update command builder method"

  - file: "README.md"
    changes:
      - "Update payload format in table"
      - "Update version history"
```

---

### Adding a New Error Code

```yaml
Serial_Library:
  - file: "xxxfx/xxxfx.h (or core/core.h for generic errors)"
    action: |
      1. Add constant in appropriate error namespace
      2. Add case to getMessage() function

Python_Framework:
  - file: "packets.py"
    action: |
      1. Add constant to XxxError class
      2. Add case to name() static method

Go_CLI:
  - file: "packets.go"
    action: |
      1. Add error constant
      2. Add to error name map

CSharp_Library:
  - file: "ErrorCodes.cs"
    action: "Add error constant"

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
    - "[ ] Reserve packet type range (0x80-0xEF available)"
    - "[ ] Create newfx/newfx.h (with NewFxServer, NewFxClient, NewFxPacket, NewFxError, NewFxSpec)"
    - "[ ] Update serial.h umbrella include"
  
  Controller:
    - "[ ] Create directory structure"
    - "[ ] Create platformio.ini"
    - "[ ] Create newfx_pico.ino using SfxServer pattern"
    - "[ ] Use server.begin(), server.onInit(), server.onShutdown()"
    - "[ ] Use server.addModuleHandler(&newfxServer)"
    - "[ ] Use server.core().onStatusData() for module status"
    - "[ ] Use server.indicators().setErrorCondition() if needed"  
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
  
  Go_CLI:
    - "[ ] Add NewFxPacket/NewFxError constants to packets.go"
    - "[ ] Add PacketTypeName() entries and error name map"
    - "[ ] Add command builders to commands.go"
    - "[ ] Create handler_newfx.go with CLI commands"
    - "[ ] Add response parsers to parsers.go"
    - "[ ] Register commands in cli.go command list"

  CSharp_Library:
    - "[ ] Add packet type constants to PacketTypes.cs"
    - "[ ] Add error constants to ErrorCodes.cs"
    - "[ ] Create Commands/NewFxCommands.cs with command builders"

  Scripts:
    - "[ ] Add controller to CONTROLLERS list in scripts/build_and_flash.py"
    - "[ ] Update docstring controller list in build_and_flash.py"
  
  Documentation:
    - "[ ] Create README.md with full protocol docs"
```

---

## File Location Reference

```yaml
Platform_Abstraction:
  path: "controllers/lib/sfx_platform/platform/"
  files:
    sfx_platform.h: "Cross-platform abstraction (mutexes, delays, GPIO, memory, I2S, servo, interrupts)"

Audio_Library:
  path: "controllers/lib/sfx_audio/audio/"
  files:
    audio_mixer.h/.cpp: "8-channel WAV mixer singleton with I2S output"
    audio_ring_buffer.h: "Lock-free SPSC ring buffer (Core 0 → Core 1)"
    audio_config.h: "Compile-time audio constants (sample rate, channels, buffer sizes)"
    audio_codec.h: "Abstract codec base class"
    audio_log.h: "Audio-specific logging macros"
    tas5825_codec.h/.cpp: "TI TAS5825M digital amp driver (I2C)"
    simple_i2s_codec.h/.cpp: "Generic I2S codec (no I2C control)"
    mock_i2s_sink.h/.cpp: "Mock I2S output for testing"

C++_Serial_Library:
  path: "controllers/lib/sfx_serial/serial/"
  files:
    serial.h: "Umbrella header"
    core/core.h: "CoreProtocol, SerialError, CommandResult, ICommandHandler, CommandRouter, SFX_* macros"
    core/core.cpp: "CoreProtocol implementations, CorePayload encode/decode"
    core/bus_server.h: "BusServer base class + CoreCommandServer"
    core/bus_server.cpp: "BusServer + CoreCommandServer implementations"
    client/bus_client.h: "BusClient base class (extends SerialBus)"
    client/bus_client.cpp: "BusClient implementation"
    client/bus.h: "SerialBus (client-only, COBS over USB CDC)"
    client/result_queue.h: "ResultQueue (tag-correlated command/response matching)"
    core/stream.h: "StreamProtocol (0xA4-0xA6) + StreamWriter (chunked streaming, CRC-16)"
    core/stream.cpp: "StreamWriter + CRC-16/CCITT implementations"
    gunfx/gunfx.h: "GunFxServer, GunFxClient, GunFxPacket, GunFxError, GunFxSpec"
    lightfx/lightfx.h: "LightFxServer, LightFxClient, LightFxPacket, LightFxError"
    gearcontrol/gearcontrol.h: "GearControlServer, GearControlClient, GearControlPacket, GearControlError"
    hubfx/hubfx.h: "HubFxAudioServer/Client, HubFxStorageServer/Client, HubFxPacket, HubFxError (NOT auto-included by serial.h)"

Controller_Firmware:
  pattern: "controllers/{name}/pico/"
  files:
    "src/{name}_pico.ino": "Main firmware"
    "platformio.ini": "Build configuration"
    "README.md": "Protocol documentation"

Scripts:
  path: "scripts/"
  files:
    "build_and_flash.py": "Centralized build/flash for all controllers (Pico + ESP32-S3)"

Python_Framework:
  path: "tests/"
  files:
    "framework/__init__.py": "Public exports"
    "framework/connection.py": "ScaleFXConnection"
    "framework/protocol.py": "COBS, CRC, packet helpers"
    "framework/packets.py": "Constants (mirror C++)"
    "framework/commands.py": "Command builders"
    "cli/base.py": "CommandHandlerBase (_send_ack, _wrap_packet), CommandInfo, OutputMixin, ControllerType"
    "cli/output.py": "TerminalUI split-screen terminal (prompt_toolkit Application)"
    "cli/parsers.py": "Response packet parsing utilities"
    "cli/interactive.py": "Main CLI class (composes handlers + TerminalUI)"
    "cli/handlers/core.py": "Core/protocol commands"
    "cli/handlers/gunfx.py": "GunFX commands (uses _send_ack for hub routing compatibility)"
    "cli/handlers/lightfx.py": "LightFX commands (uses _send_ack for hub routing compatibility)"
    "cli/handlers/gearcontrol.py": "GearControl commands (uses _send_ack for hub routing compatibility)"
    "cli/handlers/hubfx.py": "HubFX hub commands + composed slave routing"
    "cli/handlers/storage.py": "Reusable file operations (SD/Flash)"
    "conftest.py": "pytest fixtures"

Go_CLI:
  path: "tools/cli/"
  files:
    "main.go": "Entry point, flag parsing"
    "cli.go": "Interactive loop, command dispatch, async packet handler"
    "connection.go": "Serial connection, tag-correlated send/receive, stream waiters"
    "protocol.go": "COBS encode/decode, CRC-8/CRC-16, packet build/parse"
    "packets.go": "Packet type constants, error codes (mirrors C++ headers)"
    "commands.go": "Command builders (mirrors tests/framework/commands.py)"
    "parsers.go": "Response payload parsers (status, I2C, init_ready, gear, audio, etc.)"
    "output.go": "ANSI colored output, help rendering"
    "helpers.go": "Shared utilities (arg parsing, guards, servo patterns)"
    "format_storage.go": "Storage-related output formatting"
    "handler_core.go": "Core commands (connect, init, status, reboot, etc.)"
    "handler_gunfx.go": "GunFX commands (trigger, servo, smoke)"
    "handler_lightfx.go": "LightFX commands (LED, sequences, servo, landing lights)"
    "handler_gearcontrol.go": "GearControl commands (gear, servo, yaw, calibration)"
    "handler_hubfx.go": "HubFX commands (slaves, audio, engine, storage, USB)"

CSharp_Library:
  path: "app/win32/ScaleFXSerial/"
  files:
    "PacketTypes.cs": "Packet type constants (mirrors C++ headers)"
    "ErrorCodes.cs": "Error code constants with name lookup"
    "ScaleFxConnection.cs": "Serial connection with COBS framing"
    "Protocol/Packet.cs": "Packet structure and parsing"
    "Protocol/Cobs.cs": "COBS encode/decode"
    "Protocol/Crc.cs": "CRC-8 implementation"
    "Protocol/Endian.cs": "Little-endian helpers"
    "Commands/CoreCommands.cs": "Core protocol commands"
    "Commands/GunFxCommands.cs": "GunFX command builders"
    "Commands/LightFxCommands.cs": "LightFX command builders"
    "Commands/GearControlCommands.cs": "GearControl command builders"
    "Commands/HubFxCommands.cs": "HubFX command builders"
```

---

## Verification Commands

```bash
# Build verification
python -m platformio run -e pico -d controllers/gunfx/pico
python -m platformio run -e pico -d controllers/lightfx/pico
python -m platformio run -e pico -d controllers/gearcontrol/pico
python -m platformio run -e pico -d controllers/noop/pico

# Build ESP32-S3
python -m platformio run -e esp32s3 -d controllers/hubfx/esp32s3

# Build and flash (centralized script)
python scripts/build_and_flash.py gunfx
python scripts/build_and_flash.py lightfx
python scripts/build_and_flash.py gearcontrol
python scripts/build_and_flash.py hubfx
python scripts/build_and_flash.py noop

# Python syntax
python -m py_compile tests/framework/packets.py
python -m py_compile tests/framework/commands.py
python -m py_compile tests/cli/interactive.py
python -m py_compile tests/cli/handlers/gunfx.py
python -m py_compile tests/cli/handlers/hubfx.py
python -m py_compile tests/cli/handlers/lightfx.py
python -m py_compile tests/cli/handlers/gearcontrol.py

# Go CLI
cd tools/cli && go build .

# C# library
dotnet build app/win32/ScaleFXSerial/

# Run tests (requires hardware)
pytest tests/gunfx/ -v
pytest tests/lightfx/ -v
pytest tests/gearcontrol/ -v
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
  Cause: "Packet constant mismatch between C++, Python, Go, or C#"
  Fix:
    - "Verify core/core.h / xxxfx/xxxfx.h constant value"
    - "Verify packets.py constant value"
    - "Verify packets.go constant value"
    - "Verify PacketTypes.cs constant value"
    - "Ensure they are all identical"

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
