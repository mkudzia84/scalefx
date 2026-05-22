# ScaleFX Studio

Wails v2 desktop GUI (Go backend + Svelte/TS frontend) for configuring and
controlling a ScaleFX HubFX master and the expanders behind it. Built on
the shared `scalefx/client` typed API — the same one `scalefx-cli` uses.

```
wails dev      # hot-reload dev server (Go methods exposed at :34115)
wails build    # production exe → build/bin/scalefx-studio.exe
```

After connecting, a diagnostic log is written to
`%TEMP%\scalefx-studio.log` (see [DIAG.md](DIAG.md)).

---

## 1. Architecture in one paragraph

The **device model is authoritative and lives in Go**
([`app/go/devicemodel/`](../devicemodel/)). The frontend never holds model
logic — it renders a snapshot and edits it through Wails bindings; every
mutation round-trips to Go and replaces the reactive store from the
returned snapshot. There is exactly one source of truth, mirrored to the
frontend as the `deviceModel` store. The legacy per-board `engine` +
`handlers` framework was retired with the generic-expander migration; the
old tabs are archived under `frontend/src/lib/_archive/`.

```
firmware (topology: ports + roles)                ┌─ PortRoleTab (output ports)
        │  PORT_LIST / ROLE_LIST                   ├─ InputPanel (input + channels)
        ▼                                          ├─ DomainTab (functional slots)
 devicemodel.Model  ──snapshot──►  deviceModel ───►├─ PcbOverlayDialog (Diagram)
   (Go, guarded)     (Wails)        store (TS)      └─ StatusBar / etc.
        ▲                                  │
        └─────── AttachRole / Claim / ─────┘   (every edit returns a fresh
                 SetInputProtocol / …          snapshot + emits devicemodel:changed)
```

Because every view subscribes to the one `deviceModel` store, a change made
anywhere (e.g. assigning a role on the Diagram) updates everywhere
reactively — the main list recolors, the functional tabs re-run their
candidate query, the Diagram's "used by" line updates.

---

## 2. Core concepts

| Concept | What it is | Where |
|---|---|---|
| **Port** | a physical port: kind (servo/pwm/hbridge/input) + index, with derived caps, attached role, hardware label, allowed roles, operator name | `devicemodel.Port` |
| **Role attachment** | a behaviour bound to a port (`servo-actuator`, `led-animator`, `rc-pwm-input`…). Firmware truth (`ROLE_LIST`), one role per port | wire |
| **Domain** | a functional area (Landing Lights, Gear, Engine, Gun, Lighting) with capability gate + **slots** | `domainCatalog` |
| **Domain claim** | a domain *uses* a port for a slot. UI/config concept. **OUTPUT ports are exclusive; INPUT ports are shareable** (one switch → lights + gear) | model |
| **Hardware label** | silkscreen id (`CH3`, `SRV1`, `IN1`, `HB2`), derived from kind+index | `HardwareLabel` |
| **Board display name** | friendly board label (HubFX, LightFX, **LightFX #2** when two of a kind). GUIDs are internal addressing only | `boardDisplayNames` |
| **Input config** | per-input-port protocol + channel count + channel→function map | `InputPortConfig` |

---

## 3. Backend (`app/go/`)

### `devicemodel/` — pure, UI-agnostic, unit-tested

- **`types.go`** — `Port`, `PortRef`, `Direction` (Rule 31), `RoleLabel`,
  `AllowedRoleOptions` (per-kind), `HardwareLabel`, `Domain`/`Slot`, and the
  declarative **`domainCatalog`** (each domain carries its `core.Cap*`
  gate).
- **`model.go`** — `Model` (ports + claims), `BuildModel(portList,
  roleList)`, `Claim`/`Unclaim`, `Candidates(domain, slot)` (legal ports a
  slot may select), `SetRole` (optimistic).
- **`validate.go`** — `Validate() → []Issue` (role mismatch, exclusive-
  output conflict, slot cardinality, claimed-but-unattached). Zero
  `SevError` = safe to apply.
- **`input.go`** — `InputProtocol` catalog (PPM implemented; SBUS/Jeti
  catalogued-but-disabled), the **channel-function catalog**
  (`channelFunctions` — Flight/Helicopter/Fixed-wing + HubFX features:
  Engine/Lights/Gear/Gun/Audio), `InputPortConfig`.
- **`presets.go`** — symbolic presets (board selector + kind + index)
  resolved against live topology into attaches + claims; missing boards
  skipped with warnings.
- **`defaults.go` + `defaults/*.yaml`** — embedded per-board-kind default
  profiles (role attaches + claims + input channel map), applied by
  **★ Defaults**.
- **`devicemodel_test.go`** — shared-input fan-out, exclusive-output,
  role mismatch, preset resolution, capability gating.

### `studio/` — Wails bindings + lifecycle

- **`app.go`** — the single Wails-bound `App`. Connection lifecycle
  (`Connect`/`Disconnect`, `openLocked`/`closeLocked` via
  `client.Connect()` with a **5 s** timeout for topology aggregation),
  async diag through `c.Events.OnAny`, console echoes, `/diag` slash
  commands. Holds `dm` (model, `dmMu`), `inputs`, `portNames` overlays.
- **`port_watcher.go`** — connection-agnostic serial-port poller; fires
  `OnChange` (→ `ports:changed`) and `OnVanished(port)` → App tears down
  the client → `connection:changed{connected:false}` → the connect dialog
  re-shows. This is how an **unplug returns you to reconnect**.
- **`app_devicemodel.go`** — `RefreshDeviceModel` (resilient: keeps the
  previous model on a topology timeout instead of blanking), `ClaimPort`/
  `UnclaimPort`/`CandidatePorts`, `AttachRole`/`DetachRole`,
  `SetPortName`, `ApplyPreset`, servo auto-attach. Wire I/O runs on a
  client snapshot taken under `a.mu` and released **before** the call.
  Emits `devicemodel:changed`.
- **`app_input.go`** — `SetInputProtocol` (attaches the realizing role +
  starts the broadcast), `SetInputChannelCount`, `SetChannelFunction`,
  `ApplyDefaults`, and the **live-value stream**: on connect it tells the
  hub to broadcast RC values (`Input.SetBroadcastHz`, 10 Hz) and forwards
  decoded frames to the frontend as `input:values`.
- **`app_console.go` / `app_files.go` / `app_config.go` /
  `app_firmware.go` / `app_topology.go` / `diag.go`** — console bridge
  (ANSI→HTML), file manager, config round-trip, build/flash/releases,
  capability + system-info, diagnostics.

### `client` Input facet

`client/input.go` adds `Input.SetBroadcastHz` and decodes RCIN (single
PPM channel today), SBUS, and Jeti frames (direct or unwrapped from a
topology role event) into `InputValue`, surfaced via
`Events.OnInputValue`.

---

## 4. Frontend (`studio/frontend/src/`)

- **`App.svelte`** — root: installs the diag + device-model + input-value
  bridges, loads catalogs, wires `connection:changed` (connect → refresh
  model; disconnect → connect dialog), renders `MainLayout` + overlay
  dialogs.
- **`lib/stores.ts`** — UI state writables (`connectionInfo`,
  `connectPopupOpen`, `showPcbOverlay`, …).
- **`lib/devicemodel.ts`** — typed DTO mirror, the `deviceModel` store,
  binding wrappers, `normalize()` (coerces nil slices to `[]` — a nil from
  Go marshals as `null` and iterating it breaks Svelte reactivity), the
  `studioTabs` + `validationCounts` derived stores, `boardDisplayNames`,
  `liveChannels` store + `installInputValuesBridge`.
- **`lib/pcb.ts`** — board-photo overlay layout: `PortMarker` (kind, index,
  label, x%, y%) + `InfoMarker` (read-only labels e.g. speaker outputs).
  Coordinates are **measured** by `tools/analyze_hubfx.go`, not eyeballed.
- **`lib/layout/`** — `MainLayout` (renders the active tab + console pane),
  `TabBar` (functional icons, validation badge), `StatusBar`.
- **`lib/tabs/`** — `IoTab` (two-column shell), `InputPanel`,
  `PortRoleTab`, `DomainTab` (generic), `FirmwareTab`.
- **`lib/components/PortControls.svelte`** — shared role-picker + name
  field (servo → fixed "Servo" tag + name). Used by the Diagram popover.
- **`lib/dialogs/`** — `ConnectDialog`, `PcbOverlayDialog` (the Diagram),
  `FileManagerDialog`, `FlashProgressDialog`, `ConsoleDialog`, etc.

---

## 5. UI tour

### Connect dialog
Lists enumerated serial ports (no virtual/TCP option — that was removed).
Connecting runs IDENTIFY and drops into the main view. **Losing the port
returns here automatically.**

### Tab strip
Derived from the model: **Input & Ports**, then one tab per
capability-available **domain**, then **Firmware**. Each tab has an icon;
a validation badge shows error/warning counts.

### Input & Ports (the home tab — two columns)
- **Left — Input** (`InputPanel`): RC **protocol** (PPM enabled; SBUS/Jeti
  shown disabled), **channel count**, and per channel a **function**
  dropdown (grouped: Flight / Helicopter / Fixed-wing / Engine / Lights /
  Gear / Gun / Audio) with a **live value bar underneath** that shows a
  striped **NO SIGNAL** when there's no valid frame.
- **Right — Ports & Roles** (`PortRoleTab`): the board's **output** ports
  (input ports live in the left column) grouped by board, each with a role
  picker (servo = fixed tag + name) and an operator name. Header buttons:
  **Presets** dropdown + Apply, **★ Defaults** (apply bundled profile),
  **↻ Refresh**, **▣ Diagram**.

### Diagram (`PcbOverlayDialog`)
The hub's **board photo alone** with an interactive marker on each port at
**measured** coordinates (left = CH1–CH8, right = IN_12…IN_1 column;
speaker outputs marked read-only in blue at the top). Markers are **red
when unassigned, blue when a role is attached**; hover highlights, click
opens a popover with the **current role + "used by" functions + role/name
controls** (and an ✕). It edits the same model as the list.

> **Gotcha:** the hub's own ports come back tagged with the hub's **GUID**
> (not `""`), so the Diagram matches markers to the board whose kind equals
> `connectionInfo.controllerType` — not to `guid === ''`.

### Domain tabs (`DomainTab`, generic)
One per available domain. Renders the domain's **slots**; each slot lists
its claimed ports + a picker of legal **candidates** (ports whose attached
role satisfies the slot). Pure model rendering — no bespoke code per
domain.

### Console
Drives the same `scalefx/console` command set as the CLI, rendered with
ANSI→HTML colour. Connect/disconnect are handled by Studio's own controls.

---

## 6. Recipes

**Add a functional domain:** add a `DomainID` + `Domain{}` (slots, role
kinds, cap) to `domainCatalog` in `devicemodel/types.go`. The tab appears
automatically (capability-gated), the picker is driven by `Candidates`,
validation is automatic. Build a bespoke `*.svelte` only for live control
beyond port assignment.

**Add a board to the Diagram:** drop `<board>_top.png` into
`media/pcb/`, copy to `frontend/src/assets/pcb/`, run
`go run tools/analyze_<board>.go` (port of the Python silkscreen detector —
colour-mask + flood-fill → centroids) to measure connector positions, and
add a `BoardPcb` entry to `lib/pcb.ts`.

**Add a default profile:** add `devicemodel/defaults/<kind>.yaml` (role
attaches + claims + input channel map). It's embedded via `//go:embed` and
applied by ★ Defaults.

**Add a channel function / protocol:** edit `channelFunctions` /
`inputProtocols` in `devicemodel/input.go`.

---

## 7. Design system

All UI composes the shared classes in `frontend/src/style.css` (`button`,
`.field-input`, `.card`, `.form-row`, `.banner`, CSS vars). Component
`<style>` does *layout only* — never re-skins controls. See **Rule 34** in
[copilot-instructions.md](../../../.github/copilot-instructions.md) and
[instructions/20-STUDIO-DEVICE-MODEL.md](../../../instructions/20-STUDIO-DEVICE-MODEL.md).

> Svelte gotcha: template expressions can't parse TS `as` casts — read
> `e.target` through a script helper (`selValue(e)`), never inline
> `(e.target as HTMLSelectElement)` in markup.

---

## 8. Known gaps

- **Per-domain live control** (servo jog, LED sequence builder, gear
  deploy) isn't built yet — domain tabs do port assignment only.
- **Claim persistence** to `/hubfx.yaml` effect sub-files is unbuilt
  (claims are in-memory).
- **Multi-channel PPM live values**: firmware emits one channel for PPM
  today, so only CH1's bar moves on a PPM input (SBUS/Jeti decoders are
  ready). Gun yaw/pitch/retract are catalogued but not in firmware.
