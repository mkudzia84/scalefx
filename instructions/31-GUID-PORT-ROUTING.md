# 31 — GUID / Port-Routing: the hub-local-vs-remote ambiguity, and the pattern to fix it

> **Implementation status (2026-06-06, firmware 2.22.0):** the canonical-`""` redesign is **LANDED**. Hub-local is now the empty GUID end-to-end: the firmware emits `""` for its own port/role LIST blocks (`appendHubPortBlock`/`appendHubRoleBlock`); Go `BuildModel` folds any stray hub-identity GUID → `""` (version-safe net for older firmware) with a guard test (`devicemodel_test.go::TestBuildModelFoldsHubGuid`); the Studio config keying (`loadHubConfig`/`SaveHubConfig`), `hubLocal()`, `lookupProfile()`, the `servo:motion` + `input:values` async remaps, and the input-arming filter all collapsed to the single empty form. The 6 workarounds enumerated below are **deleted**. **Still deferred (optional hardening):** replacing `guid string` with the typed `BoardRef` sum type + a single `TopologyService::send()` router — the landed canonical form + guard achieve the functional outcome (one form, no leaking); the type would additionally make `Expander("")` *unrepresentable*. Needs a hardware servo-calibration + RC round-trip to fully validate (integration tests, Studio closed).

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

---

# Recommended: an architecture-level redesign (type the address; separate identity from routing)

The fix below ("canonicalise `""`") removes the *symptom* but keeps the *shape* that caused it: the address is a **stringly-typed board id with an in-band magic sentinel**, and "is this local?" is re-derived by string comparison at every call site. A representation you must remember to normalise *will* be used un-normalised (the servo bug is the proof). The redesign makes the ambiguity **unrepresentable** and gives the master's specialness a real home.

Three moves, each fixing one architectural smell:

### 1. Address ports by a typed `BoardRef`, not a raw guid + sentinel
The local-vs-remote distinction is a *tag*, not a string value. Model it as a tiny sum type with two constructors — the master (`Self`) and a discovered expander (`Expander(guid)`):

```cpp
// firmware
struct BoardRef {
    enum class Kind : uint8_t { Self, Expander };
    Kind kind = Kind::Self;
    Guid guid{};                       // meaningful only when kind==Expander
    static BoardRef self()             { return {Kind::Self, {}}; }
    static BoardRef expander(Guid g)   { return {Kind::Expander, g}; }
    bool isLocal() const               { return kind == Kind::Self; }
};
struct PortRef { BoardRef board; uint8_t portKind; uint8_t portIdx; };
```
```go
// Go — comparable, so it works as a map key
type BoardRef struct { expander bool; guid string } // guid "" unless expander
func Local() BoardRef            { return BoardRef{} }
func Expander(g string) BoardRef { return BoardRef{true, g} }
func (b BoardRef) IsLocal() bool { return !b.expander }
type PortRef struct { Board BoardRef; Kind, Index byte }
```
`isLocal()` is now a field read, not a `strcmp` against a session-derived self-GUID. There is no empty-string-means-hub convention to forget — `Self` and `Expander("")` are different things (and the latter is invalid by construction).

### 2. One codec at the wire boundary — the ONLY place self/empty/hub-GUID are reconciled
The wire still needs bytes; the empty-vs-hub-GUID question collapses into a single encode/decode pair that everything funnels through:

```
encode(BoardRef): Self → [0];  Expander(g) → [len][g]
decode(bytes, selfGuid): [0] → Self;  [g]==selfGuid → Self;  else → Expander(g)
```
This subsumes BOTH staged steps below (the hub *may* keep emitting its GUID — `decode` folds it to `Self`; or emit `[0]` — either decodes identically). No app-layer code, Go or firmware, ever sees a hub-GUID-as-address again; `BuildModel`, async decoders, and config all call `decode` and get a `Self` board. `lookupProfile` dual-form, the `app_servo.go` remap, `hubLocal`, and `normalizeSelfGuid` **all delete** — they were re-implementing this codec at 8 sites.

### 3. The TopologyService is the sole router; callers never branch on local/remote
Resolving a `BoardRef` to a transport is the *router's* job, decided once, not at every handler:

```cpp
Result TopologyService::send(const PortRef& ref, uint8_t inner, const uint8_t* p, size_t n) {
    if (ref.board.isLocal()) return _roleSvc.handle(inner, p, n);      // local fast-path
    auto* slot = _expanders.resolve(ref.board.guid);                  // identity → live USB slot
    return slot ? slot->forward(inner, p, n) : Nack(TopologyError::UNKNOWN_GUID);
}
```
Effects and Studio say "send to this `PortRef`"; they never see `isLocalTarget`/`Roles.* vs Topology.*On` again. Dynamic discovery stays clean: you address by **stable identity** (the GUID, survives USB-port changes / reconnects), and the router maps identity → whichever live slot currently holds it — exactly the indirection a "master + hot-plugged nodes" topology wants.

### 4. Identity is metadata on the node, not the address
A board *node* carries identity separately from its address: `{ ref: BoardRef, identity: { guid, deviceName, version, caps } }`. The hub node is `ref = Self` with `identity.guid = "6D60"` — the UI shows `identity.guid`, persistence/reconnect key off it, but nothing *addresses* the hub by it. This is the clean split the current design lacks (GUID doing double duty as address + identity).

### Why this over "the hub is just another node, addressed by its GUID" (uniform, no sentinel)
That alternative (drop `Self`, give the hub a normal GUID everywhere, router fast-paths `guid==selfGuid`) is elegantly uniform but a worse fit here: the master is *genuinely* special — it is the router, always present, the topology root, and the zero-cost local path. A typed `Self` variant names that specialness explicitly instead of rediscovering it with a `guid==selfGuid` compare in the hot path, and it avoids rewriting the firmware's pervasive internal `PortRef::local` / effect-binding convention to carry a GUID. Uniformity that erases a real distinction isn't simpler — it just moves the special case into the router's comparison.

### Config-file & GUI impact — small, and net-positive

**Config files: NO format change, backward compatible.** `/hubfx.yaml` + the effect/expander YAMLs already encode hub-local as the *absence* of a board key (`port: { kind: pwm, idx: 5 }`) and remote as `guid: AB12` / `board: <alias>` ([port_ref_yaml.h](../controllers/hubfx/esp32s3/src/config/port_ref_yaml.h)). Only `portRefFromNode` changes — *no key* → `BoardRef::self()`, `guid:`/`board:` → `Expander`. The on-disk schema is untouched; existing files (even a stray `guid: <hub>` a past Studio save wrote into `/lightfx.yaml`) keep loading because `decode` folds the hub GUID → `Self`. `normalizeSelfGuid` retires into one assertion.

**GUI: minimal edits — keep BoardRef SERVER-SIDE; the Wails DTO stays a string `guid` that is `""` for the hub.** The frontend already uses `PortRefT.guid: string` with the convention `'' = Hub` ([devicemodel.ts](../app/go/studio/frontend/src/lib/devicemodel.ts) `guid === '' ? 'Hub'`, [effect-claims.ts](../app/go/studio/frontend/src/lib/effect-claims.ts) "guid === '' is legitimate"). Today the Go snapshot copies `a.dm.Ports` stamped `"6D60"` ([app_devicemodel.go](../app/go/studio/app_devicemodel.go) `deviceModelSnapshot`), so the frontend's `'' = Hub` branch is latently wrong for real hub ports. Serialising `BoardRef → ""` (Self) / `guid` (Expander) at the DTO boundary means the frontend's *existing* logic becomes correct with **no structural change** — `PortRefT` is unchanged, the `${guid}|${kind}|${idx}` keys, `ClaimPort/AttachRole/SetPortProfile(guid, …)` calls, and the live-channel / servo-status keys all keep working (and now consistently, retiring the `app_servo.go` async remap that existed only to align them to `"6D60"`). **Do NOT cross a structured `{local, guid}` object into TS** — that would churn every `.guid` access; the string serialization is the whole point. Net frontend change ≈ delete a stale special-case or two; possibly *fixes* the "Hub" board label that currently shows the raw GUID.

### Migration (incremental, ships in slices — no big-bang)
1. **Go first, behind the wire.** Introduce `BoardRef` + the `decode` codec; change `BuildModel`/async decoders to produce `Self`. Delete the four Go workarounds. Zero firmware change, zero wire change. (This is strictly better than — and replaces — "Step A" below.)
2. **Firmware address type.** Replace `PortRef.guid[5]` with `BoardRef`; route everything through `TopologyService::send`. `isLocalTarget` → `ref.board.isLocal()`; `normalizeSelfGuid` → the codec.
3. **Optional wire tidy.** Have the hub emit `[0]` for its own blocks (the codec already accepts both, so this is cosmetic once 1–2 land).
4. **Guard test** (mirrors the error-collision test): build a model from a PORT_LIST_RESP whose hub block carries the hub GUID; assert every `Port.Board.IsLocal()` and that no `Port.Board.guid == hubIdentityGUID`; assert `send`-routing returns local for `Self`, forward for `Expander`.

The "canonicalise `""`" steps below are the **fallback** if the type change is deferred — they keep the stringly-typed `PortRef` but at least funnel normalisation to one boundary. Prefer the typed redesign; it deletes the same workarounds *and* prevents the next consumer from reintroducing them.

---

## Fallback (no type change): one canonical string form, GUID is identity-not-address

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
