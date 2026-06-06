# 31 — GUID / Port-Routing: the hub-local-vs-remote ambiguity, and the pattern to fix it

Status: **design proposal** (2026-06-06). Addresses the recurring class of bug where a hub-local port is addressed under the *wrong* GUID form — most visibly the servo-calibration "profile opens with defaults / deploy stops short" hunt this session, but the same root cause has spawned `hubLocal()`, `lookupProfile()` dual-form fallback, the async `servo:motion` GUID remap, `normalizeSelfGuid()`, and the two-clause `isLocalTarget()`.

## The usage pattern (what we're modelling)

The HubFX master owns local ports (CH1–8 PWM, IN_1–12) AND hosts dynamically-detected USB expanders, each with a hardware GUID (`"6D60"` etc.). Every command/query addresses a port by `PortRef{guid, kind, idx}` and the hub routes it: **local → hub's own role service; remote → forward to the expander over CDC**. So the address must encode "which board", and the router must decide local-vs-remote from it.

## The defect: THREE representations of "the hub"

| # | Form | Where it's used |
|---|------|-----------------|
| 1 | `guid == ""` (empty / `guid[0]==0`) | firmware-internal `PortRef::local`, effect input-binding match, `/hubfx.yaml` ports[], wire **requests**, wire **async** events, Go role layer (`c.Roles.*`), Go config |
| 2 | `guid == "<hub's own 4-hex>"` (e.g. `"6D60"`) | wire **LIST responses** only ([topology_service.ipp `appendHubPortBlock`](../controllers/hubfx/esp32s3/src/topology/topology_service.ipp)), and therefore Go `BuildModel` (stamps it onto every hub port), `a.id.GUID`, the Studio `portProfiles` overlay keys |
| 3 | implicit "is local" bool | `PortRef::isLocal()`, `isLocalTarget()`, `hubLocal()` — derived, but computed differently in each spot |

Form 1 is the convention **almost everywhere**. Form 2 leaks in through exactly ONE place — the hub stamping its own GUID into LIST responses — and then metastasizes through `BuildModel` into the whole Studio device model, forcing every consumer to handle both. The async path even *re-derives* form 2 from form 1 ([app_servo.go](../app/go/studio/app_servo.go): `if ev.GUID == "" { ev.GUID = a.id.GUID }`) just to match `BuildModel`.

### Root cause (verified)
- **Responses are asymmetric with requests.** Requests/async use `""` for hub-local; LIST responses use the hub's real GUID. The hub *knows* it's describing its own ports — it has no reason to emit a GUID a client must then collapse back.
- **Go decode does not normalize.** [`DecodePortListResp`](../app/go/protocol/topology/topology.go) returns `BoardPorts{GUID: g}` verbatim; `BuildModel` keys ports by it. The hub-GUID is now a *port-addressing* value, when it should only ever have been *board-identity / display* metadata.

## The pattern: one canonical address, GUID is identity-not-address

**Rule:** `PortRef.guid == "" ⟺ hub-local`; a non-empty guid is **always** a remote expander GUID. The hub's own GUID is NEVER a valid `PortRef.guid` — it is board *identity* (shown in the UI, sourced from `INIT_READY` / `a.id.GUID` / `deviceName`), not a port address. Routing is then a single, trivial predicate everywhere:

```
route(ref): ref.guid == ""  → local role service
            else            → forward to expander[ref.guid]
```

This is the least-churn canonical form because Form 1 is already the majority convention. Choosing the hub-GUID instead would force changes to the firmware effect-binding match, config, async, and requests — far more surface.

**Enforce it at the boundary, so no app-layer code ever sees Form 2:**

### Step A (immediate, zero wire risk) — normalize in Go at decode→model
Collapse the hub's own GUID → `""` the moment it enters the Go device model, using the known hub identity. One funnel: `BuildModel` (and the async decoders) call a single `canonHubLocal(guid, hubGUID) string` that returns `""` when `guid == "" || guid == hubGUID`. After this:
- `BuildModel` stamps hub ports with `""` → the model matches `/hubfx.yaml`, the role layer, and config **by construction**.
- **Delete** `lookupProfile()`'s dual-form fallback (plain map lookup works), the `app_servo.go` async remap, and `hubLocal()`'s second clause (`hubLocal(g)` becomes `g == ""`). `SetPortProfile`'s `Roles.* vs Topology.*On` fork stays but now keys off the canonical `""`.
- Add a model invariant + guard test: **no `Port` in the device model may carry `a.id.GUID`** (it must be `""`). Fails loudly if Form 2 ever leaks back.

### Step B (the real fix) — make the wire symmetric
Have the firmware emit `glen=0` (`""`) for its OWN port/role blocks in LIST responses, exactly as it already does for async events and as requests expect. Keep `deviceName` ("HubFX-6D60") in the block for display — the UI sources the hub GUID from there / `INIT_READY`, never from a port ref. After this:
- The wire has ONE hub-local form end-to-end; Step A's Go normalization becomes a no-op safety net (keep it as a guard).
- **Simplify** firmware `isLocalTarget()` to just `guid[0]==0` (drop the deviceName-suffix compare) and retire `normalizeSelfGuid()` (config can't carry the hub GUID if nothing emits it; keep one assertion).
- The CLI (which today is saved by `isLocalTarget`'s second clause) gets the same clean model as Studio for free.

Step A unblocks the Studio cleanup with no firmware flash; Step B removes the latent trap so a *future* client (or the CLI) can't reintroduce it. Do A, ship, then B.

## Concrete deletion list (the workarounds this retires)
- [app_devicemodel.go](../app/go/studio/app_devicemodel.go): `lookupProfile()` dual-form fallback → plain lookup; `hubLocal()` second clause → `guid==""`.
- [app_servo.go](../app/go/studio/app_servo.go): async `ev.GUID == "" → a.id.GUID` remap → gone (model is already `""`).
- [devicemodel/model.go](../app/go/devicemodel/model.go) `BuildModel`: stamp `canonHubLocal(b.GUID, hubGUID)` instead of `b.GUID`.
- [topology_service.ipp](../controllers/hubfx/esp32s3/src/topology/topology_service.ipp) `appendHubPortBlock` / `appendHubRoleBlock`: emit `glen=0` for the hub's own block (Step B); `isLocalTarget` → `!guid || !guid[0]` (Step B).
- [port_ref_yaml.h](../controllers/hubfx/esp32s3/src/config/port_ref_yaml.h) `normalizeSelfGuid`: downgrade to a debug assertion once B lands.

## Guard (mirrors the error-collision test pattern)
A Go unit test builds a model from a synthetic PORT_LIST_RESP whose hub block carries the hub GUID, and asserts every resulting `Port.Ref.GUID` is `""` (Step A normalization holds). A second assertion: `route()` returns "local" for `""` and "forward" for any non-empty guid — pinning the single predicate.

## Why not the alternatives
- **Hub-GUID everywhere (Form 2 canonical):** larger blast radius (firmware effect bindings, config, async, requests all change) and makes the *router's* own ports look like just-another-board, losing the useful "local is special / zero-cost" distinction. Rejected.
- **Keep both forms + a shared helper:** that's the status quo (`hubLocal`) — it works but every new consumer must remember to call it, and the servo-calibration bug is the proof that they won't. A representation you must remember to normalize is a representation that will be used un-normalized.

## Relationship to the HubFX refactor (instructions/29)
This is orthogonal to the effect-boilerplate refactor but shares a principle: **collapse a duplicated decision into one canonical place.** Land 29's role-command senders and this routing canonicalization independently; both reduce the per-site reasoning an effect author must do to address a port.
