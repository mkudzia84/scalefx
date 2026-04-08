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

Task: "Build and deploy firmware"
  → Read: 05-BUILD-AND-FLASH.md

Task: "Update CLI"
  → Read: 07-CLI-UPDATES.md

Task: "Work on HubFX audio engine (AudioTools library)"
  → Read: 08-AUDIOTOOLS.md

Task: "Work on file upload protocol (stream, windowed, flow control)"
  → Read: 10-UPLOAD-PROTOCOL-REFACTOR.md
  → Reference: controllers/hubfx/esp32s3/README.md § Stream Upload Architecture

Task: "Work on HubFX"
  → Target: controllers/hubfx/esp32s3/ (HubFX Pico is OBSOLETE)
  → Reference: controllers/hubfx/pico/ (frozen, consult for patterns only)
  → Read: 01-ARCHITECTURE.md
```

---

## Critical Constants

```yaml
Protocol:
  format: "Binary COBS with CRC-8"
  crc_polynomial: 0x07
  baud_rate: 6000000
  packet_structure: "[type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]"
  endianness: "little-endian"

Packet_Ranges:
  GunFX: "0x01-0x2F"
  LightFX: "0x40-0x5F"
  GearControl: "0x60-0x7F"
  HubFX: "0x80-0xAF"
  Streaming: "0xA4-0xA6"
  Available: "0xB0-0xEF"
  Core: "0xF0-0xFF"

Controllers:
  gunfx: { path: "controllers/gunfx/pico/", range: "0x01-0x2F" }
  lightfx: { path: "controllers/lightfx/pico/", range: "0x40-0x5F" }
  gearcontrol: { path: "controllers/gearcontrol/pico/", range: "0x60-0x7F" }
  hubfx: { path: "controllers/hubfx/esp32s3/", range: "0x80-0xAF", platform: "ESP32-S3" }
  noop: { path: "controllers/noop/pico/", range: "CORE_ONLY" }
```

---

## File Location Index

```yaml
Platform_Abstraction:
  root: "controllers/lib/sfx_platform/platform/"
  files:
    - name: "sfx_platform.h"
      purpose: "Cross-platform abstraction (mutexes, delays, GPIO, memory, I2S, servo, interrupts)"
      modify_when: "Adding new platform-specific abstractions or supporting a new chip"
    - name: "sfx_wire.h/.cpp"
      purpose: "Stateless wire encoding (CRC-8, COBS, packet build/parse, endian helpers)"
      modify_when: "Changing wire format or adding encoding utilities"
    - name: "diag_log.h/.cpp"
      purpose: "DiagLog singleton — ring-buffered diagnostic logging over COBS serial"
      modify_when: "Changing log format, ring buffer size, or adding log features"

Audio_Library:
  root: "controllers/lib/sfx_audio/audio/"
  files:
    - name: "audio_mixer.h/.cpp"
      purpose: "8-channel WAV mixer singleton with I2S output (platform-conditional buffer sizes)"
      modify_when: "Changing mixer behavior, buffer sizes, or adding playback features"
    - name: "audio_ring_buffer.h"
      purpose: "Lock-free SPSC ring buffer for stereo int16 frames (Core 0 → Core 1)"
      modify_when: "Changing ring buffer size or DMA attributes"
    - name: "audio_config.h"
      purpose: "Compile-time audio constants (sample rate, channels, buffer sizes, I2S pins)"
      modify_when: "Changing audio defaults or adding platform-conditional sizes"
    - name: "audio_codec.h"
      purpose: "Abstract codec base class interface"
      modify_when: "Adding new codec types"
    - name: "audio_log.h"
      purpose: "Audio-specific logging macros (MIXER_LOG, TAS5825_LOG, MOCK_LOG)"
      modify_when: "Adding new audio module log prefixes"
    - name: "tas5825_codec.h/.cpp"
      purpose: "TI TAS5825M digital amplifier driver (I2C)"
      modify_when: "Changing codec initialization or volume control"
    - name: "simple_i2s_codec.h/.cpp"
      purpose: "Generic I2S codec (no I2C control needed)"
      modify_when: "Rarely"
    - name: "mock_i2s_sink.h/.cpp"
      purpose: "Mock I2S output for testing (captures statistics)"
      modify_when: "Adding test instrumentation"

Config_Library:
  root: "controllers/lib/sfx_config/"
  files:
    - name: "config/yaml_parser.h/.ipp"
      purpose: "Lightweight YAML subset parser (maps, sequences, scalars) with templatized node pool"
      modify_when: "Adding YAML parsing features or fixing parser edge cases"
    - name: "config/config_store.h/.ipp"
      purpose: "Templatized config manager — schema-driven loading, validation, defaults"
      modify_when: "Changing config load/save lifecycle or adding config infrastructure"
    - name: "server/config_server.h/.ipp"
      purpose: "BusServer handler for CONFIG_RELOAD / CONFIG_STATUS protocol commands"
      modify_when: "Adding new config-related protocol commands"
    - name: "client/config_client.h/.cpp"
      purpose: "BusClient for sending CONFIG_RELOAD / CONFIG_STATUS commands"
      modify_when: "Adding new config client methods"

Serial_Library:
  root: "controllers/lib/sfx_serial/serial/"
  files:
    - name: "serial.h"
      purpose: "Umbrella header (include this)"
    - name: "core/core.h"
      purpose: "CoreProtocol (COBS/CRC/endian), SerialError, CommandResult, ICommandHandler, CommandRouter, SFX_* macros"
      modify_when: "Adding generic error codes, handler macros, or modifying core protocol"
    - name: "core/core.cpp"
      purpose: "CoreProtocol implementations, CorePayload encode/decode"
      modify_when: "Rarely — protocol-level changes only"
    - name: "core/bus_server.h"
      purpose: "BusServer base class + CoreCommandServer"
      modify_when: "Rarely — base class for all server handlers"
    - name: "core/bus_server.cpp"
      purpose: "BusServer + CoreCommandServer implementations"
      modify_when: "Rarely"
    - name: "client/bus_client.h"
      purpose: "BusClient base class (extends SerialBus)"
      modify_when: "Rarely — base class for all client controllers"
    - name: "client/bus_client.cpp"
      purpose: "BusClient implementation"
      modify_when: "Rarely"
    - name: "client/bus.h"
      purpose: "SerialBus (client-only, COBS over USB CDC)"
      modify_when: "Never (stable transport layer)"
    - name: "client/result_queue.h"
      purpose: "ResultQueue — tag-correlated command/response matching"
      modify_when: "Never (stable infrastructure)"
    - name: "core/stream.h"
      purpose: "StreamProtocol (0xA4-0xA6) + StreamWriter — chunked data streaming with CRC-16"
      modify_when: "Adding new streaming features or changing chunk format"
    - name: "core/stream.cpp"
      purpose: "StreamWriter + CRC-16/CCITT implementations"
      modify_when: "Rarely — streaming infrastructure"
    - name: "gunfx/gunfx.h"
      purpose: "GunFxServer, GunFxClient, GunFxPacket, GunFxError, GunFxSpec"
      modify_when: "Adding GunFX commands or error codes"
    - name: "lightfx/lightfx.h"
      purpose: "LightFxServer, LightFxClient, LightFxPacket, LightFxError"
      modify_when: "Adding LightFX commands or error codes"
    - name: "gearcontrol/gearcontrol.h"
      purpose: "GearControlServer, GearControlClient, GearControlPacket, GearControlError"
      modify_when: "Adding GearControl commands or error codes"
    - name: "hubfx/hubfx.h"
      purpose: "HubFxAudioServer/Client, HubFxStorageServer/Client, HubFxPacket, HubFxError (NOT auto-included by serial.h)"
      modify_when: "Adding HubFX audio, storage, or file commands"

Controllers:
  pattern: "controllers/{name}/pico/"
  files:
    - name: "src/{name}_pico.ino"
      purpose: "Main firmware file"
    - name: "platformio.ini"
      purpose: "Build configuration"
    - name: "README.md"
      purpose: "Protocol documentation"
```
```yaml
Go_CLI:
  root: "app/go/"
  packages:
    protocol:
      - name: "wire.go"
        purpose: "CRC-8/CRC-16, COBS encode/decode, packet build/parse"
      - name: "packets.go"
        purpose: "Packet type constants, error codes (mirrors C++ headers)"
        modify_when: "Adding packet types or error codes"
      - name: "commands.go"
        purpose: "Command builders (mirrors C++ command definitions)"
        modify_when: "Adding commands"
      - name: "connection.go"
        purpose: "Serial connection, tag-correlated send/receive, stream waiters"
    api:
      - name: "result.go"
        purpose: "ApiResult types"
      - name: "client.go"
        purpose: "apiClient base (wraps protocol.Connection)"
      - name: "core.go"
        purpose: "CoreApi (init, status, reboot, identify)"
      - name: "gunfx.go"
        purpose: "GunFxApi (trigger, servo, smoke)"
      - name: "lightfx.go"
        purpose: "LightFxApi (LED, sequences, servo, landing lights)"
      - name: "gearcontrol.go"
        purpose: "GearControlApi (gear, servo, yaw, calibration)"
      - name: "hubfx.go"
        purpose: "HubFxApi (slaves, audio, engine, storage, USB)"
      - name: "files.go"
        purpose: "FileApi (SD/flash file operations)"
    cli:
      - name: "main.go"
        purpose: "Entry point, flag parsing"
      - name: "cli.go"
        purpose: "Interactive loop, command dispatch, async packet handler"
      - name: "output.go"
        purpose: "ANSI colored output, help rendering"
      - name: "helpers.go"
        purpose: "Shared utilities (arg parsing, guards, servo patterns)"
      - name: "format_storage.go"
        purpose: "Storage-related output formatting"
      - name: "parsers.go"
        purpose: "Response payload parsers (shared base)"
        modify_when: "Adding response packet types"
      - name: "parsers_core.go"
        purpose: "Core response parsers"
      - name: "parsers_gunfx.go"
        purpose: "GunFX response parsers"
      - name: "parsers_lightfx.go"
        purpose: "LightFX response parsers"
      - name: "parsers_gearcontrol.go"
        purpose: "GearControl response parsers"
      - name: "parsers_hubfx.go"
        purpose: "HubFX response parsers"
      - name: "handler_core.go"
        purpose: "Core commands (connect, init, status, reboot, etc.)"
        modify_when: "Modifying core commands"
      - name: "handler_gunfx.go"
        purpose: "GunFX commands (trigger, servo, smoke)"
        modify_when: "Adding GunFX CLI commands"
      - name: "handler_lightfx.go"
        purpose: "LightFX commands (LED, sequences, servo, landing lights)"
        modify_when: "Adding LightFX CLI commands"
      - name: "handler_gearcontrol.go"
        purpose: "GearControl commands (gear, servo, yaw, calibration)"
        modify_when: "Adding GearControl CLI commands"
      - name: "handler_hubfx.go"
        purpose: "HubFX commands (slaves, audio, engine, storage, USB)"
        modify_when: "Adding HubFX CLI commands"

CSharp_Library:
  root: "app/win32/ScaleFXSerial/"
  files:
    - name: "PacketTypes.cs"
      purpose: "Packet type constants (mirrors C++ headers)"
      modify_when: "Adding packet types"
    - name: "ErrorCodes.cs"
      purpose: "Error code constants with name lookup"
      modify_when: "Adding error codes"
    - name: "ScaleFxConnection.cs"
      purpose: "Serial connection with COBS framing"
      modify_when: "Rarely"
    - name: "Protocol/Packet.cs"
      purpose: "Packet structure and parsing"
    - name: "Protocol/Cobs.cs"
      purpose: "COBS encode/decode"
    - name: "Protocol/Crc.cs"
      purpose: "CRC-8 implementation"
    - name: "Protocol/Endian.cs"
      purpose: "Little-endian helpers"
    - name: "Commands/CoreCommands.cs"
      purpose: "Core protocol commands"
    - name: "Commands/GunFxCommands.cs"
      purpose: "GunFX command builders"
      modify_when: "Adding GunFX commands"
    - name: "Commands/LightFxCommands.cs"
      purpose: "LightFX command builders"
      modify_when: "Adding LightFX commands"
    - name: "Commands/GearControlCommands.cs"
      purpose: "GearControl command builders"
      modify_when: "Adding GearControl commands"
    - name: "Commands/HubFxCommands.cs"
      purpose: "HubFX command builders"
      modify_when: "Adding HubFX commands"
```

---

## Decision Trees

### When Adding a Command

```
START: Need to add command to controller

Q1: Is packet type constant defined?
├─ NO → Add to xxxfx/xxxfx.h in correct namespace (e.g., GunFxPacket in gunfx/gunfx.h)
└─ YES → Continue

Q2: What response category? (See 03-PROTOCOL-EXTENSION.md § Response Category Decision)
├─ INSTANT → Server uses SFX_DISPATCH, client gets auto-ACK
├─ QUERY   → Server sends data response, client resolves tag in onModulePacket()
└─ LONG-RUNNING → Server sends immediate ACK, client monitors via STATUS/async

Q3: Are new error codes needed?
├─ YES → Add to xxxfx/xxxfx.h in module error namespace (e.g., GunFxError)
│        Add to app/go/protocol/packets.go and app/win32/ScaleFXSerial/ErrorCodes.cs
└─ NO → Continue

Q4: Is callback type defined in xxxfx/xxxfx.h?
├─ NO → Add callback typedef
│        Add registration method: void onXxx(Callback cb)
│        Add private member: Callback _onXxx
│        Add case in handleModulePacket() switch using SFX_* macros
└─ YES → Continue

Q5: Is client method defined in xxxfx/xxxfx.h?
├─ NO → Add method returning CommandResult (NEVER bool)
│        IF QUERY: add response type + onModulePacket() tag resolution
│        IF LONG-RUNNING: document completion signal
└─ YES → Continue

Q6: Is command implemented in firmware?
├─ NO → Add callback implementation in xxxfx_pico.ino
│        Register callback in setup()
└─ YES → Continue

Q7: Is Go CLI updated?
├─ NO → Add to app/go/protocol/packets.go, protocol/commands.go, cli/handler_xxxfx.go
│        - Add packet constant + PacketTypeName()
│        - Add command builder
│        - Add CLI command + register
└─ YES → Continue

Q8: Is C# library updated?
├─ NO → Add to PacketTypes.cs + Commands/XxxFxCommands.cs
└─ YES → Continue

Q9: Is documentation updated?
├─ NO → Update controllers/xxxfx/pico/README.md
└─ YES → DONE
```

---

## Mandatory File Sync Points

**CRITICAL:** These file pairs must stay in sync:

```yaml
Sync_Groups:
  - name: "Packet Constants"
    primary: "lib/sfx_serial/serial/core/core.h"
    mirrors:
      - "app/go/protocol/packets.go"
      - "app/win32/ScaleFXSerial/PacketTypes.cs"
    rule: "Same values, same names (PascalCase in C#)"

  - name: "Error Codes (Generic)"
    primary: "lib/sfx_serial/serial/core/core.h (SerialError namespace)"
    mirrors:
      - "app/go/protocol/packets.go (error constants + name map)"
      - "app/win32/ScaleFXSerial/ErrorCodes.cs"
    rule: "Same values, same names"

  - name: "Error Codes (Module)"
    primary: "lib/sfx_serial/serial/xxxfx/xxxfx.h (XxxError namespace)"
    mirrors:
      - "app/go/protocol/packets.go (error constants + name map)"
      - "app/win32/ScaleFXSerial/ErrorCodes.cs"
    rule: "Same values, same names"

  - name: "Command Interface"
    primary: "lib/sfx_serial/serial/xxxfx/xxxfx.h"
    mirrors:
      - "app/go/protocol/commands.go"
      - "app/go/cli/handler_xxxfx.go"
      - "app/win32/ScaleFXSerial/Commands/XxxFxCommands.cs"
    rule: "All platforms must expose same commands"
```

> **All development rules, patterns, and checklists are in `.github/copilot-instructions.md`** (auto-loaded by VS Code Copilot).
> This document is the navigation index only — detailed rules are not duplicated here.

---

## Document Index

| Doc | When to Use |
|-----|-------------|
| [01-ARCHITECTURE.md](01-ARCHITECTURE.md) | System design, packet format, class hierarchy, response categories |
| [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) | Create entirely new controller type (templates included) |
| [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) | Add commands to existing controller (worked example) |
| [04-CHANGE-PROPAGATION.md](04-CHANGE-PROPAGATION.md) | File sync checklists, change type matrix, verification |
| [05-BUILD-AND-FLASH.md](05-BUILD-AND-FLASH.md) | Build firmware, flash to device, troubleshooting |
| [07-CLI-UPDATES.md](07-CLI-UPDATES.md) | Update Go interactive CLI |
| [08-AUDIOTOOLS.md](08-AUDIOTOOLS.md) | AudioTools library reference (3rd-party, HubFX audio engine) |
| [09-CONSOLE-OUTPUT.md](09-CONSOLE-OUTPUT.md) | Console output schema for Go CLI and C# Studio |
| [10-UPLOAD-PROTOCOL-REFACTOR.md](10-UPLOAD-PROTOCOL-REFACTOR.md) | Upload protocol modes (stream, windowed), flow control, ring buffer architecture |
