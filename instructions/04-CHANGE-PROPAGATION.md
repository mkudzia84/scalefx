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
    protocol/xxxfx/xxxfx.go: REQUIRED  # Mirror packet constant + register in init()
    api/xxxfx.go: REQUIRED             # Add typed API method
    engine/handlers/xxxfx/handler.go: REQUIRED  # Add CLI command + register
    engine/handlers/xxxfx/parsers.go: IF_RESPONSE_DATA # Add response parser
    README.md: REQUIRED
    
  Modify_Command_Payload:
    core/core.h: NO
    xxxfx/xxxfx.h: REQUIRED
    xxxfx_pico.ino: REQUIRED
    protocol/xxxfx/xxxfx.go: REQUIRED  # Update command builder
    engine/handlers/xxxfx/parsers.go: IF_RESPONSE  # Update response parser
    engine/handlers/xxxfx/handler.go: REQUIRED     # Update CLI handler
    README.md: REQUIRED
    FIRMWARE_VERSION: REQUIRED   # MAJOR if field type/size changed, MINOR if appended
    
  New_Error_Code:
    core/core.h: IF_GENERIC_ERROR
    xxxfx/xxxfx.h: IF_MODULE_ERROR
    xxxfx_pico.ino: MAYBE
    protocol/xxxfx/xxxfx.go: REQUIRED  # Add error constant + register in init()
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
    protocol/newfx/newfx.go: CREATE_NEW  # Add packet/error constants
    api/newfx.go: CREATE_NEW             # Add typed API methods
    engine/handlers/newfx/handler.go: CREATE_NEW  # Add CLI handler + register
    engine/handlers/newfx/parsers.go: CREATE_NEW  # Add response parsers
    README.md: CREATE_NEW
```

> **CRITICAL:** ALWAYS update the Go CLI when new commands or packet types are added. See Rule 19 in `copilot-instructions.md`.

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
    - file: "protocol/xxxfx/xxxfx.go"
      action: |
        1. Add packet type constant
        2. Add command builder function
        3. Add error constants (if any)
        4. Register names in init()

    - file: "api/xxxfx.go"
      action: "Add typed API method"

    - file: "engine/handlers/xxxfx/handler.go"
      action: |
        1. Add command function to handler
        2. Add CmdEntry to command list
        3. Use appropriate send pattern (SendExpectACK for instant, custom for query)

    - file: "engine/handlers/xxxfx/parsers.go"
      action: "Add response parser if command returns data"

Step_4_Documentation:
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
    - "Update all parsers (C++ and Go) simultaneously"
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
  
  - file: "app/go/protocol/xxxfx/xxxfx.go"
    changes:
      - "Update command builder function"

  - file: "app/go/engine/handlers/xxxfx/handler.go"
    changes:
      - "Update CLI command handler"

  - file: "app/go/engine/handlers/xxxfx/parsers.go"
    changes:
      - "Update response parser (if applicable)"

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
  - file: "protocol/xxxfx/xxxfx.go"
    action: |
      1. Add error constant
      2. Register in init() error name map

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
    - "[ ] Reserve packet type range (0x80-0xEE available)"
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
    - "[ ] Add NewFxPacket/NewFxError constants to protocol/newfx/newfx.go"
    - "[ ] Register names in init()"
    - "[ ] Add command builders to protocol/newfx/newfx.go"
    - "[ ] Add typed API methods to api/newfx.go"
    - "[ ] Create engine/handlers/newfx/handler.go with CLI commands"
    - "[ ] Add response parsers to engine/handlers/newfx/parsers.go"
    - "[ ] Register handler in engine/handlers/handlers.go"
  
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
    "protocol/types.go": "PacketType, ErrorCode types, name registry"
    "protocol/stream.go": "Stream protocol (chunked data, CRC-16)"
    "protocol/connection.go": "Serial connection, tag-correlated send/receive, stream waiters"
    "protocol/core/core.go": "Core packet types, error codes (mirrors core/core.h)"
    "protocol/gunfx/gunfx.go": "GunFX packet types, error codes, commands (mirrors gunfx.h)"
    "protocol/lightfx/lightfx.go": "LightFX packet types, error codes, commands (mirrors lightfx.h)"
    "protocol/gearcontrol/gearcontrol.go": "GearControl packet types, error codes, commands (mirrors gearcontrol.h)"
    "protocol/hubfx/hubfx.go": "HubFX packet types, error codes, commands (mirrors hubfx.h)"
    "api/result.go": "ApiResult types"
    "api/client.go": "apiClient base (wraps protocol.Connection)"
    "api/core.go": "CoreApi (init, status, reboot, identify)"
    "api/gunfx.go": "GunFxApi (trigger, servo, smoke)"
    "api/lightfx.go": "LightFxApi (LED, sequences, servo, landing lights)"
    "api/gearcontrol.go": "GearControlApi (gear, servo, yaw, calibration)"
    "api/hubfx.go": "HubFxApi (slaves, audio, engine, storage, USB)"
    "api/files.go": "FileApi (SD/flash file operations)"
    "engine/engine.go": "Core Engine struct (connection, API, dispatch, listener)"
    "engine/types.go": "CmdEntry, CmdGroup, InitReadyInfo, ControllerColors"
    "engine/output.go": "Output interface + ANSI terminal implementation"
    "engine/helpers.go": "Shared utilities (Atoi, ParseBool, ServoSet, ServoConfig)"
    "engine/parsers.go": "Common response parsers"
    "engine/parsers_core.go": "Core response parsers (INIT_READY, STATUS header)"
    "engine/handlers/handlers.go": "RegisterDefaults() — registers all built-in groups"
    "engine/handlers/core/handler.go": "Core commands (connect, init, status, reboot, etc.)"
    "engine/handlers/gunfx/handler.go": "GunFX commands (trigger, servo, smoke)"
    "engine/handlers/lightfx/handler.go": "LightFX commands (LED, sequences, servo, landing lights)"
    "engine/handlers/lightfx/parsers.go": "LightFX response parsers"
    "engine/handlers/gearcontrol/handler.go": "GearControl commands (gear, servo, yaw, calibration)"
    "engine/handlers/gearcontrol/parsers.go": "GearControl response parsers"
    "engine/handlers/hubfx/handler.go": "HubFX commands (slaves, audio, engine, storage, USB)"
    "engine/handlers/hubfx/parsers.go": "HubFX response parsers"
    "engine/handlers/hubfx/format.go": "HubFX output formatting"
    "engine/handlers/firmware/handler.go": "Firmware release commands"
    "cli/main.go": "Entry point, flag parsing"
    "cli/cli.go": "Terminal readline loop, delegates to engine"
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
  Cause: "Packet constant mismatch between C++ and Go"
  Fix:
    - "Verify core/core.h / xxxfx/xxxfx.h constant value"
    - "Verify protocol/xxxfx/xxxfx.go constant value"
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
