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

Task: "Understand the system end-to-end (storage / audio / ports-roles-topology / effects→ports)"
  → Read: 32-ARCHITECTURE-DIAGRAMS.md   ← Mermaid diagrams of the four core subsystems
  → Then: 01-ARCHITECTURE.md (the prose reference)

Task: "Compose a board's system services (storage / audio / battery / config / USB host / expander bus)"
  → Read: 17-SYSTEM-SERVICES.md
  → Covers: BoardServer<...ServicePolicies> composition, deterministic board GUID,
            per-port GUIDs, storage backends as variadic policy, IDENTIFY payload extension

Task: "Work on HubFX"
  → Target: controllers/hubfx/esp32s3/  (ESP32-S3, PURE ESP-IDF — no Arduino; HubFX Pico is OBSOLETE)
  → Reference: controllers/hubfx/pico/ (frozen, consult for patterns only)
  → Read: 01-ARCHITECTURE.md + 32-ARCHITECTURE-DIAGRAMS.md

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

Dispatch_Map:
  note: |
    NO per-board packet ranges. The hub owns one conflict-validated map; each
    *ServicePolicy claims its bytes via ownsType() and dispatch STOPS at the
    first owner. AUTHORITATIVE allocation + drift warning: CLAUDE.md /
    .github/copilot-instructions.md → "HubFX master dispatch map". Highlights:
  Ports:    "0x10-0x3F"
  Roles:    "0x40-0x7F"
  Expander: "0x80-0x87"
  Topology: "0x88-0x8F + 0xA6-0xA7"   # role forward/query/event/bulk-attach (Rule 58)
  Storage:  "0x93-0xA5 + 0xA9 + 0xB0"
  Audio:    "0xDA-0xE1"
  Effects:  "LandingLight 0xB1-0xB6 · LightFX 0xB7-0xBD · GearControl 0xBE-0xC6 ·
             EngineFX 0xC7-0xCB · GunFX 0xCC-0xD2 + 0xE2-0xE5 · Alerts 0xD3-0xD6"
  Core:     "0xEF-0xFF"

Controllers:
  hubfx:       { path: "controllers/hubfx/esp32s3/", platform: "ESP32-S3, PURE ESP-IDF (no Arduino)",
                 role: "master — runs every effect" }
  lightfx:     { path: "controllers/lightfx/pico/", platform: "RP2040",
                 role: "thin port+role expander (8 PWM / 3 servo + ADC battery)" }
  gearcontrol: { path: "controllers/gearcontrol/pico/", platform: "RP2040",
                 role: "thin port+role expander (7 servo / 3 H-bridge + INA226 stall)" }
# REMOVED: standalone gunfx (effects live on the hub), noop / noop-esp.
# controllers/gunfx/pico/ and controllers/noop/ do NOT exist.
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
      purpose: "ConfigStore<TSchema> — schema-driven YAML load / validation / defaults"
      modify_when: "Changing config load/save lifecycle or adding config infrastructure"
    - name: "server/multi_config_server.h"
      purpose: "Multi-store path-routed config server backing the ConfigServicePolicy (CONFIG_RELOAD / SAVE / STATUS)"
      modify_when: "Adding new config protocol commands"

Wire_Library:
  root: "controllers/lib/sfx_serial/serial/"
  files:
    - name: "serial.h"
      purpose: "Umbrella header (include this)"
    - name: "core/core.h"
      purpose: "CommandHandleResult, SerialError, CorePacket, SFX_REQUIRE_LEN / SFX_VALIDATE macros"
      modify_when: "Adding generic error codes, handler macros, or core protocol"
    - name: "wire.h / wire.cpp"
      purpose: "SfxWire — CRC-8 / COBS / endian helpers (getU16LE/putU16LE)"
      modify_when: "Changing wire format or encoding utilities"
    - name: "packet_reader.h"
      purpose: "PacketReader — byte-stream → framed packet"
      modify_when: "Never (stable)"
    - name: "diag_log.h / .cpp"
      purpose: "DiagLog singleton — LOG_MESSAGE / DIAG_HISTORY over the wire"
      modify_when: "Changing log format / ring buffer"
    - name: "client/bus.h"
      purpose: "SerialBus — client-side COBS transport (master USB-host side)"
      modify_when: "Never (stable transport)"
    - name: "client/result_queue.h"
      purpose: "ResultQueue — tag-correlated command/response matching"
      modify_when: "Never (stable infrastructure)"

Board_Framework:
  root: "controllers/lib/sfx_board/server/"
  files:
    - name: "board_server.h"
      purpose: "BoardServer<TStream, ...UserPolicies> — the single board composer"
      modify_when: "Rarely — the framework core"
    - name: "board_of.h"
      purpose: "BoardOf<TBoard, TStream, PortCapacity<…>, …> — port-aware expander shorthand"
      modify_when: "Adding a port kind or board-wiring behaviour"
    - name: "board_service.h"
      purpose: "BoardServicePolicy — INIT / STATUS / IDENTIFY / KEEPALIVE lifecycle (auto-prepended)"
      modify_when: "Changing the core lifecycle or STATUS layout"
    - name: "port_service.h / role_service.h"
      purpose: "PortServicePolicy (port registry) + RoleServicePolicy (attach + drive roles)"
      modify_when: "Adding a role kind or port behaviour"
    - name: "role_registry.h"
      purpose: "roleKindFor<>() / forEachAttachedRole — the ONE role enumeration map (Rule 58)"
      modify_when: "Exposing a new role (one line here)"
    - name: "effect_clock.h"
      purpose: "EffectClock singleton — latched once per process() (Rule 40)"
      modify_when: "Never"

Hub_Effects:
  root: "controllers/hubfx/esp32s3/src/effects/<mod>/"
  files:
    - name: "<mod>_protocol.h"
      purpose: "Packet types + error codes for the effect"
      modify_when: "Adding commands / error codes"
    - name: "<mod>_service.h / .ipp"
      purpose: "The effect's *ServicePolicy (ownsType / handle / update)"
      modify_when: "Adding command handling or effect logic"

Pico_Boards:
  pattern: "controllers/{lightfx,gearcontrol}/pico/"
  files:
    - name: "src/{name}_pico.ino"
      purpose: "Thin board sketch — board class via BoardOf<…> + setup/loop"
    - name: "platformio.ini"
      purpose: "Build configuration (Arduino-Pico)"
    - name: "README.md"
      purpose: "Pinout + exposed ports"
```
```yaml
Go_stack:
  root: "app/go/"
  note: |
    Layering: protocol/<mod> (wire mirror, SOURCE OF TRUTH) → client/<mod> (typed API)
    + client/roletarget.go (transparent role I/O) → console/cmd_<mod>.go (CLI).
    The string-command "engine" + engine/handlers/<mod>/{types,format,handler,parsers}.go
    and app/go/api/ are REMOVED (archived 2026-05-28). Do NOT reference them.
  packages:
    protocol:
      - name: "wire.go"
        purpose: "CRC-8/16, COBS, BuildPacket / ParsePacket, endian helpers"
      - name: "connection.go"
        purpose: "Serial connection, tag-correlated send/recv, async filters (Rules 53,56)"
      - name: "core/core.go"
        purpose: "Core packet types, error codes, capability flags, controller-type strings"
        modify_when: "Adding core packet types, error codes, or capability bits"
      - name: "<mod>/<mod>.go"
        purpose: "Per-module wire mirror — packet consts, CmdXxx, DecodeXxx, errors"
        modify_when: "Adding/changing ANY packet, command, or error (source of truth)"
      - name: "roles/*.go"
        purpose: "Role codecs (opaque-forwarded by the hub, Rule 58)"
        modify_when: "Exposing a new role"
    client:
      - name: "client.go"
        purpose: "Client — owns protocol.Connection + every typed sub-API"
      - name: "<mod>.go"
        purpose: "Typed API (Gun, Gear, Landing, Audio, Storage, Topology, Config, …)"
        modify_when: "Adding a typed command method"
      - name: "roletarget.go"
        purpose: "RoleTarget — c.Role(guid) GUID-transparent role drive/query (Rule 58)"
      - name: "events.go"
        purpose: "Async telemetry stream (OnRole / OnXxx subscriptions)"
    console:
      - name: "registry.go"
        purpose: "Central command table + categories (register() / lookup())"
      - name: "session.go"
        purpose: "App struct (holds *client.Client), REPL, requireClient()"
      - name: "helpers.go / term.go"
        purpose: "Arg parsers (parseU8, parseOnOff, …) + output helpers (Ok, Note, Hdr, …)"
      - name: "cmd_<mod>.go"
        purpose: "ONE file per wire-domain; self-registers its commands in init()"
        modify_when: "Adding a CLI command (see 07-CLI-UPDATES.md)"
  entrypoints:
    - "cli/    — scalefx-cli.exe"
    - "flash/  — scalefx-flash.exe"
    - "studio/ — Wails v2 GUI (Svelte frontend/)"
```

---

## Decision Trees

### When Adding a Command

Full worked example: [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md). `<mod>` = effect/subsystem (gunfx, gear, landing, audio, …).

```
START: Need to add a command to a hub effect / subsystem

Q1: Claim a packet byte
└─ Add the const in effects/<mod>/<mod>_protocol.h at the next FREE byte.
   Validate vs the dispatch map in CLAUDE.md AND grep the real ownsType()
   predicates (the map drifts — first-owner-wins means a collision is swallowed).

Q2: New error code?
└─ Add it in the module's error block (CLAUDE.md error ranges) + getMessage(),
   and mirror it same-valued in protocol/<mod>/<mod>.go (error_collisions_test guards this).

Q3: Firmware handling
└─ Add the byte to ownsType(); add the case to handle() (SFX_REQUIRE_LEN /
   SFX_VALIDATE → do work → return Ack()/Nack(), OR send a typed RESP + return Handled).
   Response category? INSTANT = Ack/Nack · QUERY = typed RESP · LONG-RUNNING = Ack then
   finish across update() ticks (completion via STATUS / async event).

Q4: Go source of truth
└─ protocol/<mod>/<mod>.go: packet const (same value) + CmdXxx builder (+ DecodeXxx if query).

Q5: Typed API
└─ client/<mod>.go: a method wrapping sendExpectACK (instant) or sendForResp (query).

Q6: CLI
└─ console/cmd_<mod>.go: register(&command{…}) in init() + a cmd* func calling the typed API.

Q7: Docs + build
└─ Update the CLAUDE.md dispatch map. Then:
   scalefx-flash build hubfx --no-clean  &&  cd app/go && go build ./...
```

---

## Mandatory File Sync Points

**CRITICAL:** every wire change touches both halves. The Go side is the source of truth for the master protocol; `cd app/go && go build ./...` is the sync check.

```yaml
Sync_Groups:
  - name: "Packet Constants + Command Builders"
    firmware: "effects/<mod>/<mod>_protocol.h  (or lib/sfx_board/ for roles/infra)"
    go_mirror:
      - "app/go/protocol/<mod>/<mod>.go (packet const + CmdXxx + DecodeXxx)"
    rule: "Same values; every command exposed via client/<mod>.go + console/cmd_<mod>.go"

  - name: "Error Codes"
    firmware: "<mod>_protocol.h error block (per CLAUDE.md ranges) + getMessage()"
    go_mirror:
      - "app/go/protocol/<mod>/<mod>.go (same value + name map)"
    rule: "Same values; one namespace per range (error_collisions_test in the pre-merge gate)"

  - name: "Capabilities"
    firmware: "policy kCapabilityBits → IDENTIFY caps word"
    go_mirror:
      - "app/go/protocol/core/core.go (CapXxx + HasCapability)"
    rule: "Used to gate CLI commands (RequiresCap) + Studio probes"
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
| [17-SYSTEM-SERVICES.md](17-SYSTEM-SERVICES.md) | `BoardServer<...ServicePolicies>` composition, deterministic board GUID, per-port GUIDs, storage backends as policy |
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
| [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md) | **Mermaid diagrams of the four core subsystems** (storage / audio / ports-roles-topology / effects→ports) on the current `BoardServer<...UserPolicies>` codebase — read alongside 01-ARCHITECTURE.md |
