# Expander Board Design Guide

> **Audience:** firmware engineers building a new expander board, or
> migrating one of the legacy boards (GunFX/LightFX/GearControl) to
> the post-pivot generic expander architecture.
>
> **Companion docs:**
> [`15-GENERIC-EXPANDER-REFACTOR.md`](15-GENERIC-EXPANDER-REFACTOR.md) (the
> architectural pivot itself) and
> [`01-ARCHITECTURE.md`](01-ARCHITECTURE.md) (the wider system).
>
> **Status: 2026-05-06.** Reflects the post-pivot architecture
> (`ExpanderServer<TServos, TPwms, TLeds>` + `sfx_peripherals/collections/`
> + `sfx_expander/`).  Older "domain-specific board" docs (GunFX
> protocol, LightFX protocol, GearControl protocol) are retired.

## 0 · TL;DR — what An expander board IS

An expander board is a **dumb peripheral mux**.  It exposes a fixed
runtime: 0–N servos, 0–M mode-mutable PWM channels, 0–K LED channels,
plus an optional onboard battery sensor.  It speaks **one protocol**
(`ExpanderPacket`, packet IDs `0x01..0x7F`) plus the **core lifecycle
protocol** (`CorePacket`, `0xEE..0xFF`).  It owns no high-level
behaviour — recoil sequences, gear deploy choreography, landing-light
groups all live on the HubFX master.

An expander firmware sketch is ~50 lines.  Anything more is either
hardware bring-up code or wiring of the indicator LEDs.

```
   ┌───────────────────────── Master (HubFX) ─────────────────────────┐
   │  Effect orchestrators:  GunFX / LightFX / GearControl effects     │
   │  Routes async events → effects, RC inputs → ExpanderApi commands     │
   └───────────────────────────────┬──────────────────────────────────┘
                                   │  USB CDC, COBS, 6 Mbps
                                   ▼
   ┌───────────────────── Expander board firmware ───────────────────────┐
   │   SfxServer    (core protocol — INIT, KEEPALIVE, REBOOT, …)      │
   │   ExpanderServer  (component-collection protocol — 0x01..0x7F)      │
   │     ├── ServoCollection<N>      ← N hobby servos                  │
   │     ├── PwmCollection<M, …>     ← M PWM channels (mode-mutable)   │
   │     ├── LedCollection<K, TGpio> ← K LED channels (event runtime)  │
   │     └── TBattery (optional)     ← AdcDividerBatteryT<…> /         │
   │                                    Ina226Battery / NoBattery      │
   └──────────────────────────────────────────────────────────────────┘
```

This guide is the design contract for the **expander board firmware** box.

---

## 1 · Architectural rules

These are non-negotiable.  Most map back to the
[`.github/copilot-instructions.md`](../.github/copilot-instructions.md)
rule set; they're restated here because they constrain the expander
firmware's shape.

1. **No autonomous behaviour.**  An expander never runs a "standalone"
   mode.  It boots into `BoardState::IDLE` and stays there until it
   receives an `INIT(EXPANDER)` or `INIT(DIRECT)` from the master / CLI.
   No autonomous PWM, no autonomous servo motion, no LED programs
   running on power-up.  This is the architectural pivot of
   2026-05-06 — boards used to run config-driven autonomous loops;
   that's gone.
2. **All wire packets defined in `serial/expander/expander.h` or
   `serial/core/core.h`.**  No board-specific packet headers.  A
   "GunFX board" and a "LightFX board" differ only in their compile-
   time component fingerprint (`ComponentList` enumeration), not in
   their protocol surface.
3. **Timing-sensitive state machines stay expander-side.**  The servo
   trapezoidal profile, the LED event sequence runtime, and the PWM
   stall guard run on the expander.  Anything multi-channel and not
   tick-precise (gear-deploy choreography, recoil composite effects,
   landing-light groups) belongs on the master.
4. **Hardware safe-state on master loss.**  When the master stops
   sending traffic for `keepaliveTimeout_ms` (default 2000 ms) while
   in `EXPANDER` mode, the board MUST `enterSafeState()` — park every
   servo at its safe-state pulse, drive every PWM to 0, blank every
   LED.  the expander is the safety floor; even with the master crashed,
   no actuator stays at a destructive position.
5. **Compile-time component layout.**  Channel counts, GPIO pins,
   expander wiring — all template parameters or constexpr arrays.  No
   runtime channel-count negotiation.  Master discovers the layout via
   `COMPONENT_LIST_REQ`; the expander never reshapes itself.
6. **C++20 concepts over polymorphism** ([Rule 18](../.github/copilot-instructions.md)).
   `ServoCollection<N, TServoCtrl>`, `PwmCollection<M, TSense>`,
   `LedCollection<K, TGpio, TPwmSink>` — all gated by concepts.  No
   virtual interfaces inside the expander runtime.

---

## 2 · Anatomy of An expander firmware

A complete `expander_pico.ino`-style sketch.  The board-specific code
is just the **layout block** (servo pins, PWM pins, LED expander
wiring).  Everything else is library-supplied.

```cpp
#include <sfx_server.h>                        // SfxServer wrapper
#include <expander/expander_server.h>                // ExpanderServer<…>
#include <expander/board_identity.h>            // BoardIdentity
#include <collections/servo_collection.h>      // ServoCollection<N>
#include <collections/pwm_collection.h>        // PwmCollection<M, TSense>
#include <collections/led_collection.h>        // LedCollection<K, TGpio>
#include <storage/flash.h>                     // FlashModule (LittleFS)

// ── Compile-time component layout ────────────────────────────────────
sfx_peripherals::ServoCollection<6>            servos;
sfx_peripherals::PwmCollection<2, MyBoardSensing> pwms;
sfx_peripherals::LedCollection<8, NativeGpio>  leds;

sfx_expander::BoardIdentity                     boardIdent;

sfx_expander::ExpanderServer<decltype(servos),
                       decltype(pwms),
                       decltype(leds)>         expander;

SfxServer server(Serial);                      // core protocol wrapper

// ── Board-local indicator LEDs (separate from protocol-controlled LEDs)
StatusLed                                      indicator;

void setup() {
    Serial.begin(6'000'000);

    // 1. Compile-time hardware specs into each collection.
    servos.configure({
        ServoSpec{.pin = 2, .defaultMin_us = 900, .defaultMax_us = 2100, /*…*/},
        // ×6
    });
    pwms.configure({
        PwmSpec{.pin = 12, .defaultFreq_Hz = 20'000, /*…*/},
        // ×2
    });
    leds.configure({
        LedSpec{.pin = 22, /*…*/},
        // ×8
    }, &NativeGpio::instance());

    // 2. Restore the persisted identifier (read /board.yaml from flash).
    auto& flash = sfx_storage::FlashModule::instance();
    flash.begin();
    boardIdent.load([&](const char* path, char* buf, size_t bufLen) {
        return flash.readFile(path, buf, bufLen);
    });

    // 3. Hook the core server.
    server.begin("MyBoard", FW_VERSION, BUILD_NUMBER);
    server.core().addCapability(CoreCapability::FLASH);   // identifier persists in flash
    server.core().onI2CScan([] { return scanMyI2CBus(); });
    server.core().onStatusData([](uint8_t* buf, size_t maxLen) {
        return expander.appendStatusToCoreBlob(buf, maxLen);
    });

    // 4. Bind the expander server to its collections + identifier.
    expander.bind(&servos, &pwms, &leds, &boardIdent, {
        /*read =*/ [&](auto p, auto b, auto n) { return flash.readFile(p, b, n); },
        /*write=*/ [&](auto p, auto b, auto n) { return flash.writeFile(p, b, n); },
    });

    // 4a. (Optional) Bind a battery sensor.  Skip on boards without one.
    //     See §10 for the AdcDividerBatteryT / Ina226Battery variants.
    //     expander.bindBattery(&battery);
    //     server.core().addCapability(CoreCapability::BATTERY);

    // 5. Wire INIT / SHUTDOWN / KEEPALIVE to the expander's lifecycle.
    server.onInit     ([](uint8_t mode, uint8_t flags) { expander.handleInit(mode, flags); });
    server.onShutdown ([] { expander.handleShutdown(); });
    server.onKeepalive([] { expander.handleKeepalive(); });

    // 6. Register the expander dispatcher with the bus router.
    server.addModuleHandler(&expander);

    indicator.begin();   // board-local; not part of LedCollection
}

void loop() {
    server.poll();       // drives the wire — reads + writes
    expander.update();      // ticks servo / PWM / LED collections + watchdog
    indicator.update();  // board-local indicator LEDs
}
```

That's it.  The board contributes:

- the **layout** (channel counts + per-channel specs)
- the **hardware-specific glue**: GPIO expander class, sensing policy
  type (`MyBoardSensing`), I²C-scan callback, indicator-LED logic
- the **flash storage callbacks** for identifier persistence

Everything else — INIT lifecycle, dispatch of every `0x01..0x7F`
packet, async event emission, keepalive watchdog, safe-state, status
broadcast — is library-supplied and uniform across boards.

---

## 3 · Core protocol — what every expander inherits

`SfxServer` automatically registers a `CoreCommandServer` that handles
all of `CorePacket` (`0xEE..0xFF`).  These behave identically on
every expander; the board firmware only supplies callbacks for the
hooks it cares about.

### 3.1 Lifecycle

| Packet                  | ID    | Direction      | Meaning |
|---|---|---|---|
| `INIT`                  | 0xF0  | master → expander | `[mode:u8][flags:u8]` — start operating in EXPANDER / DIRECT mode |
| `INIT_READY`            | 0xF3  | expander → master | response to INIT (and to IDENTIFY) — board info + capabilities |
| `SHUTDOWN`              | 0xF1  | master → expander | graceful shutdown — expander parks safely, returns to IDLE |
| `KEEPALIVE`             | 0xF2  | master → expander | resets the expander's master-traffic watchdog |
| `IDENTIFY`              | 0xFE  | master → expander | non-activating query — same payload as `INIT_READY`, no transition |

**Lifecycle rules:**

- Power-on → `BoardState::IDLE`.  No actuators energised, no LEDs lit.
- `INIT(EXPANDER)` → `BoardState::EXPANDER`, attaches all collections,
  starts the keepalive watchdog.
- `INIT(DIRECT)` → `BoardState::DIRECT`, attaches all collections,
  no keepalive watchdog (CLI / Studio session expected).
- `SHUTDOWN` or master-loss timeout → `enterSafeState()` → back to
  `IDLE`.
- `IDENTIFY` returns the same `INIT_READY` payload but does **not**
  transition state — used by the master's `UsbRegistry` for safe
  type detection at enumerate-time before deciding whether to INIT.

The `INIT_READY` payload carries:

```
[nameLen:u8][name][verLen:u8][version][platLen:u8][platform]
[cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE][capabilities:u32LE]
```

The `capabilities` field is a Rule 11 append-only bitmask.  Expanders
typically advertise `CoreCapability::FLASH` (since they persist
`/board.yaml`); a board with onboard SD or audio hardware would OR
in the relevant bits.

### 3.2 STATUS

| Packet         | ID    | Direction      | Meaning |
|---|---|---|---|
| `STATUS_REQ`   | 0xFA  | master → expander | poll for one STATUS reply |
| `STATUS`       | 0xF4  | expander → master | core header + module-specific payload |

The 22-byte core header is fixed:

```
[counter:u32LE][uptime:u32LE][freeRam:u32LE]
[lastActivity:u32LE][keepaliveCount:u32LE]
[boardState:u8][initFlags:u8]
```

After it, each registered module appends bytes via the
`onStatusData(cb)` callback.  `ExpanderServer` has its own broadcast
path (`EXPANDER_STATUS_BROADCAST`) for the per-component arrays — the
core STATUS payload is for board-level counters only.  See §6 below
for the expander-protocol broadcast.

### 3.3 Reset / bootloader

| Packet      | ID    | Meaning |
|---|---|---|
| `REBOOT`    | 0xF8  | software reset (the `flash` CLI uses this when re-flashing without BOOTSEL) |
| `BOOTSEL`   | 0xF9  | enter the bootloader / DFU mode (Pico: jumps to ROM bootloader; ESP32-S3: enters download mode) |

`SfxServer` ships default implementations for both:

- `REBOOT`: ACKs, then triggers the platform's reset (`watchdog_reboot`
  on Pico, `esp_restart` on ESP32-S3) after a small delay so the ACK
  reaches the master.
- `BOOTSEL`: ACKs, then jumps to the ROM bootloader / DFU.

Boards rarely need to override these.  If the board has external
hardware that needs a clean shutdown before reboot (e.g. drop a
contactor), wire `server.core().onReboot(cb)` and do the work inside
the callback before the default reset fires.

> **Build flow tie-in.**  `scalefx-flash` sends `BOOTSEL` over the
> active CDC link, then waits for the RPI-RP2 USB drive (Pico) or the
> ESP32-S3 ROM bootloader to enumerate.  Without `BOOTSEL` support,
> flashing requires the user to physically hold the BOOTSEL button on
> every flash — Expanders MUST keep this command working.

### 3.4 I²C scan

| Packet              | ID    | Meaning |
|---|---|---|
| `I2C_SCAN`          | 0xFB  | master → expander: scan the I²C bus |
| `I2C_SCAN_RESULT`   | 0xFC  | expander → master: scan results |

The expander firmware supplies an `I2CScanCallback` describing which
addresses are *expected* (e.g. `INA226 @ 0x40`, `AW9523B @ 0x58`)
plus the actual bus probe.  Wire format:

```
[numExpected:u8]
  per expected device × N:
    [address:u8][found:u8][identified:u8]
[numExtra:u8]
  per extra device × M:
    [address:u8]
```

`identified` is true when a device-specific ID register check passed
(e.g., reading `INA226` `MFG_ID` returns the expected `0x5449`).
Boards that don't use I²C just omit the callback — the core server
returns an empty result.

### 3.5 Diagnostic logs

| Packet          | ID    | Direction      | Meaning |
|---|---|---|---|
| `LOG_MESSAGE`   | 0xFD  | expander → master | `[level:u8][millis:u32LE][message:str]` — async, unsolicited |
| `DIAG_HISTORY`  | 0xFF  | master → expander | request buffered log entries (replays without draining) |

The `DiagLog` singleton (in `sfx_platform`) buffers the most recent
log entries (severity `DEBUG..ERROR`) in a ring buffer.  Live logs go
out over the wire as `LOG_MESSAGE` async packets; the `DIAG_HISTORY`
reply replays the buffered ring so a master that connects
mid-session can see the boot trace.  Studio's "Diagnostics" pane and
the CLI `diag` command both consume this stream.

Use `DiagLog::instance().info("startup OK")` etc. from inside the
firmware — the routing is automatic.

**`ExpanderServer` already logs the high-value events** so an expander
firmware doesn't need to emit them by hand:

| Source                        | Level | What gets logged |
|---|---|---|
| `handleInit()`                | info  | `expander: INIT mode=N flags=0xNN — attaching collections` |
| Re-INIT while attached        | info  | `expander: re-INIT received while attached — parking first` |
| `handleShutdown()`            | info  | `expander: SHUTDOWN received` |
| `enterSafeState()`            | warn  | `expander: enterSafeState (was attached/idle)` |
| Keepalive timeout             | warn  | `expander: keepalive timeout (N ms since last master traffic) — entering safe state` |
| `IDENT_SET` invalid chars     | warn  | `expander: IDENT_SET rejected — invalid chars in '…'` |
| `IDENT_SET` flash failure     | error | `expander: IDENT_SET persistence failed for '…'` |
| `IDENT_SET` accepted          | info  | `expander: identifier set to '…'` |
| `BATTERY_RECONFIGURE`         | info  | `battery: reconfigured chem=N cells=N low=N crit=N` |
| `BATTERY_ALERT(LOW)`          | warn  | `battery: LOW — N mV across N cell(s)` |
| `BATTERY_ALERT(CRITICAL)`     | error | `battery: CRITICAL — N mV across N cell(s)` |
| `BATTERY_ALERT(OK)` (re-arm)  | info  | `battery: re-armed (OK) at N mV / N cell(s)` |
| `PWM_STALL` async             | warn  | `pwm[N]: stall guard tripped — peak=N mA after N ms` |

Board firmware can add its own `DiagLog::instance().info(...)` /
`warn(...)` / `error(...)` calls for board-specific events (e.g.
"INA226 channel 0 returned MFG_ID mismatch") and they'll flow
through the same `LOG_MESSAGE` / `DIAG_HISTORY` pipe.  Avoid `debug`
calls from per-loop hot paths — they fire on every iteration and
flood the master's history ring.

### 3.6 Async status updates

| Packet            | ID    | Meaning |
|---|---|---|
| `STATUS_UPDATE`   | 0xEF  | `[source:u8][type:u8][data:variable]` — async, opt-in |

A **generic** envelope for verbose async telemetry distinct from the
expander-protocol async events (which are typed and live in
`ExpanderPacket`).  In the new architecture Expanders rarely use this —
typed events (`SERVO_TARGET_REACHED`, `PWM_STALL`, …) are preferred.
Kept for back-compat with existing master-side parsers and for
ad-hoc diagnostic streams.

### 3.7 Battery configuration

| Packet              | ID    | Meaning |
|---|---|---|
| `BATTERY_CONFIG`    | 0xEE  | `[chemistry:u8][cellCount:u8]` — sets battery model on a `BatteryServerT`-equipped board |

Expanders that own battery monitoring (e.g., a wing tip board with its
own INA226) handle this via `BatteryServerT<TBattery>`.  Most expanders
delegate battery monitoring to the master, in which case this packet
is unhandled (BusServer dispatches it to the registered handler — if
none, the core server NACKs with `INVALID_COMMAND`).

### 3.8 Capability advertisement — what NOT to expose

`CoreCapability::*` flags advertised in `INIT_READY` tell the master
which command surfaces are available.  Expanders typically advertise:

- `FLASH` — yes, the expander persists `/board.yaml`
- `BATTERY` — yes, when the board has an onboard battery sensor
  (`AdcDividerBatteryT<…>` or `Ina226Battery`); see §10
- *(none of the others by default)*

Expanders DO NOT advertise:

- `SD` — there's no SD slot on an expander
- `AUDIO` — audio playback lives on HubFX
- `USB_HOST` — USB-host stack is a HubFX-only concern
- `ENGINE` — sound engine is HubFX-only
- `CONFIG` — expanders do **not** ship the YAML `ConfigServerT`
  surface; the only persisted state is the identifier (see §5)
- `EXPANDER_BUS` — Expanders don't enumerate other expanders

This separation matters: Studio's file-manager + config-tab paths
gate on these bits and silently skip expanders.  Don't OR in capabilities
the expander doesn't actually serve, and don't add new wire commands to
expanders that map to master-owned subsystems.

---

## 4 · expander protocol — `ExpanderPacket` (`0x01..0x7F`)

Full reference lives in
[`controllers/lib/sfx_serial/serial/expander/expander.h`](../controllers/lib/sfx_serial/serial/expander/expander.h).
Summary by block:

### 4.1 Identity / enumeration / status broadcast — `0x01..0x0F`

| ID    | Name                       | Meaning |
|---|---|---|
| 0x01  | `COMPONENT_LIST_REQ`       | query → live `ComponentList` (incl. current PWM modes) |
| 0x02  | `COMPONENT_LIST_RESP`      | `[count:u8][ComponentInfo×N]` — grouped by kind |
| 0x03  | `IDENT_GET_REQ`            | query → assigned identifier |
| 0x04  | `IDENT_GET_RESP`           | `[boardType:u8][len:u8][utf8 name]` |
| 0x05  | `IDENT_SET`                | `[len:u8][utf8 name]` — persisted to `/board.yaml`, ACK'd |
| 0x06  | `EXPANDER_STATUS_BROADCAST`   | async unified status (expander → master, TAG_ASYNC) |
| 0x07  | `EXPANDER_STATUS_RATE`        | `[hz:u8][kindsBitmask:u8]` — configures broadcast rate |
| 0x08  | `EXPANDER_STATUS_REQ`         | `[kindsBitmask:u8]` — sync status query (returns broadcast payload tagged with the request tag) |
| 0x09  | `BATTERY_INFO_REQ`         | query → `BATTERY_INFO_RESP` (always answered — boards without a battery reply with `present=0`) |
| 0x0A  | `BATTERY_INFO_RESP`        | `[present:u8]` + (if present) chemistry / cellCount / voltage_mV / cellVoltage_mV / percentage / flags / profile thresholds |
| 0x0B  | `BATTERY_RECONFIGURE`      | `[chemistry:u8][cellCount:u8][customLow_mV:u16LE][customCritical_mV:u16LE]` — full battery config (extended replacement for `BATTERY_CONFIG` 0xEE) |
| 0x0C  | `BATTERY_ALERT`            | async — `[level:u8][voltage_mV:u16LE][cellCount:u8]` — fired on every low / critical / re-arm transition |

`COMPONENT_LIST_RESP` is the **canonical re-enumeration mechanism**.
It returns the *live* current mode of every component — a PWM channel
that's been reconfigured to `PwmMotor` reports back as `PwmMotor`,
not as its compile-time default.  The master re-queries whenever it
needs a fresh fingerprint (e.g., after sending PWM_RECONFIGURE).

### 4.2 Servo collection — `0x10..0x2F`

Full intelligent-servo control surface owned by `ServoControl` in
`sfx_peripherals/servo/`.  Trapezoidal profile, soft limits, jerk
offset for transient shocks, async target-reached + motion-update
events.

| ID    | Name                       | Meaning |
|---|---|---|
| 0x10  | `SERVO_SET`                | `[idx:u8][position_us:u16LE]` — set target |
| 0x11  | `SERVO_CONFIG`             | full per-channel config (range + motion) |
| 0x12  | `SERVO_QUERY`              | query → position / target / velocity |
| 0x13  | `SERVO_QUERY_RESP`         | response |
| 0x14  | `SERVO_TARGET_REACHED`     | async — fired once per command on convergence |
| 0x15  | `SERVO_APPLY_JERK`         | `[idx:u8][offset_us:i16LE][duration_ms:u16LE]` |
| 0x16  | `SERVO_SET_MOTION`         | tune speed / accel / decel only |
| 0x17  | `SERVO_HOLD`               | soft-disable PWM output (for low-power hold / bench setup) |
| 0x18  | `SERVO_MOTION_UPDATE`      | async — periodic mid-motion (pos, target, vel) |
| 0x19  | `SERVO_MOTION_UPDATES`     | `[enable:u8][rate_hz:u8]` — toggle motion update emission |

Use `SERVO_MOTION_UPDATES` to opt into mid-motion telemetry only
during sequences that benefit from it (gear-deploy progress) — the
default is silent (target-reached only).

### 4.3 PWM collection — `0x30..0x4F`

Mode-mutable channels: `PwmGeneric` / `PwmLed` / `PwmMotor` /
`PwmHeater`.  Two reconfiguration paths:

- **`PWM_SET_MODE` / `PWM_SET_FREQ`** — incremental, "change one thing".
- **`PWM_RECONFIGURE`** — atomic full-config swap (mode + freq +
  polarity + soft duty limit in one packet).  Use when multiple
  parameters change simultaneously so the channel never sees an
  inconsistent mid-state.

| ID    | Name                       | Meaning |
|---|---|---|
| 0x30  | `PWM_SET_MODE`             | `[idx:u8][mode:u8]` — switch to PwmLed/PwmMotor/… |
| 0x31  | `PWM_SET_DUTY`             | `[idx:u8][duty:u16LE]` (0..1000 ‰) |
| 0x32  | `PWM_SET_MOTOR`            | `[idx:u8][speed:i16LE]` (-1000..+1000 signed) |
| 0x33  | `PWM_SET_HEATER`           | duty or target temperature based on flags |
| 0x34  | `PWM_SET_FREQ`             | runtime frequency change |
| 0x35  | `PWM_QUERY`                | live readback (mode, duty, voltage_mV, current_mA) |
| 0x36  | `PWM_QUERY_RESP`           | response |
| 0x37  | `PWM_RECONFIGURE`          | atomic full-config swap |
| 0x38  | `PWM_GET_CONFIG`           | configuration snapshot (capabilities + flags) |
| 0x39  | `PWM_GET_CONFIG_RESP`      | response |
| 0x3A  | `PWM_SET_STALL_GUARD`      | `[idx:u8][threshold_mA:u16LE][debounce_ms:u8][stallFlags:u8]` |
| 0x3B  | `PWM_CLEAR_STALL`          | re-arm a latched channel |
| 0x3C  | `PWM_STALL`                | async — fired when guard trips |

Mode-transition side effects (excerpt from the refactor doc):

- Switching INTO `PwmLed`: prior duty drops to 0; LedCollection adopts
  the channel; no LED program is running until `LED_PROGRAM_RUN`.
- Switching OUT of `PwmLed`: any running LED program is stopped (no
  `LED_PROGRAM_DONE` event since this is master-initiated); duty drops
  to 0.
- Switching to `PwmHeater`: duty=0 until the master sets a target.

### 4.4 LED collection — `0x50..0x7F`

Event-sequence runtime owned by `LedManager` /
`sfx_peripherals/led/`.  Events: `ON` / `OFF` / `FLASHING` /
`FADE_IN` / `FADE_OUT` / `FADING` / `BEACON`.  Programs can be
one-shot (emit `LED_PROGRAM_DONE` on completion) or looped (`REPEAT`
flag — never emits done).

| ID    | Name                          | Meaning |
|---|---|---|
| 0x50  | `LED_SET_BRIGHTNESS`          | `[addr:u8][brightness:u8]` |
| 0x51  | `LED_PROGRAM_LOAD`            | `[addr:u8][progId:u8][eventCount:u8][LedEvent×N]` |
| 0x52  | `LED_PROGRAM_RUN`             | `[addr:u8][progId:u8][flags:u8]` |
| 0x53  | `LED_PROGRAM_STOP`            | stop without finishing |
| 0x54  | `LED_QUERY`                   | live state |
| 0x55  | `LED_QUERY_RESP`              | response |
| 0x56  | `LED_PROGRAM_DONE`            | async — fired on natural completion (one-shot only) |
| 0x57  | `LED_PROGRAM_RESTART`         | restart current program from event 0 |
| 0x58  | `LED_RESET_CHANNEL`           | full channel reset (addr=0xFF for all) |
| 0x59  | `LED_ENABLE_CHANNEL`          | gate channel without losing its program |
| 0x5A  | `LED_SET_MASTER_BRIGHTNESS`   | global 0..100 % multiplier |
| 0x5B  | `LED_SEQ_STATUS_REQ`          | detailed sequence status (event index, type, repeat count) |
| 0x5C  | `LED_SEQ_STATUS_RESP`         | response |

The address byte spans both pools — bit 7 selects:

- `0x00..0x7F` → dedicated LED (LedCollection)
- `0x80..0xFF` → PWM-borrowed LED (PwmCollection in `PwmLed` mode)

A single LED-protocol command set targets both — the master doesn't
need to know where the physical output sits.

### 4.5 Async event taxonomy

Events flagged `TAG_ASYNC` in the protocol header — emitted
unsolicited by the expander's collection update tick:

| Event                       | Trigger |
|---|---|
| `SERVO_TARGET_REACHED`      | servo profile converges on the latest commanded target |
| `SERVO_MOTION_UPDATE`       | periodic mid-motion (10 Hz default; opt-in) |
| `PWM_STALL`                 | stall guard trips on a `PwmMotor` channel |
| `LED_PROGRAM_DONE`          | one-shot LED program reaches its last event |
| `BATTERY_ALERT`             | low / critical / re-arm transition with hysteresis |
| `EXPANDER_STATUS_BROADCAST`    | configurable rate (1 Hz default; up to 10 Hz) |
| `LOG_MESSAGE` (core 0xFD)   | `DiagLog` entries above the configured severity |

Async events are *the* mechanism for closing the high-level loop on
the master: gear-deploy advances on `SERVO_TARGET_REACHED`, motor
calibration learns endpoints from `PWM_STALL`, lighting effects
chain on `LED_PROGRAM_DONE`.  No master-side timers, no polling.

### 4.6 Universal port ID

`ExpanderPacket::PortId` packs `(kind:3, idx:5)` into one byte:

```
   bits 7–5: kind (Servo=1, Pwm=2, LedDed=3, LedPwm=4, …)
   bits 4–0: index within the kind (0..31)
```

Used in cross-cutting events / log entries / stall traces where a
single byte must identify any component on the expander without a
separate `(kind, idx)` tuple.  Decode helpers in the namespace.

---

## 5 · Persistence — what An expander is allowed to store

An expander board has **one** persistent file: `/board.yaml`.  It holds
the assigned identifier (the human-readable name set via
`IDENT_SET`).  Schema:

```yaml
type: gunfx       # board type (read-only — fixed at compile time)
name: "Wing-L"    # human-assigned alias, persisted across reboots
```

### 5.1 What Expanders DO NOT have

The general file-system surface (`FILE_LIST`, `FILE_DOWNLOAD`,
`FILE_UPLOAD_*`, `FILE_DELETE`, `FILE_MKDIR`, `FILE_TREE`) lives in
`controllers/lib/sfx_storage/server/storage_server.h` and is only
registered by the **HubFX master**.  Expanders DO NOT expose:

- arbitrary file upload / download
- directory listing / creation / deletion
- SD card commands (no SD slot anyway)
- the YAML `ConfigServerT` surface (`CONFIG_RELOAD`, `CONFIG_SAVE`,
  `CONFIG_STATUS`)

This is deliberate: expanders are stateless from the master's view aside
from their identifier.  All board-level configuration (servo trim,
LED programs, motor calibration values) is **pushed by the master at
INIT time** — typically from a YAML file the master loaded for that
board type.  See §7 for the per-board config-push pattern.

### 5.2 Identifier persistence wiring

the expander's `BoardIdentity` takes two callback functions at `load()`
time:

```cpp
auto& flash = sfx_storage::FlashModule::instance();
flash.begin();

boardIdent.load(
    /*read =*/ [&](const char* path, char* buf, size_t bufLen) -> int {
        return flash.readFile(path, buf, bufLen);
    });

expander.bind(&servos, &pwms, &leds, &boardIdent, {
    /*read =*/ [&](auto p, auto b, auto n) { return flash.readFile(p, b, n); },
    /*write=*/ [&](auto p, auto b, auto n) { return flash.writeFile(p, b, n); },
});
```

Why injected callbacks rather than a hard `LittleFS` dependency:
boards on alternative platforms (e.g., a future ESP32-C3 expander) can
swap in a different storage backend without touching `sfx_expander`.

### 5.3 What gets persisted automatically

`ExpanderServer` itself writes `/board.yaml` whenever an `IDENT_SET`
arrives with a valid name.  Boards never write the file directly.

If a board has additional non-identifier persistent state (e.g., a
thermistor calibration constant), wire it through a separate
`storage_config_bridge` — but this is unusual and should be
challenged before adding.  The architectural default is "all config
comes from the master at INIT time".

---

## 6 · Unified status broadcast

the expander emits one **unified status packet** that bundles the live
state of every component.  Saves the master from polling `SERVO_QUERY
× N + PWM_QUERY × M + LED_QUERY × K` packets per refresh.

Wire format (`EXPANDER_STATUS_BROADCAST` and the response to
`EXPANDER_STATUS_REQ`):

```
header        [boardState:u8][mode:u8][uptime_ms:u32LE][freeRam:u32LE]
servoCnt      [count:u8] × { port_id:u8, pos:u16LE, target:u16LE, vel:i16LE, flags:u8 }
pwmCnt        [count:u8] × { port_id:u8, mode:u8, duty:u16LE,
                             voltage_mV:i16LE, current_mA:i16LE,
                             stallFlags:u8, peak_mA:u16LE }
ledCnt        [count:u8] × { port_id:u8, brightness:u8, progState:u8, progId:u8 }
batteryPresent [u8]     // 0 = no battery on this board → no further bytes
   if present:           { chemistry:u8, cellCount:u8,
                           voltage_mV:u16LE, cellVoltage_mV:u16LE,
                           percentage:u8, flags:u8 }
```

Per-kind sub-blocks shrink to size 0 if the expander has no components
of that kind.  The battery section is **always one byte minimum** —
`present = 0` on `NoBattery` boards or when filtered out via
`StatusKinds::BATTERY` — so master-side parsers can read the
trailing flag unconditionally.  Total size is bounded by the COBS
payload limit (512 B), which comfortably covers the 6+8+8 + battery
worst case.

Configurability:

- `EXPANDER_STATUS_RATE` — `[hz:u8][kindsBitmask:u8]`.  `hz=0` disables
  the broadcast; `1..10 Hz` enables periodic emission.  `kindsBitmask`
  filters which sections appear (default 0 = all).  The battery
  section bit is `StatusKinds::BATTERY = 1<<3`.
- `EXPANDER_STATUS_REQ` — `[kindsBitmask:u8]` — synchronous poll.
  Returns the SAME payload tagged with the request tag.

The default rate is **disabled** — the master enables it when needed
(e.g. during a gear-deploy cycle or live-tuning a servo) and disables
it afterward.

---

## 7 · Component layout — the board's identity

an expander's "type" is its **compile-time ComponentList**: the tuple of
servo count, PWM count + per-channel capability flags, and LED count.
The fingerprint a master receives via `COMPONENT_LIST_RESP` makes it
discoverable.

### 7.1 ServoSpec

```cpp
struct ServoSpec {
    uint8_t  pin;                  // GPIO
    uint16_t defaultMin_us;
    uint16_t defaultMax_us;
    uint16_t defaultCenter_us;
    uint16_t defaultMaxSpeed;      // µs/s
    uint16_t defaultAccel;         // µs/s² accel ramp
    uint16_t defaultDecel;
    uint16_t safeStatePos_us;      // 0 ⇒ default to center
};
```

`safeStatePos_us` is the position the channel parks at on
`enterSafeState()` — typically center for symmetric controls
(steering yaw), but `closePosition` for irreversible doors.

### 7.2 PwmSpec

```cpp
struct PwmSpec {
    uint8_t       pin;
    uint16_t      defaultFreq_Hz;
    uint8_t       defaultMode;     // ComponentKind::PwmGeneric/PwmLed/PwmMotor/PwmHeater
    uint8_t       capabilityFlags; // PwmFlags — what modes the channel SUPPORTS

    // Motor-only fields (used when mode = PwmMotor)
    uint8_t       motorTopology;   // SinglePinPwm / HBridgeDualGpio / HBridgePwmDir
    uint8_t       motorCwPin;
    uint8_t       motorCcwPin;
    bool          motorInvertDir;
    bool          motorBrakeCapable;

    // Sensing — opt-in capability flags
    bool          voltageSenseAvailable;
    bool          currentSenseAvailable;
};
```

`capabilityFlags` documents what modes the *hardware* supports — the
master can set the channel to any flagged mode.  Switching to a mode
without the flag set returns `WRONG_COMPONENT_KIND`.

### 7.3 LedSpec

```cpp
struct LedSpec {
    uint8_t  pin;                  // index into the TGpio expander's pin space
    uint8_t  defaultBrightness;
    uint8_t  capabilityFlags;      // LedFlags — pwm-capable, polarity, etc.
};
```

`TGpio` is a template parameter — the board picks `NativeGpio` for
direct MCU pins, an `Aw9523Gpio` adapter for an I²C expander, an
`Pca9685Gpio` adapter for a 16-channel PWM expander, etc.  All
satisfy the `GpioExpander` concept gated on `LedCollection`'s
`requires`-clause.

### 7.4 Sensing policy

`PwmCollection<M, TSense>` is templated on a `SensePolicy` concept
type.  The board supplies a struct with channel-level voltage / current
read methods — typically a small wrapper over an INA226 or the
platform's analog inputs.  Boards with no sensing use `NoSensing` (the
default).  See [`sense_policy.h`](../controllers/lib/sfx_peripherals/collections/sense_policy.h)
for the concept definition.

---

## 8 · Hardware safety — `enterSafeState()`

Triggered by:

1. Keepalive watchdog timeout (only in `EXPANDER` mode).
2. Explicit `SHUTDOWN` packet.
3. Re-entrant `INIT(EXPANDER)` (master rejoin) — re-INIT also parks
   first to clear any residual state.

What it does (atomic from the protocol's view):

```
servos.parkAtNeutral();    // each ServoControl ramps to safeStatePos_us
pwms.allOff();             // every PWM channel → duty 0 (motors coast or brake per StallFlags)
leds.allOff();             // every LED channel → brightness 0
_attached = false;         // collection .update() becomes a no-op
```

Each collection emits `ComponentEvent::SafeStateEntered` for any
per-channel hooks the board has registered (typically board-local
indicator LEDs going amber to flag "dead-master mode").

> **DIRECT mode does not enforce keepalive** — a CLI session can take
> seconds between commands without tripping the watchdog.  an expander
> that needs hard fail-safe in DIRECT mode should layer its own
> watchdog over the top.

---

## 9 · Build setup

### 9.1 Library wiring (PlatformIO)

An expander firmware's `platformio.ini` pulls in the library tree from
`controllers/lib/`:

```ini
[env:my_expander]
platform = raspberrypi
board    = pico
framework = arduino
build_flags =
    -std=gnu++20
    -DBUILD_NUMBER=42
    -DFW_VERSION=\"0.1.0\"
lib_deps =
    sfx_platform
    sfx_serial
    sfx_server
    sfx_peripherals
    sfx_expander
    sfx_storage      ; only because we need FlashModule for /board.yaml
    sfx_audio        ; OMIT — Expanders don't do audio
```

Required libraries:

- `sfx_platform`    — cross-platform macros (`SFX_DELAY_MS`, `SfxMutex`)
- `sfx_serial`      — protocol headers (core, expander)
- `sfx_server`      — `SfxServer` boilerplate wrapper
- `sfx_peripherals` — collections + servo/LED/PWM primitives
- `sfx_expander`       — `ExpanderServer<…>` template
- `sfx_storage`     — `FlashModule` for `/board.yaml`

Forbidden (master-only) libraries:

- `sfx_audio`, `sfx_usb`, `sfx_config` — no use case on an expander

### 9.2 Build flags

- `-std=gnu++20` is non-negotiable (Rule 18).  C++20 concepts gate
  every collection template.
- `BUILD_NUMBER` is auto-incremented by `scalefx-flash` on every
  flash — the firmware echoes it in `INIT_READY`.
- `FW_VERSION` follows semver.  Bump MAJOR for wire-breaking changes,
  MINOR for additive (Rule 11), PATCH for logic-only.

---

## 10 · Battery monitoring — optional 4th template parameter

An expander board MAY have an onboard battery sensor.  Boards that do
parameterise `ExpanderServer` on a `TBattery` policy; boards that don't
let it default to `NoBattery` (a no-op stub) and inherit the right
wire-protocol behaviour for free — `BATTERY_INFO_RESP` returns
`present=0`, the status broadcast emits a single `0` byte for the
battery section, `BATTERY_RECONFIGURE` NACKs, and `INIT_READY` does
not advertise `CoreCapability::BATTERY`.

```cpp
// Battery-equipped expander (ADC + 50k/10k divider on GP29):
sfx_peripherals::AdcDividerBatteryT<6000>  battery;
sfx_expander::ExpanderServer<decltype(servos),
                       decltype(pwms),
                       decltype(leds),
                       decltype(battery)>  expander;

void setup() {
    // …configure servos / pwms / leds…
    battery.begin(29, BatteryChemistry::LIPO);

    server.begin("WingBoard", FW_VERSION, BUILD_NUMBER);
    server.core().addCapability(CoreCapability::FLASH | CoreCapability::BATTERY);

    expander.bind(&servos, &pwms, &leds, &boardIdent, identStorage);
    expander.bindBattery(&battery);     // ← wire the policy

    server.addModuleHandler(&expander);
}
```

Boards without a battery skip both `bindBattery()` and the BATTERY
capability bit; the default `TBattery = NoBattery` does the rest.

### 10.1 Two policy variants

Both implementations satisfy the `BatteryPolicy` concept (defined in
[`battery_server.h`](../controllers/lib/sfx_peripherals/power/battery_server.h))
and share the same `BatteryStateMachine` for cell-count auto-detect,
hysteresis, and SOC% interpolation.  Pick whichever matches the
hardware:

| Variant                            | When to use | Hardware |
|---|---|---|
| `AdcDividerBatteryT<MultiplierMilli>` | Cheap RP2040/2350 boards with a resistor divider on a spare ADC pin | `R_top + R_bottom` divider into a 12-bit ADC; multiplier expressed ×1000 (`÷6.0` → `<6000>`).  Convenience aliases `AdcDividerBatteryX5_1`, `X6_0`, `X11`. |
| `Ina226Battery`                    | Boards already using INA226 for power diagnostics — bind one channel as the battery rail | I²C `INA226` on the same bus as motor / rail sensing |
| `NoBattery`                        | Default for boards without a battery | (none — stub) |

Each backend exposes the same surface — chemistry / cell count
configuration, low / critical thresholds, alert callbacks — so the
ExpanderServer's dispatch is policy-agnostic.

### 10.2 Wire-format surface — what the master sees

the expander exposes battery state through three identity-block packets
plus a section in the unified status broadcast:

| Packet                | Direction      | Purpose |
|---|---|---|
| `BATTERY_INFO_REQ`    | master → expander | query → `BATTERY_INFO_RESP`; valid pre-INIT (battery sampling runs even outside attached state so the master can read voltage during enumeration) |
| `BATTERY_INFO_RESP`   | expander → master | full snapshot: present flag, chemistry, cell count, voltage / cell voltage, percentage, flags, profile thresholds |
| `BATTERY_RECONFIGURE` | master → expander | full reconfig: chemistry, cell count, optional custom low / critical mV (per cell) — extends the legacy `BATTERY_CONFIG` (0xEE) which only carried chemistry + cellCount |
| `BATTERY_ALERT`       | expander → master | TAG_ASYNC; emitted on every low / critical / re-arm transition |

Plus the **battery section** appended to every
`EXPANDER_STATUS_BROADCAST` / `EXPANDER_STATUS_REQ` reply (see §6) — gated
by `StatusKinds::BATTERY`.

Capability gating:

- `INIT_READY` advertises `CoreCapability::BATTERY` (bit 7).
- Studio / CLI gate live battery-tab probes on this bit.
- Firmware that pre-dates the field decodes `capabilities = 0`
  (Rule 11) — Studio falls back to a single `BATTERY_INFO_REQ`
  probe and inspects the `present` byte.

### 10.3 Alerts + hysteresis

`BatteryStateMachine` (shared between both backends) supplies
`HYSTERESIS_PER_CELL_mV = 50`.  Once `low` fires the voltage must
rise back above `(low_threshold + 50 mV/cell) × cellCount` before
the expander re-arms and emits a `BATTERY_ALERT(level=OK)` async
packet.  Critical follows the same pattern.  Master parsers see
clean transitions, never threshold chatter.

`emitBatteryAlert()` runs from the policy's update tick (i.e., on
the protocol task) — no locking, packet emission is direct.

### 10.4 Battery in identity output

The master's `UsbRegistry` collects the following at enumeration:

- `INIT_READY.capabilities & CoreCapability::BATTERY` — fast bit
  check.  Drives Studio's "battery tab visible?" UI gate.
- `BATTERY_INFO_RESP` — the full snapshot.  Drives the live
  battery card (voltage bar, percentage, low / critical badges).

The `present` byte in `BATTERY_INFO_RESP` is the canonical
"is there actually a pack plugged in?" signal — separate from the
capability bit, which only says "this board can monitor a battery".
A board with the sensor wired but no pack reads `present = 0`
(below `MIN_DETECT_mV`, or USB-powered with `cellCount` not pinned).

### 10.5 Reconfiguration flow

```
master                                      expander
──────                                      ────────
1. INIT_READY (capabilities & BATTERY)  ◄───
2. BATTERY_INFO_REQ                     ───►
                                        ◄─── BATTERY_INFO_RESP
3. BATTERY_RECONFIGURE                  ───►   (chem=LiPo, cells=4,
                                                low=3300, crit=3000)
                                        ◄─── ACK
4. (later)                              ◄─── BATTERY_ALERT(level=LOW)
5. (re-arm)                             ◄─── BATTERY_ALERT(level=OK)
```

The legacy `BATTERY_CONFIG` (0xEE) packet still works on boards
running a `BatteryServerT<TBattery>` alongside `ExpanderServer` — but
new firmware should prefer `BATTERY_RECONFIGURE` since it carries
the custom thresholds in one round-trip.

### 10.6 What the master does with this

- **Logs / Studio battery card** — render voltage / percentage /
  alert badges from the snapshot + status section.
- **Effect interlocks** — the gear effect's `isInterlockSafe()`
  refuses deploys when the expander reports `BatteryAlertLevel::CRITICAL`.
  See [`gearcontrol_effect.cpp`](../controllers/hubfx/esp32s3/src/effects/gearcontrol/gearcontrol_effect.cpp).
- **Audio engine ducking** — the master can auto-attenuate amp
  output on `low` to extend pack life.
- **Logging** — every `BATTERY_ALERT` is logged via `DiagLog`.

Master-side battery monitoring (HubFX's own pack via INA226) is
still owned by the master and doesn't go through these expander
packets — they're strictly for expander-attached batteries.

---

## 11 · Migration recipe — porting a legacy board

Use the existing `gearcontrol_pico` migration (in progress) as the
worked example.  Steps for any legacy board:

1. **Inventory the high-level effects.**  Anything sequenced
   (gear-deploy, recoil, landing-light groups, sound-triggered
   composites) MOVES TO HUBFX.  Build a master-side orchestrator
   under `controllers/hubfx/esp32s3/src/effects/<board>/` first —
   see `controllers/hubfx/esp32s3/src/effects/gearcontrol/` for a
   reference layout (door + gear sequencers + effect orchestrator).
2. **Map peripherals to a `ComponentList`.**  Every motor → PWM
   channel.  Every LED → LedCollection slot or PWM-borrowed slot.
   Every servo → ServoCollection slot.  Sensors → `SensePolicy`
   adapter.
3. **Delete the old protocol header.**  `serial/<board>/<board>.h`
   gets retired wholesale (per the no-compatibility-window rule of
   the pivot).  Its packet IDs are now reserved space in the
   `ExpanderPacket` allocation.
4. **Delete the old server / client.**  `sfx_boards/<board>/server`,
   `sfx_boards/<board>/client`, the matching Go protocol mirror, the
   Go `api/<board>.go`, the engine handler.  Replaced by generic
   `ExpanderApi` (already exists).
5. **Write the new firmware sketch.**  ~50 lines following §2.
   Wire indicator LEDs as a board-local concern — they're NOT part
   of `LedCollection` (which is protocol-controlled).
6. **Promote any reusable hardware drivers** (motor stall detection,
   sense adapters, expander drivers) into `sfx_peripherals/` rather
   than re-inventing them under the board.  The `StallDetector` lift
   from `gearcontrol/` to `sfx_peripherals/motor/` is the canonical
   example.
7. **Wire master-side config push.**  The master loads the board's
   YAML at boot, sets the expander's component config via
   `EXPANDER_*` / `SERVO_*` / `PWM_*` / `LED_*` commands at INIT time
   (`UsbRegistry::onReady`).  See Rule 17 for the per-board
   `pushXxxConfigToExpander(cfg, client)` pattern.
8. **Run the test harness.**  Tests under `tests/` exercise the
   protocol surface; the migrated board must pass before its old
   firmware is removed.

---

## 12 · Things to avoid

- **No high-level state machines on an expander.**  If you find yourself
  writing a "trigger sequence" or "phase enum" in expander code, move
  it to the HubFX master.  the expander only owns timing-precise per-
  component runtimes.
- **No board-specific wire packets.**  If a feature can't be expressed
  through the existing `ExpanderPacket` surface, propose an extension
  via [Rule 11](../.github/copilot-instructions.md) in `expander.h` —
  not a per-board header.
- **No `STANDALONE` / autonomous mode.**  Boot quietly, wait for INIT.
  Don't drive any actuator until the master has spoken.
- **No raw `delay()` / `sleep_ms()` / `delayMicroseconds()`.**  Use
  `SFX_DELAY_MS` from `sfx_platform.h` if you genuinely must block;
  prefer non-blocking polling in `loop()`.
- **No `volatile` for cross-core variables** ([Rule 15](../.github/copilot-instructions.md)).
  Use `std::atomic<T>` with explicit memory order.
- **No file system commands on the expander wire surface.**  Persistence
  is `/board.yaml` only (§5).
- **No virtual interfaces inside the expander runtime.**  Concepts +
  `requires` clauses, not polymorphism.
- **No emojis in code or comments.**  No marketing prose either.

---

## 13 · Cross-references

- [`controllers/lib/sfx_serial/serial/expander/expander.h`](../controllers/lib/sfx_serial/serial/expander/expander.h)
  — wire-format reference (authoritative).
- [`controllers/lib/sfx_serial/serial/core/core.h`](../controllers/lib/sfx_serial/serial/core/core.h)
  — core protocol packets, capabilities, error codes.
- [`controllers/lib/sfx_expander/expander/expander_server.h`](../controllers/lib/sfx_expander/expander/expander_server.h)
  — `ExpanderServer<TServos, TPwms, TLeds, TBattery>` declaration (expander firmware side).
- [`controllers/lib/sfx_expander/client/expander_client.h`](../controllers/lib/sfx_expander/client/expander_client.h)
  — `ExpanderClient` typed master-side API + observer chain (HubFX firmware side).
- [`controllers/lib/sfx_peripherals/collections/`](../controllers/lib/sfx_peripherals/collections/)
  — servo / PWM / LED collections + concepts.
- [`controllers/lib/sfx_peripherals/power/`](../controllers/lib/sfx_peripherals/power/)
  — battery monitoring: `BatteryStateMachine`, `AdcDividerBatteryT<…>`,
  `Ina226Battery`, `NoBattery`, `BatteryServerT` + `BatteryPolicy` concept.
- [`controllers/hubfx/esp32s3/src/effects/`](../controllers/hubfx/esp32s3/src/effects/)
  — master-side effect orchestrators (the "high-level" layer that
  used to live on the expanders).
- [`15-GENERIC-EXPANDER-REFACTOR.md`](15-GENERIC-EXPANDER-REFACTOR.md)
  — the pivot's design rationale + per-board migration plan.
- [`01-ARCHITECTURE.md`](01-ARCHITECTURE.md)
  — system-wide context.
- [`03-PROTOCOL-EXTENSION.md`](03-PROTOCOL-EXTENSION.md)
  — process for adding a packet ID under Rule 11.
