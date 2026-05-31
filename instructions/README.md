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

Task: "Add an on-board (non-USB) expander co-processor — PWM expander, sensor frontend, etc."
  → Read: 14-ONBOARD-COPROCESSOR.md
  → Pattern: ESP32-C3 over UART, master-bridged flashing
  → Cross-reference: 02-NEW-CONTROLLER.md (7-file pattern still applies)

Task: "Migrate an expander board to the generic component-collection protocol"
  → Read: 15-GENERIC-EXPANDER-REFACTOR.md
  → Lib skeleton: controllers/lib/sfx_peripherals/collections/ + sfx_expander/ (currently sfx_slave/ pre-rename)
  → Per-board steps: GunFX → GearControl → LightFX (in that order)

Task: "Design or audit an expander board firmware (post-pivot)"
  → Read: 16-EXPANDER-BOARD-DESIGN.md
  → Covers: core protocol surface (INIT / KEEPALIVE / REBOOT / I2C_SCAN / LOG / IDENTIFY)
  → Covers: expander protocol surface (component collections, async events, status broadcast)
  → Covers: persistence rules (only /board.yaml + /.system/board.guid — no general file-system on expanders)

Task: "Compose a board's system services (storage / audio / battery / config / USB host / expander bus)"
  → Read: 17-SYSTEM-SERVICES.md
  → Covers: CoreCommandServer<...ServicePolicies> composition, deterministic UUIDv5 board GUID,
            per-port GUIDs, storage backends as variadic policy, IDENTIFY payload extension

Task: "Work on HubFX"
  → Target: controllers/hubfx/esp32s3/ (HubFX Pico is OBSOLETE)
  → Reference: controllers/hubfx/pico/ (frozen, consult for patterns only)
  → Read: 01-ARCHITECTURE.md

Task: "Build a new Studio effect-tab panel (operational — RC-channel-gated, sounds, Apply+Start)"
  → Read FIRST: 23-STUDIO-WIDGET-CATALOG.md   ← handbook of every reusable Studio widget pattern
  → Walkthrough:    21-STUDIO-ENGINEFX-PANEL.md
  → Cribs from: app/go/studio/frontend/src/lib/tabs/EnginePanel.svelte
  → Rules: 34 (design system), 35 (validation gates Apply + operational), 36 (channel-setup cluster), 43 (named channels)

Task: "Add or modify a Studio widget — picker / cluster / row / button cluster / validation"
  → Read: 23-STUDIO-WIDGET-CATALOG.md           ← find the pattern you need
  → Follow up: the formal rule + reference panel cited in the catalog entry

Task: "Add a channel-gated trigger (RC channel + threshold + hysteresis cluster, ANY effect)"
  → Read: 21-STUDIO-ENGINEFX-PANEL.md § 2  — the bar with overlays, copy markup + .chan-cluster/.threshold-mark/.hyst-band/.bar-legend CSS verbatim
  → Rule: 36
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
  Expander_Identity: "0x01-0x0F" # COMPONENT_LIST, IDENT_*, EXPANDER_STATUS_BROADCAST/RATE
  Expander_Servo:    "0x10-0x2F" # servo control + async target-reached
  Expander_PWM:      "0x30-0x4F" # PWM control + mode mutability + sensing query
  Expander_LED:      "0x50-0x7F" # LED control + event-sequence runtime + async program-done
  HubFX:             "0x80-0xAF"
  Streaming:         "0xA4-0xA6"
  Available:         "0xB0-0xEE"
  Core:              "0xEF-0xFF"
# Hard cutover 2026-05-06: per-board ranges (0x01-0x2F GunFX,
# 0x40-0x5F LightFX, 0x60-0x7F GearControl) RETIRED — boards migrate
# in one PR each with no compatibility window.  The 0x01-0x7F space
# is now the single generic-expander range.  See
# instructions/15-GENERIC-EXPANDER-REFACTOR.md.

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
      - name: "types.go"
        purpose: "PacketType, ErrorCode types, name registry"
      - name: "stream.go"
        purpose: "Stream protocol (chunked data, CRC-16)"
      - name: "connection.go"
        purpose: "Serial connection, tag-correlated send/receive, stream waiters"
      - name: "core/core.go"
        purpose: "Core packet types, error codes (mirrors core/core.h)"
        modify_when: "Adding core packet types or error codes"
      - name: "gunfx/gunfx.go"
        purpose: "GunFX packet types, error codes, commands (mirrors gunfx.h)"
        modify_when: "Adding GunFX packet types, errors, or commands"
      - name: "lightfx/lightfx.go"
        purpose: "LightFX packet types, error codes, commands (mirrors lightfx.h)"
        modify_when: "Adding LightFX packet types, errors, or commands"
      - name: "gearcontrol/gearcontrol.go"
        purpose: "GearControl packet types, error codes, commands (mirrors gearcontrol.h)"
        modify_when: "Adding GearControl packet types, errors, or commands"
      - name: "hubfx/hubfx.go"
        purpose: "HubFX packet types, error codes, commands (mirrors hubfx.h)"
        modify_when: "Adding HubFX packet types, errors, or commands"
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
    engine:
      - name: "engine.go"
        purpose: "Core Engine struct (connection, API, dispatch, listener)"
      - name: "types.go"
        purpose: "CmdEntry, CmdGroup, InitReadyInfo, ControllerColors"
      - name: "output.go"
        purpose: "Output interface + ANSI terminal implementation"
      - name: "helpers.go"
        purpose: "Shared utilities (Atoi, ParseBool, ServoSet, ServoConfig)"
      - name: "parsers.go"
        purpose: "Common response parsers"
      - name: "parsers_core.go"
        purpose: "Core response parsers (INIT_READY, STATUS header)"
    engine_handlers:
      - name: "handlers/handlers.go"
        purpose: "RegisterDefaults() — registers all built-in groups"
      - name: "handlers/core/handler.go"
        purpose: "Core commands (connect, init, status, reboot, etc.)"
        modify_when: "Modifying core commands"
      - name: "handlers/gunfx/handler.go"
        purpose: "GunFX commands (trigger, servo, smoke)"
        modify_when: "Adding GunFX CLI commands"
      - name: "handlers/lightfx/handler.go"
        purpose: "LightFX commands (LED, sequences, servo, landing lights)"
        modify_when: "Adding LightFX CLI commands"
      - name: "handlers/lightfx/parsers.go"
        purpose: "LightFX response parsers"
      - name: "handlers/gearcontrol/handler.go"
        purpose: "GearControl commands (gear, servo, yaw, calibration)"
        modify_when: "Adding GearControl CLI commands"
      - name: "handlers/gearcontrol/parsers.go"
        purpose: "GearControl response parsers"
      - name: "handlers/hubfx/handler.go"
        purpose: "HubFX commands (slaves, audio, engine, storage, USB)"
        modify_when: "Adding HubFX CLI commands"
      - name: "handlers/hubfx/parsers.go"
        purpose: "HubFX response parsers"
      - name: "handlers/hubfx/format.go"
        purpose: "HubFX output formatting"
      - name: "handlers/firmware/handler.go"
        purpose: "Firmware release commands"
    cli:
      - name: "main.go"
        purpose: "Entry point, flag parsing"
      - name: "cli.go"
        purpose: "Terminal readline loop, delegates to engine"
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
│        Add to app/go/protocol/xxxfx/xxxfx.go
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
├─ NO → Add to app/go/protocol/xxxfx/xxxfx.go, api/xxxfx.go, engine/handlers/xxxfx/handler.go
│        - Add packet constant + register in init()
│        - Add typed API method
│        - Add CLI command + register
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
    primary: "lib/sfx_serial/serial/core/core.h"
    mirrors:
      - "app/go/protocol/core/core.go"
    rule: "Same values, same names"

  - name: "Error Codes (Generic)"
    primary: "lib/sfx_serial/serial/core/core.h (SerialError namespace)"
    mirrors:
      - "app/go/protocol/core/core.go (error constants + name map)"
    rule: "Same values, same names"

  - name: "Error Codes (Module)"
    primary: "lib/sfx_serial/serial/xxxfx/xxxfx.h (XxxError namespace)"
    mirrors:
      - "app/go/protocol/xxxfx/xxxfx.go (error constants + name map)"
    rule: "Same values, same names"

  - name: "Command Interface"
    primary: "lib/sfx_serial/serial/xxxfx/xxxfx.h"
    mirrors:
      - "app/go/protocol/xxxfx/xxxfx.go"
      - "app/go/engine/handlers/xxxfx/handler.go"
    rule: "All commands must be exposed in Go CLI"
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
| [09-CONSOLE-OUTPUT.md](09-CONSOLE-OUTPUT.md) | Console output schema for Go CLI |
| [10-UPLOAD-PROTOCOL-REFACTOR.md](10-UPLOAD-PROTOCOL-REFACTOR.md) | Upload protocol modes (stream, windowed), flow control, ring buffer architecture |
| [11-LANDING-LIGHT-GROUPS.md](11-LANDING-LIGHT-GROUPS.md) | Landing-light channel-mask group model (`LANDING_LIGHT_BIND` byte 2 = channelMask) |
| [12-LIGHTFX-MODERNIZATION.md](12-LIGHTFX-MODERNIZATION.md) | LightFX refactor record — battery monitoring, per-board config YAML, UI redesign (landed 2026-05) |
| [13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md) | Hub pass-through routing by packet-type range; board-prefix CLI disambiguation |
| [14-ONBOARD-COPROCESSOR.md](14-ONBOARD-COPROCESSOR.md) | On-board UART-attached expander (ESP32-C3 PWM expander pattern, master-bridged flashing) |
| [15-GENERIC-EXPANDER-REFACTOR.md](15-GENERIC-EXPANDER-REFACTOR.md) | Pivot to generic expander protocol — component collections (servo / PWM / LED), per-board migration plan |
| [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md) | Expander-board design contract — anatomy of a firmware, full core + expander protocol surface, persistence rules, migration recipe |
| [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md) | `CoreCommandServer<...ServicePolicies>` composition, deterministic board GUID (UUIDv5), per-port GUIDs, storage backends as policy |
| [18-HUBFX-INA-CLONE-WEDGE.md](18-HUBFX-INA-CLONE-WEDGE.md) | Investigation: counterfeit INA226 @ 0x40 corrupts PCA9685 @ 0x70 on writes; gate on canonical IDs |
| [19-HUBFX-CONFIG-SCHEMA.md](19-HUBFX-CONFIG-SCHEMA.md) | `/hubfx.yaml` schema — expander aliases (alias→GUID + ports), effect sub-files reference ports by alias |
| [20-STUDIO-DEVICE-MODEL.md](20-STUDIO-DEVICE-MODEL.md) | Studio's authoritative device model in Go (`devicemodel/`) — port/role/claim semantics, validation, presets |
| [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md) | **Reference design for Studio effect tabs** — panel anatomy, channel-setup cluster (bar with threshold marker + hysteresis band), sound rows, validation lattice, dirty-draft state. Cribbed from for any new operational effect tab. |
| [22-GUNFX-FEATURE-ROLLOUT.md](22-GUNFX-FEATURE-ROLLOUT.md) | GunFX rollout record (LANDED 2026-05-23) — port voltage, effect clock, multi-band ROF, manual/puppet mode, verbose status, role-layer actuators. §0 architecture still authoritative. |
| [23-STUDIO-WIDGET-CATALOG.md](23-STUDIO-WIDGET-CATALOG.md) | **Start here for Studio UI work** — handbook of every reusable widget pattern with copy-pasteable snippets |
| [24-COREDUMP-DEBUGGING.md](24-COREDUMP-DEBUGGING.md) | Pull + decode an ESP32-S3 flash coredump (`scalefx-flash coredump hubfx`); the measure-don't-guess discipline for panics |
| [25-ARDUINO-REMOVAL.md](25-ARDUINO-REMOVAL.md) | Record of the HubFX Arduino→pure-ESP-IDF migration (COMPLETE) — native abstraction map; reference for the future Pico-SDK migration (P8) |
| [26-CODE-AND-DESIGN-IMPROVEMENTS.md](26-CODE-AND-DESIGN-IMPROVEMENTS.md) | Catalogue of code-quality observations from the arduino-removal audit; most items DONE, remainder are Pico-side follow-ups |
| [27-WIRE-ASYNC-AND-UPLOAD.md](27-WIRE-ASYNC-AND-UPLOAD.md) | Wire multiplexing discipline (Rules 53–57) — lossy vs flow-control async, stream-upload exclusivity, thread-safe `Connection`, upload diagnostics, UART RX FIFO tuning |
| [28-IO-FLUSH-DEBUGGING.md](28-IO-FLUSH-DEBUGGING.md) | Methodology for low-level I/O flush bugs — measure each buffer boundary, don't infer; sent ≠ delivered ≠ persisted |
