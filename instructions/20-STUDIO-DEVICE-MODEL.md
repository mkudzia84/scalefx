# 20 — Studio Device Model & Tab Re-architecture

> Full GUI tour (tabs, Diagram, input panel, recipes): **[app/go/studio/README.md](../app/go/studio/README.md)**. This file covers the model architecture + rules.

ScaleFX Studio's GUI is built around an **authoritative device model** that
lives in Go (`app/go/devicemodel/`), not in the Svelte frontend. The
frontend renders the model and edits it through Wails bindings; all
validation, claim semantics, and presets resolve in Go so there is exactly
one source of truth. This replaces the retired per-board tab/slave-STATUS
design (archived under `studio/frontend/src/lib/_archive/`).

## Two relationships — never conflate them

| | What | Owner | Cardinality |
|---|---|---|---|
| **Role attachment** | a behaviour bound to a port (`servo-actuator`, `led-animator`, `rc-pwm-input`…) | firmware (`ROLE_LIST_RESP`) | one role per port |
| **Domain claim** | a functional area (landing lights, gear…) *uses* a port | Studio/config (UI concept) | OUTPUT exclusive · INPUT shared |

The pivotal rule: **output ports are claimed exclusively** (one domain),
**input ports are shareable** — a single landing switch feeds both the
landing-lights and the landing-gear domains. The model enforces this.

## `app/go/devicemodel/` (pure, UI-agnostic, tested)

- `types.go` — `PortRef`, `Port` (with derived capability tokens +
  attached role), `Direction` (output/input from port kind, Rule 31),
  `Domain`/`Slot`, and the **declarative `domainCatalog`**. Adding a
  functional domain or slot here is all the model-side wiring a new tab
  needs. Each domain carries the `core.Cap*` bit that gates its
  availability.
- `model.go` — `Model` (ports + claims), `BuildModel(portList, roleList)`,
  `Claim`/`Unclaim`, and `Candidates(domain, slot)` — the legal ports a
  slot may select (right direction, role-kind match, output not already
  claimed elsewhere).
- `validate.go` — `Validate()` → `[]Issue` (error/warn): role mismatch,
  exclusive-output conflict, slot cardinality, claimed-but-unattached. A
  model with zero `SevError` is safe to apply/persist.
- `presets.go` — symbolic presets (`BoardSel` + kind + index) resolved
  against live topology into role attachments + claims. Missing boards are
  skipped with warnings, not errors, so a partial system still applies what
  it can.
- `devicemodel_test.go` — covers shared-input fan-out, exclusive-output
  rejection, role mismatch, preset resolution, capability gating.

## Studio backend (`app/go/studio/`)

- `app_devicemodel.go` — the Wails bindings: `RefreshDeviceModel`
  (PortList+RoleList → BuildModel), `DeviceModelSnapshot`, `ClaimPort`,
  `UnclaimPort`, `CandidatePorts`, `AttachRole`/`DetachRole` (wire +
  optimistic model update), `ApplyPreset`. The model is guarded by `dmMu`;
  wire I/O runs on a client pointer snapshotted under `a.mu` and released
  **before** the call (never hold a mutex across a transfer). Emits
  `devicemodel:changed`.
- `port_watcher.go` — `PortWatcher` extracted from `App`: enumerates serial
  ports, fires `OnChange`, and fires `OnVanished(port)` for a watched port
  (connection-agnostic; `App` registers the port it holds via
  `SetWatched`).
- Connect/identify now go through `client.Connect(port, opts)` — the single
  home of the open→IDENTIFY→peer-payload sequence. The platform→payload
  heuristic no longer lives in any UI layer.
- Async diagnostics route through `c.Events.OnAny(...)` (the one async
  owner) instead of monkeypatching the connection callback.

## Frontend (`studio/frontend/src/lib/`)

- `devicemodel.ts` — typed mirror of the DTOs + the reactive `deviceModel`
  store, binding wrappers, and the `studioTabs` derived store. No model
  logic; every mutation round-trips to Go and replaces the store from the
  returned snapshot.
- Tabs: `DomainTab.svelte` is the **generic** functional tab — renders any
  domain's slots from the catalog; new functional tabs pass a different
  `domain` prop, no bespoke code. `MainLayout`/`TabBar` render from
  `studioTabs`: `Input & Ports · <one per available domain> · Firmware`,
  each tab with a functional icon.
- The **Input & Ports** tab (`IoTab.svelte`) is two columns: left =
  `InputPanel.svelte` (RC protocol — PPM enabled, SBUS/Jeti disabled —
  channel count, per-channel function dropdown, and a live value bar
  *under* each channel that shows **NO SIGNAL** when no valid frame);
  right = `PortRoleTab.svelte` (port→role wiring + presets + ★ Defaults +
  ▣ Diagram). The **Diagram** (`PcbOverlayDialog.svelte`) overlays markers
  on the board photo at coordinates measured by `tools/analyze_hubfx.go`;
  it edits the same model. (`PortControls.svelte` is the shared role+name
  editor it embeds.)

> Svelte gotcha: template expressions can't parse TS `as` casts. Read
> `e.target` values through a script helper (`selValue(e)`/`numValue(e)`),
> never inline `(e.target as HTMLSelectElement)` in markup.

## Ports, roles, names & hardware labels

- **Hardware label** (`Port.HardwareName`) is the silkscreen-style id —
  `SRV1`, `CH3`, `IN1`, `HB2` — derived in Go (`devicemodel.HardwareLabel`)
  from kind + index, since it's deterministic from the port count the
  firmware already reports. Shown as the port's id. (A firmware-authored
  per-port label would only be needed for a board whose silkscreen
  deviates from the kind+index pattern — not the case for any current
  board, so it stays a Go derivation rather than a wire-breaking
  port-descriptor change.)
- **Allowed roles** (`Port.AllowedRoles`) are derived from kind
  (`devicemodel.AllowedRoleOptions`): servo→Servo; pwm→LED/DC Motor/Heater;
  hbridge→Bi-Dir/DC Motor; input→RC PWM/SBUS/Jeti. The role picker offers
  only these (human-readable via `RoleLabel`).
- **Servos** host only `servo-actuator`, so the UI shows a fixed "Servo"
  tag + a name field (no dropdown). The backend auto-attaches
  `servo-actuator` to unattached servo ports on refresh so they're
  immediately claimable.
- **Operator name** (`Port.Name`) is overlay state in the studio backend
  (`portNames`, survives topology refresh), set via `SetPortName`.

## Multi-board grouping, the Diagram, and OFFLINE (abandoned) boards

The model is a **flat port list**; the UI groups it by `Port.ref.guid`. Every
distinct GUID is its own board card (`PortRoleTab.groupByBoard`) and its own
**Diagram tab** (`PcbOverlayDialog` — hub tab first, then one per board). Two
expanders of the same kind but different GUID therefore get two cards + two
tabs; `boardDisplayNames` indexes them (`GearControl #1`, `#2`). Board kind is
derived from the device name by `devicemodel.BoardKindFromName` (Go) /
`boardKindOf` (TS) — these MUST stay in lock-step (the firmware prefix
`GearCtrl` ≠ the kind `gearcontrol`, so it's a prefix/alias map, not equality).

**Diagram overlay** (`pcb.ts` + `PcbOverlayDialog.svelte`): each board kind has
a `BoardPcb { image, markers, info }`. Marker `(x%, y%)` come from MEASUREMENT,
not eyeballing — `tools/analyze_<board>.go` colour-masks the connector
silkscreen/housings and prints centroids (HubFX: white+magenta; GearControl:
magenta H-bridge terminals + yellow servo-pin clusters). Re-run if a render is
regenerated. A board kind with no `BoardPcb` shows a "no image" state.

**Abandoned boards (configured-but-disconnected expanders).** The live model is
topology-only, so a board declared in `/hubfx.yaml`'s `expanders:` block but not
plugged in would vanish — AND the old `SaveHubConfig` rebuilt purely from live
ports, silently **dropping** its config on the next Apply (data loss). Now:

- `LoadHubConfig` RETAINS the parsed `expanders:` block in `a.hubExpanders`
  (keyed by GUID), with a `type:` field (board kind, stamped on Save; inferred
  from an H-bridge port when absent, for back-compat).
- `deviceModelSnapshot` appends **offline ghost ports** (`devicemodel.OfflinePort`,
  `Port.Offline=true`) for every retained GUID not in the live model, plus a
  `SevWarn` issue (non-blocking — doesn't gate Apply).
- `SaveHubConfig` **preserves** every abandoned entry verbatim (no more drops).
- The UI renders offline boards as dimmed cards / tabs with an OFFLINE badge +
  warning; their roles/names are read-only (no wire to push to). The explicit
  **Remove from config** action calls `RemoveExpanderConfig(guid)` → drops the
  retained entry + overlays + marks dirty → Apply rewrites `/hubfx.yaml` without
  it. Offline ports never enter the Inputs tab (appended after that loop).

## Design system — see Rule 34

All Studio UI composes the shared classes in `style.css` (`button`,
`.field-input`, `.card`, `.form-row`, `.banner`, CSS vars). Component
`<style>` does layout only — never re-skins controls. This is what keeps
control heights uniform.

## Adding a functional domain (the whole checklist)

1. Add a `DomainID` const + a `Domain{}` entry (slots, role kinds, cap) to
   `domainCatalog` in `devicemodel/types.go`.
2. Add a test if the slot rules are novel.
3. Done — the tab appears automatically (capability-gated), the picker is
   driven by `Candidates`, and validation is automatic. Build a bespoke
   `*.svelte` only when the domain needs live control beyond port
   assignment (it then reads the same model store).
