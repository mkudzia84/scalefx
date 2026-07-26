# ScaleFX Agent Instructions

> **FOR AI AGENTS:** Start here. `instructions/` holds the numbered, in-depth workflow
> and subsystem guides. The compact rulebook + navigation index is
> [CLAUDE.md](../CLAUDE.md); the **authoritative rule source** (Rules 0–58, rationale +
> examples) is [.github/copilot-instructions.md](../.github/copilot-instructions.md) —
> when a guide here conflicts with it, that file wins.

> **Visual starting point:** **[32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md)** —
> Mermaid diagrams of the four core subsystems (storage / audio / ports-roles-topology /
> effects→ports) on the current `BoardServer<...UserPolicies>` codebase. Read it before the
> prose docs to get the shape of the system fast.

---

## Find the doc for your task

| Task | Doc |
|------|-----|
| Understand the system end-to-end (visual) | [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md) → [01-ARCHITECTURE.md](01-ARCHITECTURE.md) |
| Add a command to an existing hub effect / subsystem | [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) + [04-CHANGE-PROPAGATION.md](04-CHANGE-PROPAGATION.md) |
| Create a new controller / service policy | [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) |
| Design / audit an expander board firmware | [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md) |
| Build + flash firmware | [05-BUILD-AND-FLASH.md](05-BUILD-AND-FLASH.md) |
| Add a CLI command | [07-CLI-UPDATES.md](07-CLI-UPDATES.md) |
| Build a Studio effect tab / widget | [23-STUDIO-WIDGET-CATALOG.md](23-STUDIO-WIDGET-CATALOG.md) → [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md) |
| Debug a firmware panic | [24-COREDUMP-DEBUGGING.md](24-COREDUMP-DEBUGGING.md) |
| Debug an upload stall / wire collision | [27-WIRE-ASYNC-AND-UPLOAD.md](27-WIRE-ASYNC-AND-UPLOAD.md) + [28-IO-FLUSH-DEBUGGING.md](28-IO-FLUSH-DEBUGGING.md) |
| Edit the `/hubfx.yaml` config schema | [19-HUBFX-CONFIG-SCHEMA.md](19-HUBFX-CONFIG-SCHEMA.md) |

---

## Architecture & framework

| Doc | What it covers |
|-----|----------------|
| [32-ARCHITECTURE-DIAGRAMS.md](32-ARCHITECTURE-DIAGRAMS.md) | **Start here.** Mermaid diagrams of the four core subsystems (storage / audio / ports-roles-topology / effects→ports) on the post-decomposition `BoardServer<...UserPolicies>` codebase. |
| [01-ARCHITECTURE.md](01-ARCHITECTURE.md) | The prose reference — system topology, packet format, policy class hierarchy, response categories. HubFX master runs every effect; Pico boards are thin port+role expanders. |
| [16-EXPANDER-BOARD-DESIGN.md](16-EXPANDER-BOARD-DESIGN.md) | Expander-board design contract on the current Ports/Roles model — `BoardOf<...>` + static port descriptors + `RoleServicePolicy`; how the hub drives a board's roles transparently (Rule 58). |

## Workflows (how-to)

| Doc | What it covers |
|-----|----------------|
| [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) | Create a new physical expander board (Pico, via `BoardOf<…>`) **or** a new protocol-exposed subsystem (`*ServicePolicy`) composed into a board's `BoardServer<…>`. |
| [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) | Add a command to an existing hub effect / subsystem — worked example through firmware policy + the Go mirror (source of truth). |
| [04-CHANGE-PROPAGATION.md](04-CHANGE-PROPAGATION.md) | File-sync checklists + change-type matrix; verify every affected half (firmware ↔ Go mirror) after any wire change. |
| [05-BUILD-AND-FLASH.md](05-BUILD-AND-FLASH.md) | Compile firmware + deploy to hardware via `scalefx-flash`; build/flash/release commands + troubleshooting. |
| [07-CLI-UPDATES.md](07-CLI-UPDATES.md) | Add commands to the Go interactive CLI (`app/go/console/` over `client/` over `protocol/`). |

## Protocol & subsystems

| Doc | What it covers |
|-----|----------------|
| [09-CONSOLE-OUTPUT.md](09-CONSOLE-OUTPUT.md) | Console output schema for the Go CLI (the canonical reference) — size formatting, layout conventions every parser must match. |
| [11-LANDING-LIGHT-GROUPS.md](11-LANDING-LIGHT-GROUPS.md) | Named multi-channel landing-light group model (one optional servo + N LED channels); `LANDING_LIGHT_BIND` byte 2 = channelMask. |
| [19-HUBFX-CONFIG-SCHEMA.md](19-HUBFX-CONFIG-SCHEMA.md) | `/hubfx.yaml` schema + `HubFxConfigServicePolicy` — expander aliases (alias→GUID + ports), effect sub-files referencing ports by alias. |
| [27-WIRE-ASYNC-AND-UPLOAD.md](27-WIRE-ASYNC-AND-UPLOAD.md) | Wire-multiplexing discipline (Rules 53–57) — lossy vs flow-control async, stream-upload exclusivity, thread-safe `Connection`, upload diagnostics, UART RX FIFO tuning. |
| [29-GEARCONTROL-UNDERCARRIAGE.md](29-GEARCONTROL-UNDERCARRIAGE.md) | Retractable-undercarriage door sequencing on the HubFX + Studio Gear tab — door servos, multi-channel coordination, `/gearcontrol.yaml` v2 (firmware landed 2026-06-07). |

## Studio (GUI)

| Doc | What it covers |
|-----|----------------|
| [23-STUDIO-WIDGET-CATALOG.md](23-STUDIO-WIDGET-CATALOG.md) | **Start here for Studio UI work** — handbook of every reusable widget pattern with copy-pasteable snippets, cross-referenced to the formal rules. |
| [21-STUDIO-ENGINEFX-PANEL.md](21-STUDIO-ENGINEFX-PANEL.md) | Reference design for operational effect tabs — panel anatomy, channel-setup cluster (threshold + hysteresis bar), sound rows, validation lattice, dirty-draft state. Crib for any new effect tab. |
| [20-STUDIO-DEVICE-MODEL.md](20-STUDIO-DEVICE-MODEL.md) | Studio's authoritative device model in Go (`devicemodel/`) — port/role/claim semantics, validation, presets. The frontend renders + edits this one source of truth. |
| [33-CONFIG-WIZARD.md](33-CONFIG-WIZARD.md) | Guided Setup Wizard (modal stepper over the draft stores: features → input → channel map → one step per enabled effect with auto-attaching ports → review/apply) + the design for a Claude-powered config assistant. As-built tree + the no-TS-in-markup gotcha in §10. |
| [34-HUBFX-PERF-AUDIT.md](34-HUBFX-PERF-AUDIT.md) | DRAM/PSRAM budget + decoder-pool sizing + speed folds (mix kernel already esp-dsp SIMD) + alignment — findings and a priority-ordered optimization plan (2026-07-15 overnight audit). |

## Debugging & gotchas

| Doc | What it covers |
|-----|----------------|
| [18-HUBFX-INA-CLONE-WEDGE.md](18-HUBFX-INA-CLONE-WEDGE.md) | Investigation + fix: a counterfeit INA226 @ I²C `0x40` corrupts the PCA9685 @ `0x70` on writes; the driver now refuses chips failing the canonical TI ID check. |
| [24-COREDUMP-DEBUGGING.md](24-COREDUMP-DEBUGGING.md) | Pull + decode an ESP32-S3 flash coredump (`scalefx-flash coredump hubfx`); the measure-don't-guess discipline for panics. |
| [28-IO-FLUSH-DEBUGGING.md](28-IO-FLUSH-DEBUGGING.md) | Methodology for low-level I/O flush bugs — localize the failing buffer boundary empirically; sent ≠ delivered ≠ persisted. |
| [08-AUDIOTOOLS.md](08-AUDIOTOOLS.md) | AudioTools (`pschatzmann/arduino-audio-tools`) library reference + known bugs (broken `InputMixer<float>`, `SineWaveGenerator` amplitude) — keep the pipeline in `int16_t`. |

## Hardware

| Doc | What it covers |
|-----|----------------|
| [99-HW-TODO.md](99-HW-TODO.md) | Hardware + feature TODO backlog — firmware feature refactors (e.g. `GunDef` → `GunWiring` + `GunPreset` preset library) and bring-up notes. |

---

## Critical constants

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
    .github/copilot-instructions.md → "HubFX master dispatch map". Always grep
    the real ownsType() predicates before claiming a byte — the map drifts.

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

> **All development rules, patterns, and checklists are in
> [.github/copilot-instructions.md](../.github/copilot-instructions.md)** (auto-loaded by
> VS Code Copilot). This document + [CLAUDE.md](../CLAUDE.md) are navigation only — detailed
> rules are not duplicated here. After any wire change: `cd app/go && go build ./...` is the
> sync check (the Go side is the source of truth for the master protocol).
