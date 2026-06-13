# 33 — Jeti EX Telemetry (bench findings + collection architecture)

> **Status:** reference + workflow · **Read when:** working on Jeti EX telemetry —
> the two-way responder, the telemetry collection, input connection-loss, or the
> physical layer (pull-up / baud).
> **TL;DR:** 4.7 kΩ pull-up on the IN_1/IN_2 signal is bench-proven best; 125 k
> baud stays; poll slaves ~10 Hz, publish per-value ~5–10 Hz (rate-limited,
> round-robin over the collection). The telemetry collection is built on
> `JetiTelemetryHub`.

Companion memory: `project_jeti_expander`, `reference_jeti_ex_telemetry_encoding`,
`reference_jeti_sbus_wire`. Firmware lives in
[controllers/lib/sfx_peripherals/jeti_ex/](../controllers/lib/sfx_peripherals/jeti_ex/).

---

## 1. Physical layer — pull-up (bench-verified 2026-06-13)

The IN_1 (`jeti-ex-input`) / IN_2 (`jeti-ex-telemetry`) signal idles HIGH and needs
a pull-up to 3.3 V. **One 4.7 kΩ per input** off the board 3.3 V rail (a shared
rail is fine — each input gets its OWN resistor; never share one resistor across
two inputs, that cross-couples them). The S3 GPIOs are **not** 5 V tolerant.

Bench A/B on HubFx-6DA4, IN_1, 125 k baud, measured via the firmware
`[jexp] IN_1 rxB/rxF/rxErr/valid` periodic log:

| Pull-up | Frames | rxErr | Behaviour |
|---|---|---|---|
| **4.7 kΩ** | 92,880 (4-min soak) | **4 (~0.004 %)** | `valid=1` throughout, zero NOISY/freeze. **Best.** |
| 10 kΩ | ~10 k | 59 → 744, incl. a burst | Marginal — intermittent NOISY dropouts. This was the "signal-loss" cause. |
| none | frozen | 568 frozen | Hard fail — decoder stalls, total signal loss. |

**Why:** at 125 k the bit time is 8 µs; the passive rising edge is τ = R·C. 10 kΩ
≈ 2× slower rise + lower noise immunity → marginal sampling → sporadic framing
errors. 4.7 kΩ pulls up fast enough to stay clean.

**Rules of thumb:** stay on **4.7 kΩ**. If a longer/noisier harness ever shows a
rare NOISY burst, go **down to 2.2 kΩ** (more margin), never up. Diagnose a
"completely lost input" by the signature: bytes still arriving (`rxB` climbing)
but `valid=0` + `rxErr` climbing + frame counter frozen = **physical layer**
(disturbed/marginal signal), not a decode bug — the decoder is provably fine if
it recovers clean on reconnect.

## 2. Baud — stay at 125 k

EX Bus supports 125 k and 250 k. **Do not move to 250 k** for this use:
- It halves the bit time (8 → 4 µs) → halves the edge-rate budget we just won
  with 4.7 kΩ (would likely need 2.2 kΩ + full re-validation).
- The benefits don't apply: the radio caps telemetry consumption at ~10 Hz/value
  (more bus bandwidth is unread); the reply slot is already ~80 % idle (~2 ms
  reply in a ~12 ms slot); the reply-stretch bug was a *scheduling* fix (IN_1 on a
  dedicated Core-0 task at prio 6), not a baud problem.
- Baud is fixed per **receiver** config — not a hub-side-only change.

The robustness lever is the pull-up, not the baud.

## 3. Poll / publish rates (EX protocol)

Governing fact: **the receiver is the bus master**; the hub replies only in the
window the RX offers. The real ceiling is the radio's consumption (~10 Hz/value),
not the wire.

Measured on HubFx-6DA4: RX offers a telemetry slot every ~12 ms (~81 Hz); the
firmware answers ~1-in-4 (~18.6 Hz). Targets:

| | Rate | Why |
|---|---|---|
| Poll slave ESC (IN_2) | **~10 Hz** (100 ms) | matches ESC internal refresh + radio log rate; faster widens the IN_2↔IN_1 crosstalk window |
| Emit EX frame (IN_1) | **~25–30 Hz** | one frame per answered window; answer a *subset* of slots so the half-duplex reply never stretches into the next channel frame |
| Per-value refresh | **~5–10 Hz** | after round-robin value-cycling; = what the radio actually shows. **This is what the rate limiter caps.** |
| Floor | ~3 Hz/value | slower feels laggy / risks RX "sensor absent" timeout |

Poll the ESC on a **decoupled timer** feeding a value cache; the publish step
reads the cache (never block publish on a synchronous poll). Key publish logic
off the *incoming request event* (in-window), not a hardcoded period — other RXs
and 250 k mode poll differently.

---

## 4. Telemetry collection (this branch — `jetiex-tuning`)

The canonical key/value telemetry store is **`JetiTelemetryHub`**
([jeti_telemetry_hub.h](../controllers/lib/sfx_peripherals/jeti_ex/jeti_telemetry_hub.h)):
device (USN/LSN/name) → up to 16 sensors (id/type/decimals/value/label/unit),
`local` devices never expire, downstream (ESC) devices expire on staleness. Both
hub-local metrics (`setLocalSensor`/`setLocalValue`) and actively-polled input
telemetry (the ESC monitor) already land here.

### Landed on this branch (2026-06-13)

1. **Collection** = `JetiTelemetryHub` (already a key/value store: device→sensors,
   `local` + polled-ESC). Kept minimal local metrics (Uptime + Version) — a
   placeholder that fills out as the hubfx board gains real sensors.
2. **Round-robin publish** — already existed (`JetiExpander::buildData` cycles
   `_dataDev`/`_dataSen` over the hub, one value per answered slot).
3. **Rate limiter** — `JetiExpander::replyIntervalMs()` scales the reply interval
   by active-sensor count N so each metric refreshes at ~10 Hz:
   `interval = clamp(1000/(N·10), 25 ms [40 Hz emit floor — channel-decode guard],
   200 ms [5 Hz keep-warm])`. Bench-verified: 2 hub sensors → 50 ms = 20 Hz.
4. **Read protocol** — `TelemetryPacket` `0xEB/0xEC/0xED` in the InputDispatcher;
   `buildTelemetrySnapshot` serializes the hub. Go mirror
   (`protocol/input.DecodeTelemetry`) → `client.Input.GetTelemetry` → CLI
   `telemetry` → Studio `GetTelemetry` → the IO tab `TelemetryPanel` (full-width,
   ~1 Hz poll). Publish-rate stat (target Hz) rides the snapshot header.
5. **Generic connection-loss** — the InputDispatcher timestamps every input
   source on each frame (before the routing gate; protocol-agnostic). `update()`
   state machine: silence > 300 ms → LOST (hold outputs, count brownout); silence
   > `linkLossMs` (configurable global, default 1000, `0` disables) → DOWN. Emits
   `CONNECTION_EVENT` (0xAD async) + an in-firmware callback. Wire: `0xAE/0xAF`
   GET/RESP, `0xA8` SET_CFG. CLI `links [ms]`. (Jeti UART reset/reconnect on LOST
   is the one remaining sub-item — the detection + signal + brownout record are
   in; the actual `usb`/UART re-begin is a HW-in-the-loop follow-up.)
6. **GearControl opt-in** — `deploy_on_connection_loss` (/gearcontrol.yaml +
   Studio "On signal loss" toggle). Gear `begin()` resolves the dispatcher via
   `findPolicy<InputDispatcherServicePolicyT<TTopology>>()` and registers an
   `onConnectionLoss` callback; on DOWN (latched, one per loss) it `commandAll
   (deploy)`. Distinct from the per-channel RC-loss failsafe (that's the gear
   up/down channel going invalid; this is the whole input LINK dying).

Bench (build 859, HubFx-6DA4): `telemetry` shows HubFx Uptime/Version @ 20 Hz;
`links` shows `hub port=0 up brownouts=0`. The loss→deploy path is wired; confirm
it on the bench by unplugging the Jeti with the gear enabled + the toggle on
(watch `[link] CONNECTION DOWN` → `[gear-svc] … EMERGENCY DEPLOY`).

---

## 5. Architecture: decoupling the IN_2 ESC monitor (DESIGN — not built)

> **Status:** design only (2026-06-13).  Today the IN_2 ESC telemetry monitor is
> wired ONLY when IN_1 is `jeti-ex-input`: `JetiExpander::begin()` (started from
> the IN_1 attach) owns both links, and `JetiExTelemetryRole` is a pure marker
> (`tick()` is empty).  So with **SBUS/PPM on IN_1 the ESC reads nothing**.  This
> section is the plan to support a STANDALONE IN_2 monitor — collection-only (no
> radio path; SBUS/PPM are one-way), expressed in the policy/concept/template
> grammar (Rule 6/18).  **Deferred by decision; do NOT implement yet.**

### Why it's coupled today (the three blockers)

1. **Destination** — ESC telemetry → radio is Jeti-only (the responder replies to
   the radio's polls).  SBUS/PPM carry no telemetry back, so a standalone monitor
   feeds ONLY the `JetiTelemetryHub` (Studio collection / effects / Rule-5
   connection-loss) — never the pilot's radio.  *Non-goal:* radio relay on non-Jeti.
2. **Poll template** — we poll the ESC by replaying a REAL Jeti telemetry-request
   captured from IN_1 (`captureEscPoll`).  No Jeti on IN_1 ⇒ nothing to capture ⇒
   must SYNTHESIZE the poll.
3. **Crosstalk** — IN_2's poll TX couples onto IN_1 RX (adjacent GPIO).  Paired,
   we window it into IN_1's guaranteed-quiet Jeti reply slot; standalone there's
   no signalled slot, so the poll must land in IN_1's inter-frame GAP.

### Target shape — two compile-time policies behind one monitor

Extract the ESC guts out of `JetiExpander` into a reusable, board-unique
`EscTelemetryMonitorT<TPoll, TWindow>` parameterised on two concepts; the
JetiExpander and a new standalone ServicePolicy each instantiate it with their
own policy models.  Shared sink stays `JetiTelemetryHub` (singleton, unchanged).

```cpp
// What frame to send the ESC to elicit a reply.
template <typename T> concept EscPollSource = requires(T& s, uint8_t* out, size_t cap) {
    { s.nextPoll(out, cap) } -> std::convertible_to<size_t>;   // writes a Jeti telemetry-request; 0 = none yet
};
// When it is crosstalk-safe to TX on IN_2 (vs IN_1 RX).
template <typename T> concept TxWindow = requires(T& w, uint32_t nowUs) {
    { w.canTxNow(nowUs) } -> std::convertible_to<bool>;
};

template <EscPollSource TPoll, TxWindow TWindow>
class EscTelemetryMonitorT {           // board-unique; owns InputPort* + decoder + presence machine
    void tick(uint32_t now) {
        _decoder.update(now);                       // bounded drain → JetiTelemetryHub (reuse JetiExTelemetryMonitor)
        updatePresence(now);                        // probe 200ms ⇄ active 75ms (reuse current FSM)
        if (now - _lastPollMs < _pollIntervalMs) return;
        if (!_window.canTxNow(SFX_MICROS())) return;        // crosstalk guard
        uint8_t f[16]; const size_t n = _poll.nextPoll(f, sizeof f);
        if (n) { _escPort->txEnable(); _stream->write(f,n); _stream->flush(); _escPort->txDisable(); _lastPollMs = now; }
    }
};
```

**Policy models:**

| Concept | Paired (IN_1 = Jeti) | Standalone (IN_1 = SBUS/PPM/none) |
|---|---|---|
| `EscPollSource` | `CapturedRxPoll` — the `captureEscPoll` template (today's path) | `SynthesizedJetiPoll` — builds a minimal valid telemetry-request with a rotating packetId |
| `TxWindow` | `JetiReplySlotWindow` — true only in the post-reply quiet slot (today's `maybePollEsc`-from-`serveTelemetry`) | `InputGapWindowPolicy<TDispatcher>` — true when IN_1's last-frame age sits in the safe gap; reads the InputDispatcher's per-source `lastMs` (the Rule-5 `touchLink` timestamps — already there). `FreeWindow` when IN_1 has no input role. |

So: paired = `EscTelemetryMonitorT<CapturedRxPoll, JetiReplySlotWindow>` (owned by
`JetiExpander`, ticked from `serveTelemetry`); standalone =
`EscTelemetryMonitorT<SynthesizedJetiPoll, InputGapWindowPolicy<TDispatcher>>`.

### Where standalone plugs in — a ServicePolicy, runtime-gated

```cpp
template <hubfx::effects::input::InputDispatcher TDispatcher>
class EscTelemetryServicePolicyT {                   // composed into BoardOf<…> after the dispatcher
    static constexpr uint32_t kCapabilityBits = 0;   // internal plumbing; reads ride the existing TELEMETRY_GET_REQ
    bool begin(BoardServerBase* ctx);                // resolve TDispatcher (for InputGapWindowPolicy)
    bool ownsType(uint8_t) const { return false; }   // no wire surface of its own
    void update() {
        if (JetiEx::JetiExpander::instance().drivesEsc()) return;   // paired wins — never double-drive IN_2
        if (!_standaloneActive) return;                              // only a lone jeti-ex-telemetry role, no jeti-ex-input
        _monitor.tick(SFX_MILLIS());
    }
};
```

- **Ownership / no-double-drive:** ONE driver of IN_2 at a time.  The standalone
  policy yields whenever the responder owns it (`JetiExpander::drivesEsc()` — a
  new accessor reflecting `_escPort != nullptr && respondingOnIn1`).  `_standaloneActive`
  is set at attach (`attachTelemetry`) when a `jeti-ex-telemetry` role is bound
  AND no `jeti-ex-input` is present; cleared if a `jeti-ex-input` later attaches
  (the expander takes over).  Concept `requires` gates the whole policy out on
  non-ESP32 / boards with <2 input ports (Rule 31).
- **Concept reuse:** `InputDispatcher` concept already exists (input_dispatcher.h)
  — `InputGapWindowPolicy<TDispatcher>` just needs a `lastFrameMs(PortRef)` reader
  on it (one accessor over the Rule-5 `_links` table).

### Migration (keep the proven path intact)

1. **Refactor-only:** lift the ESC bits from `JetiExpander` into
   `EscTelemetryMonitorT<CapturedRxPoll, JetiReplySlotWindow>`; the expander holds
   that instantiation and ticks it from `serveTelemetry`.  Behaviour-identical —
   bench-diff `[jexp] IN_2 ESC` stats before/after.
2. **Add** `SynthesizedJetiPoll` + `InputGapWindowPolicy` + `EscTelemetryServicePolicyT`,
   compose it into the hub's `BoardOf<…>` pack, wire `_standaloneActive` in
   `attachTelemetry`.
3. **Bench** the standalone path with SBUS on IN_1 + the ESC on IN_2: validate the
   synthesized poll elicits replies and the gap-window TX doesn't raise IN_1
   `rxErr` (SBUS frame-loss).

### Open questions

- **PPM windowing** is twitchier than SBUS (no CRC, pulse train) — ship SBUS-gap
  first; PPM may need a wider guard or a hard "no standalone ESC on PPM" gate.
- **Poll cadence vs SBUS frame period** (~14 ms) — one ESC poll per SBUS gap caps
  the ESC refresh at ~50–70 Hz; fine (ESCs update ~10 Hz).
- **Crosstalk reality** must be measured on the bench (the windowing math is a
  model); keep the `[jexp] IN_2` + IN_1 `rxErr` instrumentation as the gate.
