# 33 — Configuration Wizard + Config Assistant (design)

Status: **DESIGN / PLAN** (branch `wizzard-confgen`). Not yet implemented.

Goal: a guided "Setup Wizard" that takes an operator from a blank/intimidating
config to a working one — pick features → set up the radio input → map channels
→ configure each effect with advice + sensible defaults. Plus a longer-term
**config assistant** (Claude-powered chat) that drives the same actions in
natural language.

---

## 1. Why this is cheap to build (key finding)

The hard part already exists. Every config surface in Studio is a **draft store +
DirtySource** (Rule 46):

- `hubDraft`/device-model mutators (`AttachRole`, `SetChannelFunction`,
  `SetInputProtocol`, …) → `/hubfx.yaml` (ports, roles, inputs, expanders, features)
- `engineDraft` `gunfxDraft` `lightfxDraft` `landingDraft` `gearDraft` → per-effect YAML
- All registered in `App.svelte` and applied **in dependency order** by
  `applyAll()` (hubconfig first, effects after) via the global `ConfigToolbar`.

**The wizard is a guided front-end that writes those same drafts and then calls
the existing apply.** No new config format, no new save/reload backend — it
reuses `SaveHubConfig` + `SaveXxxConfig`. The only genuinely new backend is the
(optional) assistant's LLM call.

References (from the analysis):
- Tabs/apply: `devicemodel.ts` (`studioTabs`, `hubConfigSource`), `dirty-registry.ts`,
  `layout/ConfigToolbar.svelte`, `App.svelte` (registration order).
- Draft triads: `gear.ts` `gunfx.ts` `effects.ts` `lightfx.ts` `landing.ts`.
- Live data: `deviceModel` (ports/roles/domains/inputs), `effectClaims`,
  `channels.ts:collectChannelOptions`, `port_pool.ts:freePortPool`, `liveChannels`.
- Channel pickers: `ChannelToggleCluster.svelte` (Rule 36), `ChannelBandCluster.svelte` (Rule 38).

## 2. The config dependency graph (the wizard's spine)

```
1. features:        which effects + sub-features are on        (/hubfx.yaml features:)
        │
2. input source:    protocol (PPM/SBUS/Jeti) + channel count   (/hubfx.yaml inputs ports)
        │
3. channel map:     channel N → named function (gear_updown,    (/hubfx.yaml inputs[] names)
        │           gun_trigger, engine_toggle, …)
        │           └─ FITNESS CHECK vs the features from step 1
        │
4. ports & roles:   assign output ports, attach the role each   (/hubfx.yaml ports[] + .profile)
        │           effect needs (BiDcMotor/ServoActuator/
        │           LedAnimator/Heater/DcMotor)
        │
5. per-effect:      bind the named input(s), set key params      (/<effect>.yaml)
        │           with sensible defaults + advice
        │
6. review & apply:  applyAll() in order (hub first)
```

Each effect references shared resources by **name/PortRef**, resolved at apply
(`findInputByName`, the alias table) — so the order above is the *authoring*
order, and `applyAll()` already enforces the *apply* order.

## 3. Architecture decision — modal overlay over the existing drafts

**Recommended: a full-screen modal wizard**, not a new tab.

- Launched from a **"Setup Wizard" button** (toolbar/menu) AND auto-offered on
  connect when the config is effectively empty (no roles/inputs).
- A stepper store `wizard.ts` (`currentStep`, `steps[]`, per-step `valid`/`warnings`,
  a working snapshot). Mirrors the existing modal pattern (`servo_calibration.ts`
  store + a dialog rendered in `App.svelte`; there's no stepper component yet —
  we add one).
- The wizard **mutates the real draft stores** as it goes (so the normal panels
  reflect every choice), and the final step calls `applyAll()`.
- "Cancel" restores the pre-wizard drafts (snapshot/rollback), so it's safe to
  explore.

Why modal over tab: it's a linear, focused flow that should *block* the dense
panels while running; it's reusable from anywhere; and it needs zero
domain-catalog/tab plumbing (Rule 59). (A thin "Wizard" entry could still live in
the tab bar that just opens the modal.)

## 4. Wizard steps (matches the requested flow)

Each step reads live `deviceModel`/`effectClaims`/`channels` and writes drafts.

**Step 1 — Features & sub-features.**
Card grid of effects gated by `deviceModel.domains`/capabilities: Engine, GunFX,
Lighting, Landing lights, Gear, Audio/Alerts. Toggle each on; expand to pick
sub-features (gun: muzzle / smoke / recoil / yaw-pitch; gear: doors? sync mode;
engine: type; lighting: programs / selector). Writes `features.*` + seeds each
effect draft's `enabled` + a minimal block (e.g. one strut, one gun).

**Step 2 — Input source.**
For each input port (hub IN_1, optional IN_2): pick protocol (PPM / SBUS / Jeti-EX,
filtered by `allowedRoles`) and channel count (with live "Autodetect" from
`liveChannels`). Writes the `/hubfx.yaml` input port config.

**Step 3 — Channel mapping + fitness check.**
A table: channel 0..N → named function (catalog functions like `gear_updown`,
`gun_trigger`, `engine_toggle`, `lightfx_selector`, or a custom name), with the
live µs bar per channel (reuse the InputPanel widgets). The **fitness engine**
(§5) runs here: it cross-checks the mapping against the step-1 features and shows
advice/warnings inline ("Gear is ON but no channel maps to *gear up/down*",
"You enabled GunFX ROF but mapped no `gun_rof` channel", "SBUS gives 16 channels;
you've named 3 — that's fine", "Two features both claim `aux1`").

**Steps 4…N — One step per enabled effect** (port/role + params).
For each effect: pick its output ports from `freePortPool` (filtered to the
required `RoleKind` + unclaimed), and **auto-attach that role** to the port in
`/hubfx.yaml` (the same `AttachRole` the PortRole tab uses) — so the operator
never thinks about roles. Bind the effect's named input(s) (pre-filled from
step 3). Set the few params that matter, each with a one-line "what this does"
help and a **sensible default** already applied (§6). Per-step advice: missing
port of the right role → "attach/clear", servo with no calibration → hint, etc.
Expanders: if a chosen port lives on a remote board, ensure an `expanders[]`
alias entry exists (auto-create alias).

**Step N+1 — Review & Apply.**
Summary of every change (features, inputs, role attachments, effect blocks) with
the fitness report; "Apply" runs `applyAll()` (hub first), surfaces any
per-source error, and on success closes. Offer "open the X panel to fine-tune".

## 5. The fitness / advice engine (deterministic, reusable)

A pure module `wizard-advice.ts` that, given `{features, inputs, channelMap,
ports, claims, effectDrafts}`, returns a list of `{level: info|warn|error,
where, message, fix?}`. Rules, e.g.:

- **Feature ↔ input coverage:** every enabled effect that needs a trigger has a
  mapped channel (gear→up/down, engine→toggle, gun→trigger[, rof], landing→deploy,
  lightfx→selector). Missing ⇒ warn with a one-click "map a free channel".
- **Channel sufficiency:** named channels ≤ channel count; enough channels for
  the selected features; duplicate/conflicting function names.
- **Port/role availability:** each effect's required role kind has a free port;
  voltage-rail sanity (Rule 37/42) for sub-elements; servo ports calibrated.
- **Capability sanity:** feature enabled but capability bit absent on the board.
- **Failsafe advice:** gear/engine failsafe direction set; deploy-on-link-loss.

This engine is shared by **both** the wizard (inline advice + gating) and the
assistant (as grounding + a `validate_config` tool). It largely overlaps the
existing per-effect `hasErrors` verifiers — factor those so the wizard reuses them.

## 6. Sensible defaults

Centralise in `wizard-defaults.ts` (extend the existing `defaultXxx()` factories):
- Enabling an effect seeds a realistic starter block (gear: 3 struts Main L/R +
  nose; gun: 1 gun with muzzle; engine: turbine + stereo both).
- Triggers default to threshold 1500 µs / hysteresis 50 (matches current code).
- Gear: LiveRatio stall guard 2.5×, deploy/retract ±20000 duty, 30 s timeout,
  RC ON = down (the RC1 convention).
- Servo profiles: default min/max/center; door open=MAX/close=MIN.
- Channel auto-assign: when a feature is enabled and a free channel exists, the
  wizard *proposes* a mapping (operator confirms).

## 7. Config Assistant (Claude-powered chat) — design + suggestions

Greenfield: the Studio Go backend makes **no outbound HTTP** and has no secret
handling today, so this is additive and isolated.

**Shape.** A dockable chat panel (modal or side-pane) that can *read* the live
config + *propose structured changes* the operator previews and applies — it
drives the **same wizard actions**, never edits YAML blindly.

**Backend.** New `app_assistant.go` Wails binding `AssistantChat(history,
contextJSON) → stream`. Use the Anthropic Messages API (Go: official
`anthropic-sdk-go`, or `net/http` to `api.anthropic.com/v1/messages`), streaming.
Default model **`claude-sonnet-4-6`** (fast/cheap for interactive config), with an
**`claude-opus-4-8`** "deep" toggle. (Per project guidance, default to the latest
Claude models.)

**Grounding (context the backend assembles each turn):**
1. A compact **schema/capability digest** (effects, sub-features, required roles,
   named-input conventions) — static, generated from this doc + the config headers.
2. The **live device model**: ports + roleKinds + free pool, capabilities, the
   current channel map, expander GUIDs.
3. The **current drafts** + the **fitness report** (§5).

**Tool use (keep the human in control).** Expose the wizard's mutations as tools:
`enable_feature`, `set_input_protocol`, `map_channel`, `assign_port` (auto-attaches
role), `set_effect_param`, `validate_config`, `apply`. The model calls tools →
the frontend renders a **preview/diff** of the resulting draft changes → operator
clicks Apply. This makes the assistant safe (no free-form YAML), reuses all the
deterministic logic, and gives great UX ("set up retractable gear on channel 5"
→ proposed diff).

**API key.** Operator-supplied, stored via a Studio setting (OS keychain or a
local settings file the user controls); passed to the Go backend per session;
never committed, never written to device config.

**Phasing.** Ship the deterministic wizard first; the assistant is a thin NL layer
over the same actions + fitness engine, added after.

## 8. Implementation phases / status

1. **Scaffolding** — ✅ `wizard.ts` stepper store + `ConfigWizard.svelte` modal +
   🪄 toolbar launch + auto-offer on empty config (`isConfigEmpty` = no named
   inputs). (Snapshot/rollback of drafts on Cancel still TODO — Cancel currently
   leaves drafts edited-but-unapplied, like the panels.)
2. **Steps 1–3** — ✅ Features (`WizardStepFeatures`, capability-gated toggle grid
   over the effect registry `wizard-features.ts`), Input source
   (`WizardStepInput`, protocol + channel count + autodetect), Channel map
   (`WizardStepChannels`, per-channel function + live bars). Fitness engine
   `wizard-advice.ts` (feature↔channel coverage, duplicates) drives the inline
   advice.
3. **Per-effect (Step 4)** — ✅ v1: `WizardStepEffects` binds each enabled
   effect's RC channel (the key Rule-43 mapping) + engine type/output, shows
   per-effect advice, and deep-links to the full panel. **Deferred:** in-wizard
   port/role assignment (pick + `attachRole`) — today that's done in the panels
   (`freePortPool` needs the role pre-attached; the auto-attach-on-pick flow is
   the next increment). Defaults: enabling an effect seeds its starter via the
   existing `default*` factories (gear struts, a gun, a landing light, …).
4. **Review & Apply (Step 5)** — ✅ `WizardStepReview`: effect/channel summary +
   non-info advice + the global `applyAll()` (hub-first) with result surfacing.
5. **Polish** — partial; remaining: draft snapshot/rollback on Cancel, sub-feature
   toggles per effect (gun muzzle/smoke/recoil), in-wizard port assignment,
   "explain this option" tooltips.
6. **Assistant (separate)** — ⏳ not started. `app_assistant.go` + chat panel +
   tool-driven previews, grounded on §5/§7.

## 9. Decisions (locked 2026-06-14)
- **Surface: modal overlay** (full-screen stepper launched from a "Setup Wizard"
  button + a thin tab-bar entry). Not a dedicated config tab.
- **Build order: wizard first, assistant later** (§7 is phase 6).
- **Auto-launch: yes** — offer the wizard (dismissible) on connect to a board
  with effectively no roles/inputs configured.
- **Assistant API key: bring-your-own** Anthropic key in a local Studio setting
  (when phase 6 lands).
