# 33 — Configuration Wizard + Config Assistant (design)

Status: **WIZARD IMPLEMENTED** (branch `wizzard-confgen`) · **ASSISTANT = DESIGN** (§7, not started).

Goal: a guided "Setup Wizard" that takes an operator from a blank/intimidating
config to a working one — pick features → set up the radio input → map channels
→ configure each effect with advice + sensible defaults. Plus a longer-term
**config assistant** (Claude-powered chat) that drives the same actions in
natural language.

The as-built component tree + the implementation patterns/gotchas are in **§10**;
the design rationale (§1–§7) is preserved as written.

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
3. **Per-effect steps** — ✅ **DONE** (replaces the old single `WizardStepEffects`,
   now deleted). Steps are **dynamic** (`wizardSteps` derived store): one dedicated
   step per *enabled* effect, inserted between the channel map and review. Each is a
   full setup surface modelling that effect's real workflow (§10), with **in-wizard
   auto-attach port assignment** — `WizardPortPicker` claims a free compatible port
   AND attaches its role on pick (`ensureRole`), so the operator never visits the IO
   tab. Defaults: enabling an effect seeds its starter via the existing `default*`
   factories (gear struts, a gun, a landing light, …).
4. **Review & Apply** — ✅ `WizardStepReview`: effect/channel summary +
   non-info advice + the global `applyAll()` (hub-first) with result surfacing.
5. **Polish** — partial; remaining: draft snapshot/rollback on Cancel, per-effect
   sub-feature gating in step 1 (today each effect step exposes its own
   sub-sections), "explain this option" tooltips.
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

## 10. As-built (the wizard, 2026-06-14)

**File layout** — everything lives in `app/go/studio/frontend/src/lib/wizard/`
except the stepper store + the modal shell:

| File | Role |
|---|---|
| `lib/wizard.ts` | stepper store: `wizardSteps` (derived — HEAD + one per enabled effect + REVIEW), `wizardStep`, `next/prev/goto`, `openWizard`/`closeWizard`/`installWizardAutoOffer`, `isConfigEmpty` |
| `lib/wizard-features.ts` | effect registry `WIZARD_FEATURES[]` (`isEnabled`/`setEnabled`/`getInput`/`setInput`/`domainIds`); `availableFeatures(dm)` gates by `deviceModel.domains` |
| `lib/wizard-advice.ts` | `analyzeWizard(dm) → Advice[]` (feature↔channel coverage, duplicates) + `adviceFor(all, where)` |
| `lib/wizard-ports.ts` | `assignablePool(ports, claims, kind, role, exempt)`, `ensureRole(p, role)` (attach-on-pick), `portToRef`/`emptyRef`/`refKey`/`portKey` |
| `lib/dialogs/ConfigWizard.svelte` | modal shell: left step rail + body + footer; routes by `current.id` (`features`/`input`/`channels`/`review`) then by `current.feature` (`engine`/`gear`/`gun`/`landing`/`lighting`) |
| `wizard/WizardStep{Features,Input,Channels,Review}.svelte` | the four fixed steps |
| `wizard/WizardPortPicker.svelte` | role-aware output-port dropdown; auto-attaches role on pick; empty-pool warning; 240px min-width |
| `wizard/WizardChannelSelect.svelte` | named-RC-channel dropdown (Rule 43) over `collectChannelOptions`, deduped by `fnId`; 240px min-width |
| `wizard/WizardEffect{Engine,Gun,Gear,Landing,Lighting}.svelte` | one per effect — the per-effect workflow below |

**Per-effect workflows** (distilled from each tab's real usage, not a UI clone):
- **Engine** — type · speakers (output mode) · RC on/off toggle (channel + threshold + hysteresis + failsafe) · the three sounds (running required; start/stop optional; `pickFile({targets:'sd'})`).
- **Gun** — per gun: trigger (channel + threshold + ROF rpm) · muzzle flash (pwm/`LedAnimator` + duration + brightness) · recoil (enable + jerk) · smoke (heater pwm/`Heater` + fan pwm/`DcMotor` + element mV) · turret yaw/pitch (servo/`ServoActuator` + channel).
- **Gear** — coordination (independent/full-sync) · RC up/down (channel + threshold + deploy-on-link-loss) · per strut: motor (hbridge/`BiDcMotor` + deploy direction + timeout) + doors 0–2 (servo/`ServoActuator` + opening mode + close policy).
- **Landing** — per light: servos[] (servo/`ServoActuator`) + LEDs[] (pwm/`LedAnimator` + brightness) + fade-in + activation (manual / RC channel).
- **Lighting** — master brightness · LED channels (pwm/`LedAnimator` + default brightness) · program selector (named channel + hysteresis) · seed a blank program (full timeline stays in the Lighting tab).

**Key patterns / gotchas:**
- **Reuses the drafts, never a new format.** Every effect component imports its draft
  store (`engineDraft`/`gunfxDraft`/`gearDraft`/`landingDraft`/`lightfxDraft`) + that
  store's mutators and writes them directly. The wizard is pure view + mutate; Apply
  is the existing `applyAll()`.
- **Auto-attach on pick** — `WizardPortPicker` offers `assignablePool` (compatible
  `roleKind`/`allowedRoles` AND unclaimed, current pick exempt) and calls
  `ensureRole(p, role)` (attaches the role to `/hubfx.yaml` if not already) before
  emitting the `PortRefT`. This is the increment that was deferred in the original §8.3.
- **Dynamic steps** — `wizardSteps` is `derived([...all five drafts], …)` so toggling
  a feature in step 1 inserts/removes its step live; `ConfigWizard` clamps the index
  when the list shrinks.
- **Svelte-3 markup gotcha (cost a build cycle):** **no TypeScript in markup
  expressions.** `svelte-preprocess` only transpiles the `<script>` block — template
  handlers are parsed as plain JS by the Svelte compiler. A typed inline arrow param
  `on...={(ref: PortRefT) => …}` throws `Unexpected token` (with a *phantom* line:col,
  because the `µ` multibyte chars in the markup desync the byte-vs-line counter). Pass
  untyped params in markup (`(ref) => …`); the component prop type drives inference.
  Same family: inline `as` casts and unwrapped `{#each $store.member as x}` (wrap the
  expression in parens) break the parser — keep all TS in `<script>` helpers.

## 11. Shipped alongside the wizard (same branch) — config-toolbar UX

Two global config-toolbar changes landed with the wizard:

- **Auto-apply** (`lib/auto-apply.ts`). Optional "persist on settle": an edit
  that leaves a draft dirty arms a VISIBLE 5 s countdown in the toolbar (pill +
  Hold); if neither Apply nor Hold is pressed it runs `applyAll()`. Per-edit
  debounce (each edit resets the timer), held by validation errors (Rule 35) and
  suppressed while the wizard is open, reuses `applyAll()` so only DIRTY sources
  persist (no needless hub role-cycle). Toggle persisted to localStorage, **on by
  default**. Manual Apply + auto-fire share one `applyInFlight` guard. Activity is
  detected by subscribing to the draft stores + `deviceModel` (structural only —
  live RC telemetry is a separate `liveChannels` store, so the debounce can't get
  stuck). Wired in `layout/ConfigToolbar.svelte`.
- **Layout (Option B).** The global config bar moved ABOVE the tab strip
  (`layout/MainLayout.svelte`): order rows by scope, broadest outermost —
  config (apply/dirty/wizard) ⊃ domain tabs ⊃ a tab's sub-tabs ⊃ content. The
  bar ALWAYS occupies its row and self-gates its content to HubFX inside (an
  expander shows a muted note at the same height) so the tab bar never jumps when
  the controller type changes.

## 12. As-built — the Config Assistant (branch `chatbot-assistant`)

The Claude-powered assistant (§7) shipped as an **advisory, multi-provider chat
dock**. It answers ScaleFX setup questions grounded in an embedded textbook +
the operator's live config; it does NOT apply config or actuate hardware (it has
no such tool) — it explains and points at the Wizard/tabs.

**Reusable AI layer** — `app/go/studio/genai/` (provider-agnostic, no ScaleFX
coupling): `Provider` interface (`Generate`/`Name`/`Model`); a native **Gemini**
client + a generic **OpenAI-compatible** client used for **Groq** + **Mistral**;
`ListGeminiModels`/`ListOpenAIModels` for live model lists; build-time
`Builtin{,Groq,Mistral}Key` vars (ldflags `-X studio/genai.BuiltinKey=…`).

**ScaleFX assistant** — `app/go/studio/assistant/`: an embedded markdown
**textbook** (`knowledge/*.md`, `go:embed`) written purely from the user/setup
perspective (overview, effects, Studio navigation, Console + commands, setup
workflow, FAQ, glossary, output-format) + a guardrail (`assistant.go`) that
scopes answers to ScaleFX, enforces the output format, and forbids
apply/actuate. `SystemPrompt(liveContext)` = guardrail + textbook + the live
context. The Snapshot is NOT built in Go — see below.

**Bindings + multi-provider** — `app_assistant.go`: settings in
`%APPDATA%/ScaleFX/settings.json` (active provider + per-provider key/model);
`AssistantStatus`/`AssistantAsk(history, liveContext)`/`SetAssistantProvider`/
`SetAssistantKey(provider,key)`/`SetAssistantModel`/`ListAssistantModels`. Key
precedence: settings key → build-time builtin → none. Providers registered in
one `aiProviders` slice.

**Frontend** — `lib/assistant/`: the dock (`AssistantPanel.svelte`, mirrors the
Console dock), a dependency-free **markdown renderer** (`markdown.ts` — safe
HTML-escape + fixed tag set; headings/lists/inline-code/fenced-code/links/
blockquotes/**tables**) with a colour scheme (teal items, plain-bold headers,
RED safety blockquotes), the **live-context builder** (`context.ts`), and the
store. Per-provider model **allow-lists** live in one `ALLOWLISTS` map in the
panel; the live list only flags availability. Settings (`ViewSettingsDialog`)
holds the provider dropdown + per-provider key.

**The "(None)" bug + the key architectural call:** the live context is built in
the FRONTEND (`context.ts`) from the device model + the **effect drafts** — NOT
in Go from the device-model *claims* (which are vestigial — that was the
"Configured effects = none" bug). So the assistant sees each effect's real
enabled state + unsaved edits, names ports by their friendly label resolving
hub-local vs expander via the canonical `guid|kind|index` key (`modelPortKey`/
`portRefToKey` — a bare `kind:idx` key collides across boards), and cites
channels as `CHn` + label. Deferred: write-tools (still advisory-only), streaming.

**FAQ tab + drift guard (branch `assistant-faq`).** The Assistant dock has a
**Chat | FAQ** toggle; the FAQ is the **non-LLM, offline, no-key** path — a
searchable accordion parsed from `knowledge/40-faq.md` (`assistant.FAQItems()` →
`AssistantFAQ()` binding → `FaqView.svelte`), answers rendered through the shared
markdown renderer. To stop the grounding rotting (the textbook's hand-maintained
half), **Rule 64** makes the textbook + FAQ + `context.ts` docs-as-code, and
`tests/host/go_unit/assistant_docs_test` (pre-merge gate) asserts the textbook
documents every effect channel-function label from
`devicemodel.ChannelFunctions()` — add a channel function + forget the glossary
and the gate fails. The glossary's channel-function list is now the canonical
reference (verbatim labels).
