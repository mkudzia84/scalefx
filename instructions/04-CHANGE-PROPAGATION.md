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
    packets.go: REQUIRED         # Add error constant + name map entry
    ErrorCodes.cs: REQUIRED      # Add error constant
    README.md: REQUIRED
    
  Bug_Fix_In_Handler:
    core/core.h: NO
    xxxfx/xxxfx.h: MAYBE
    xxxfx_pico.ino: REQUIRED
    README.md: NO
    
  New_Controller:
    core/core.h: IF_NEW_GENERIC_ERROR
    newfx/newfx.h: CREATE_NEW
    newfx_pico.ino: CREATE_NEW
    packets.go: REQUIRED          # Add packet/error constants
    commands.go: REQUIRED         # Add command builders
    handler_newfx.go: CREATE_NEW  # Add CLI handler + register
    parsers.go: REQUIRED          # Add response parsers
    PacketTypes.cs: REQUIRED      # Add packet constants
    ErrorCodes.cs: REQUIRED       # Add error constants
    Commands/NewFxCommands.cs: CREATE_NEW
    README.md: CREATE_NEW
```

> **CRITICAL:** ALWAYS update BOTH the Go CLI and the C# library when new commands or packet types are added. See Rule 19 in `copilot-instructions.md`.

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

Step_3_Go_CLI:
  location: "app/go/"
  actions:
    - file: "protocol/packets.go"
      action: |
        1. Add packet type constant
        2. Add to PacketTypeName() switch
        3. Add error constants and error name map entries (if any)

    - file: "protocol/commands.go"
      action: "Add command builder function"

    - file: "cli/handler_xxxfx.go"
      action: |
        1. Add command function to handler
        2. Add CommandInfo to command list
        3. Use appropriate send pattern (SendExpectACK for instant, custom for query)

    - file: "cli/parsers.go"
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
    - "Update all parsers (C++, Go, and C#) simultaneously"
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
  
  - file: "app/go/protocol/commands.go"
    changes:
      - "Update command builder function"

  - file: "app/go/cli/handler_xxxfx.go"
    changes:
      - "Update CLI command handler"

  - file: "app/go/cli/parsers.go"
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

Go_CLI:
  path: "app/go/"
  files:
    "protocol/wire.go": "CRC-8/CRC-16, COBS encode/decode, packet build/parse"
    "protocol/packets.go": "Packet type constants, error codes (mirrors C++ headers)"
    "protocol/commands.go": "Command builders (mirrors C++ command definitions)"
    "protocol/connection.go": "Serial connection, tag-correlated send/receive, stream waiters"
    "api/result.go": "ApiResult types"
    "api/client.go": "apiClient base (wraps protocol.Connection)"
    "api/core.go": "CoreApi (init, status, reboot, identify)"
    "api/gunfx.go": "GunFxApi (trigger, servo, smoke)"
    "api/lightfx.go": "LightFxApi (LED, sequences, servo, landing lights)"
    "api/gearcontrol.go": "GearControlApi (gear, servo, yaw, calibration)"
    "api/hubfx.go": "HubFxApi (slaves, audio, engine, storage, USB)"
    "api/files.go": "FileApi (SD/flash file operations)"
    "cli/main.go": "Entry point, flag parsing"
    "cli/cli.go": "Interactive loop, command dispatch, async packet handler"
    "cli/output.go": "ANSI colored output, help rendering"
    "cli/helpers.go": "Shared utilities (arg parsing, guards, servo patterns)"
    "cli/format_storage.go": "Storage-related output formatting"
    "cli/parsers.go": "Response payload parsers (shared base)"
    "cli/parsers_core.go": "Core response parsers"
    "cli/parsers_gunfx.go": "GunFX response parsers"
    "cli/parsers_lightfx.go": "LightFX response parsers"
    "cli/parsers_gearcontrol.go": "GearControl response parsers"
    "cli/parsers_hubfx.go": "HubFX response parsers"
    "cli/handler_core.go": "Core commands (connect, init, status, reboot, etc.)"
    "cli/handler_gunfx.go": "GunFX commands (trigger, servo, smoke)"
    "cli/handler_lightfx.go": "LightFX commands (LED, sequences, servo, landing lights)"
    "cli/handler_gearcontrol.go": "GearControl commands (gear, servo, yaw, calibration)"
    "cli/handler_hubfx.go": "HubFX commands (slaves, audio, engine, storage, USB)"

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
# Build firmware (via Flash CLI)
app/go/scalefx-flash.exe build gunfx --no-clean
app/go/scalefx-flash.exe build lightfx --no-clean
app/go/scalefx-flash.exe build gearcontrol --no-clean
app/go/scalefx-flash.exe build noop --no-clean

# Build ESP32-S3
app/go/scalefx-flash.exe build hubfx --no-clean

# Go CLI
cd app/go && go build ./cli/

# C# library
dotnet build app/win32/ScaleFXSerial/

# CLI smoke test
app/go/scalefx-cli.exe -p COM5
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
  Cause: "Packet constant mismatch between C++, Go, or C#"
  Fix:
    - "Verify core/core.h / xxxfx/xxxfx.h constant value"
    - "Verify packets.go constant value"
    - "Verify PacketTypes.cs constant value"
    - "Ensure they are all identical"

Error: "NACK with MISSING_PARAMETER"
  Cause: "Payload too short"
  Fix:
    - "Check tryProcess() SFX_REQUIRE_LEN() value"
    - "Check Go command builder payload size"
    - "Count bytes: u8=1, u16=2, u32=4"

Error: "CLI command not appearing"
  Cause: "Command not registered in handler"
  Fix:
    - "Verify command added to handler command list"
    - "Verify controller detection logic"

Error: "Values appear swapped/corrupted"
  Cause: "Endianness mismatch"
  Fix:
    - "C++: val = payload[0] | (payload[1] << 8)  // little-endian"
    - "Go: binary.LittleEndian.PutUint16(buf, val)  // little-endian"
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
```
