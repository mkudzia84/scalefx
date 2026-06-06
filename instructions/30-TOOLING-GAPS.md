# 30 — Tooling Gaps & Additions

Analysis (2026-06-06) of developer-tooling gaps surfaced by the landing/servo-calibration debugging arc and the general HubFX workflow, plus what was built to close them and what's deferred.

## Built

### 1. `servo-profiles` — dump every hub servo's LIVE calibration in one shot
**Gap it closes:** the 2.21.2 calibration bug took many round-trips because the only readout was `servo-profile-get <portIdx>` — one servo at a time, by index you had to already know. There was no "show me every servo's current min/max/center/rev" view, so a profile whose max was silently clamped narrower than the operator set was invisible at a glance.
**What:** [`servo-profiles`](../app/go/console/cmd_roles.go) enumerates every attached `servo-actuator` role on the hub (`Topology.RoleList("")`) and prints one colourized row each — `servo[2]  1373…1620 µs  center 1500  speed 1000  accel 100  REV`. A stale/clamped profile jumps out immediately.

### 2. Error-code collision guard — automated, in the pre-merge gate
**Gap it closes:** CLAUDE.md's error-range table is enforced only by discipline; the lesson "error codes collide silently in Go's `errorNames` map (last `init()` wins → wrong name shown)" had no automated check. The hazard had actually fired — `TopologyError` 0x90–0x96 squatted in the `AlertError` range (0x90–0x9F), so a topology NACK displayed as `ALERT_DISABLED`.
**What:**
- [`protocol.RegisterErrorNames`](../app/go/protocol/types.go) now records every *distinct* name per code; `CheckErrorNameCollisions()` reports any code registered under two names.
- [`tests/host/go_unit/error_collisions_test`](../tests/host/go_unit/error_collisions_test) imports every protocol sub-package and asserts zero collisions — runs in `tools/run-tests.ps1` (and the `-Premerge` gate).
- **Found + fixed the real collision:** `TopologyError` moved to the bottom of the Reserved error block (**0xC0–0xC6**), firmware + Go + CLAUDE.md table updated in lock-step.

## Deferred / proposed (not built)

### A. Packet dispatch-map (`ownsType`) collision validator
CLAUDE.md warns the hub dispatch map DRIFTS and silently collides (gunfx grew into 0xE2–0xE5 and swallowed a later AUDIO_PRELOAD). A validator would parse every policy's `ownsType()` predicate and flag overlapping byte claims.
**Why deferred:** the authoritative source is C++ `ownsType` predicates (ranges + special-cases), which are hard to parse statically. A *partial* win is tractable now: extend the Go side — assert no two `protocol/*` packages register the same `PacketType` *display name* under conflicting names (mirror the error-collision test using the existing `RegisterPacketNames`). That catches Go-mirror drift but not a firmware-only `ownsType` overlap. Recommended next step.

### B. On-device config round-trip / diff
A `config-diff` that downloads `/hubfx.yaml` (+ effect YAMLs) and diffs against the Studio device-model would catch "Apply didn't persist what I expected" (the class of confusion behind the calibration hunt — it turned out reload doesn't re-attach, so the *device* state and the *role* state diverge). Tractable as a CLI command over existing file download + the YAML parsers.

### C. Filtered / tagged `subscribe`
The live `subscribe` stream is colourized but unfiltered. A `subscribe <tag…>` (e.g. `subscribe servo ll`) that only prints matching `[…]`/`[LOG]` lines would make instrument-watching during a repro far less noisy. Small change to [cmd_events.go](../app/go/console/cmd_events.go).

### D. `coredump` — already exists
`scalefx-flash coredump hubfx` (instructions/24) already covers crash decode; no gap.

## Claude skills (`.claude/skills/`, tracked)
Captured the frequent dev loops as invocable skills: `scalefx-flash` (build/flash/coredump/version-bump), `scalefx-studio` (Wails build/run + reactivity-trap + design-system pointers), `scalefx-test` (unit/integration/premerge), `scalefx-cli` (board commands + `subscribe` instrumentation + wire-hazard notes). `.gitignore` now tracks `.claude/skills/` while keeping local `.claude/` state ignored.
