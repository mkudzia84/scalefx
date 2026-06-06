# 29 — HubFX Effects: Cleanup / Refactor / Sanitization Plan

Status: **proposal** (2026-06-06). Read-only analysis of `controllers/hubfx/esp32s3/src/effects/**` + the shared `controllers/lib/sfx_board/**` it builds on. No behaviour changes, no wire-format changes (Rule 11) — every item below is a pure de-duplication of repeated C++ shapes across the six effect services (Alerts, EngineFx, GearControl, GunFx, LandingLight, LightFx) + the InputDispatcher.

All line references verified 2026-06-06. Occurrence counts are exact (grep-counted), not estimates.

> Execution rule: land Tier 1 first (lowest risk, highest duplication), one shared helper per commit, `go build ./...` + `tools/run-tests.ps1 -Premerge` green between each. No back-compat shims (Rule 21) — convert every call site in the same commit that introduces the helper.

---

## Tier 1 — high duplication, low risk

### 1.1 Role-command senders (the 3-byte `[portIdx][u16LE]` pattern)
**Evidence:** 15 hand-rolled payload builders across
[gun_unit.ipp](../controllers/hubfx/esp32s3/src/effects/gunfx/gun_unit.ipp) (`commandServoTargetUs`, `commandServoPosNorm`, `commandHeater`, `commandFanPct`),
[landing_light.ipp](../controllers/hubfx/esp32s3/src/effects/landing_lights/landing_light.ipp) (`commandAllServos`, `commandLedsOn/Off`),
[light_controller.ipp](../controllers/hubfx/esp32s3/src/effects/lightfx/light_controller.ipp), and
[gunfx_service.ipp](../controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_service.ipp). Each rebuilds:
```cpp
uint8_t payload[3]; payload[0] = port.portIdx;
SfxWire::putU16LE(&payload[1], value);
_send(_sendCtx, port, RolePacket::X, payload, sizeof(payload));
```
**Refactor:** a header of free functions `effects::role::servoSetTarget/servoSetPosNorm/motorSetPct/heaterSetTarget/ledStart/ledStop/ledSetBrightness(send, ctx, port, value)` that own the marshalling + endianness in ONE place. Each effect's private `command*` helper becomes a one-line forward (or is deleted at the call site).
**Why first:** every manual `putU16LE(&buf[idx])` is an endianness/offset bug waiting to happen (exactly the class of bug behind several wire issues this codebase has hit). ~15 sites → 1 tested helper set.

### 1.2 Handler validation guard (`len < N` → NACK)
**Evidence:** 58 `sendNack(SerialError::MISSING_PARAMETER)` sites; each handler opens with the same `if (len < N) { _ctx->sendNack(...); return; }` and frequently a second `findById(p[0])` / `!occupied()` guard. See the servo/gear/gun/landing service `handle*` methods and [role_service.cpp](../controllers/lib/sfx_board/server/role_service.cpp).
**Refactor:** a tiny inline `bool requireLen(ctx, len, n, err=MISSING_PARAMETER)` returning false + NACK-ing on failure, so handlers read `if (!requireLen(_ctx, len, 3)) return;`. Pair with an `findOr Nack(reg, id, err)` for the id-lookup guard. (The macros `SFX_REQUIRE_LEN` / `SFX_VALIDATE` in `serial/core/core.h` already exist for the server-side framework — extend their use into the effect services instead of open-coding.)
**Risk:** very low — mechanical.

### 1.3 Named-channel resolution (Rule 43)
**Evidence:** 7 `findInputByName` call sites, each followed by the identical "warn + leave binding empty + compute `channelId-1`" block — in [apply_hubfx_config.h](../controllers/hubfx/esp32s3/src/config/apply_hubfx_config.h), [landing_activation.h](../controllers/hubfx/esp32s3/src/config/landing_activation.h), [enginefx_config.h](../controllers/hubfx/esp32s3/src/config/enginefx_config.h).
**Refactor:** `config::resolveNamedInput(hub, name, context, itemId) -> {PortRef port; uint8_t channel; bool ok;}` centralising the warn + `channelId-1` convention.

### 1.4 ServoProfile attach-payload marshalling
**Evidence:** the 13-byte servo-profile payload is hand-packed in [apply_hubfx_config.h](../controllers/hubfx/esp32s3/src/config/apply_hubfx_config.h) (`attachPortRolesForGuid`) and hand-unpacked in [role_service.cpp `attachServoActuator`](../controllers/lib/sfx_board/server/role_service.cpp) + `handleServoSetProfile`. Three sites must agree byte-for-byte on offsets 0/2/4/6/7/9/11.
**Refactor:** a single `ServoProfileWire` struct with `pack(buf)` / `unpack(buf,len)` so the layout lives in one place. (This class of drift caused the REV-on-attach bug fixed in 2.21.1 — the pack site set `inverted` but the unpack site didn't apply it.)

---

## Tier 2 — medium duplication, medium risk

### 2.1 Phase-transition + event-emit shape
**Evidence:** byte-identical `enterPhase` in [gear.ipp:192](../controllers/hubfx/esp32s3/src/effects/gearcontrol/gear.ipp) and [landing_light.ipp:108](../controllers/hubfx/esp32s3/src/effects/landing_lights/landing_light.ipp) (`_state = p; LOG; if (_phase) _phase(_phaseCtx, id, p);`), plus the matching `emitPhaseEvent` (`[id][phase]` TAG_ASYNC) in both services and EngineFx's `enterState`/`emitStateEvent`.
**Refactor:** a `PhaseEmitter` mixin/holder (`id`, `state`, `fn`, `ctx` + `transition(newPhase, nameFn)`) the item classes compose. The service-side `emitPhaseEvent` collapses to one templated helper keyed by packet type.
**Risk:** medium — touches 3 effects' state machines; land LandingLight first, verify phase broadcasts, then Gear/Engine.

### 2.2 Verbose-status / status packer
**Evidence:** GunFx `emitVerboseStatus` ([gunfx_service.ipp](../controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_service.ipp)) hand-assembles a fixed-layout buffer with `buf[off++]` / `putU16LE(&buf[off]); off+=2;`; EngineFx + others repeat the cursor pattern.
**Refactor:** a `StatusWriter<N>` cursor (`u8/u16le/i16le/u32le` + `data()/size()`) — removes manual offset bookkeeping (a recurring source of off-by-two payload bugs).

### 2.3 Trampoline callbacks
**Evidence:** each of [gunfx_service.h](../controllers/hubfx/esp32s3/src/effects/gunfx/gunfx_service.h), [gearcontrol_service.h](../controllers/hubfx/esp32s3/src/effects/gearcontrol/gearcontrol_service.h), [landing_light_service.h](../controllers/hubfx/esp32s3/src/effects/landing_lights/landing_light_service.h), [input_dispatcher.h](../controllers/hubfx/esp32s3/src/effects/input/input_dispatcher.h) defines 3-4 `static …Trampoline(void* ctx, …)` that `static_cast<T*>(ctx)->method(...)`.
**Refactor:** a `TrampolineFor<&T::method>` template OR (cleaner, C++20) hand the item a capturing lambda once. Lower priority — ergonomic, not correctness.

---

## Tier 3 — deferred / low impact

- **Service lifecycle base** (`configure() → reapply() → claimPorts()` repeated in every service.h/.ipp). A `ConfigurableEffectService<TSpec,TItem,MaxItems>` base would absorb it, but the inner loops genuinely differ per effect — defer until the pattern stops drifting, or it becomes over-generalised.
- **Magic constants** — `kRcMinUs/kRcMaxUs` (also defined on the role as `kRcMinUs`/`kRcMaxUs` and in `gun_unit.ipp`), `kVerboseStatusIntervalMs`, fan/shot rate limits — consolidate into `effects/effect_constants.h`. Note `RolePacket::kPosNormFull` already centralises the normalised-position scale; follow that example.

---

## Already well-factored — do NOT churn

- The role protocol surface ([roles.h](../controllers/lib/sfx_serial/serial/roles.h)) and the `SERVO_SET_POS_NORM` normalised-position intent verb (Rule 42) — clean separation, role owns calibration.
- TopologyService `sendRoleCommand` / `claim` / `beginBatch`/`commitBatch` — well abstracted; the senders in 1.1 should call THROUGH it, not around it.
- InputDispatcher publish/subscribe (named RC channels, Rule 43) — clean.
- Per-item state machines (`GunUnit`, `Gear`, `LandingLight`) are appropriately item-specific; only their *phase-emit plumbing* (2.1) is shared, not their logic.

---

## Rule-compliance sweep to run alongside the refactor

While touching each effect, verify (and fix in the same commit) against the standing bans:
- **Rule 40** — no raw `millis()`/`micros()` in `effects/**`; must read `EffectClock::instance()`. Grep target: `controllers/hubfx/esp32s3/src/effects/**`.
- **Rule 15** — no `volatile` for cross-core; `std::atomic<T>` with explicit memory order.
- **Rule 55** — no Arduino API (`pinMode`/`digitalWrite`/`Wire`/`Serial*`/`Servo`/`millis`) in `controllers/lib/**`.
- **Rule 21** — delete dead fields / removed-flag fallbacks outright; no `// kept for back-compat` stubs.

## Suggested commit sequence
1. `role-command senders` header + convert 15 sites (1.1)
2. `requireLen` guard + convert handlers (1.2)
3. `resolveNamedInput` + convert 7 sites (1.3)
4. `ServoProfileWire` pack/unpack (1.4)
5. `PhaseEmitter` — LandingLight, then Gear, then Engine (2.1)
6. `StatusWriter` (2.2)
7. (optional) trampoline/lifecycle/constants (Tier 3)

Each step: `cd app/go && go build ./...` (protocol mirror unaffected, but the build is the cheapest regression net) + `scalefx-flash build hubfx --no-clean` + the relevant `tests/host/go_integration` suite against hardware.
