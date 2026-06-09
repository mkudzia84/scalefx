# Expander Board Design Guide

> **Audience:** firmware engineers building a NEW expander board, or
> studying the two shipping ones (LightFX / GearControl) to add a third.
>
> **Status: 2026-06-09.** Rewritten onto the current **Ports / Roles**
> model. The old `ExpanderServer<TServos, TPwms, TLeds>` + the three
> "collections" (`ServoCollection` / `PwmCollection` / `LedCollection`) +
> the `ExpanderPacket 0x01..0x7F` range + `addModuleHandler()` are all
> GONE — replaced by `BoardOf<...>` + static port descriptors +
> `RoleServicePolicy`. An expander is now a thin board running the SAME
> `RoleServicePolicy` every board runs; the hub drives its roles
> transparently by opaque `PortRef` ([Rule 58](../.github/copilot-instructions.md)).
>
> **Companion docs:**
> [`17-SYSTEM-SERVICES.md`](17-SYSTEM-SERVICES.md) (the policy framework, RFC
> history), [`31-GUID-PORT-ROUTING.md`](31-GUID-PORT-ROUTING.md) (how the hub
> addresses a remote port), [`32-ARCHITECTURE-DIAGRAMS.md`](32-ARCHITECTURE-DIAGRAMS.md)
> §3 (port/role data-flow diagrams), and [`01-ARCHITECTURE.md`](01-ARCHITECTURE.md)
> (the wider system). [`15-GENERIC-EXPANDER-REFACTOR.md`](15-GENERIC-EXPANDER-REFACTOR.md)
> is the historical planning doc for this pivot.
>
> **Worked examples (read these alongside the guide):**
> [`controllers/lightfx/pico/src/lightfx_pico.ino`](../controllers/lightfx/pico/src/lightfx_pico.ino)
> (8 PWM→LedAnimator + 3 servo→ServoActuator + ADC battery) and
> [`controllers/gearcontrol/pico/src/gearcontrol_pico.ino`](../controllers/gearcontrol/pico/src/gearcontrol_pico.ino)
> (7 servo→ServoActuator + 3 H-bridge→BiDcMotor, with INA226 current sense).

## 0 · TL;DR — what an expander board IS

An expander board is a **thin port + role host**. It declares its physical
**ports** (servo / pwm / hbridge / input) at compile time, and lets the
HubFX master **attach roles** onto those ports at runtime and drive them
over USB CDC. It owns no high-level behaviour — recoil sequences, gear-deploy
choreography, landing-light groups, LED programs all live on the HubFX master
(its effect-service policies). The expander only runs the per-role *runtime*
(servo motion profile, motor stall guard, LED animator tick).

A modern expander sketch is ~50–250 lines: a board class deriving
`BoardOf<...>`, the static port descriptors, optional local hardware
(I²C sensors, status LEDs), and a two-line `loop()`. It speaks **one**
protocol surface — the core lifecycle + `PortServicePolicy` +
`RoleServicePolicy` packets that EVERY board runs. There is no
board-specific wire protocol.

```
   ┌───────────────────────── Master (HubFX) ─────────────────────────┐
   │  Effect orchestrators: GunFX / LightFX / GearControl / EngineFX   │
   │  TopologyService: addresses any role by PortRef{guid,kind,idx}    │
   │    → ROLE_FORWARD (drive) · ROLE_QUERY (status) · ROLE_EVENT (tlm)│
   └───────────────────────────────┬──────────────────────────────────┘
                                   │  USB CDC host, COBS, 6 Mbps
                                   ▼
   ┌────────────────────── Expander board firmware ──────────────────────┐
   │  BoardOf<TBoard, TStream, PortCapacity<...>, ...ExtraPolicies>       │
   │   (auto-prepended, in order)                                         │
   │     BoardServicePolicy      ← INIT / IDENTIFY / KEEPALIVE / STATUS   │
   │     IndicatorServicePolicy  ← connection / error status LEDs         │
   │     PortServicePolicy       ← enumerate declared ports (PORT_LIST)   │
   │     RoleServicePolicy       ← ROLE_ATTACH / ROLE_BULK_ATTACH, drive  │
   │   (user ExtraPolicies, e.g. BatteryServicePolicy<…>)                 │
   │                                                                      │
   │   static kServoPorts / kPwmPorts / kHBridgePorts / kInputPorts       │
   │     → PortRegistry slots → roles emplace into a std::variant slot    │
   └──────────────────────────────────────────────────────────────────────┘
```

This guide is the design contract for the **expander board firmware** box.
The hub-side transport ("how does a `light-servo` command reach this board?")
is [Rule 58](../.github/copilot-instructions.md) + the TopologyService — see
[`32-ARCHITECTURE-DIAGRAMS.md`](32-ARCHITECTURE-DIAGRAMS.md) §3–4.

---

## 1 · Architectural rules

These are non-negotiable. Most map back to the
[`.github/copilot-instructions.md`](../.github/copilot-instructions.md)
rule set; they're restated here because they constrain the expander
firmware's shape.

1. **No autonomous behaviour.** An expander never runs a "standalone"
   mode. It boots into `BoardState::IDLE` and stays there until the
   master (or CLI) sends `INIT` — and even then it only energises a port
   once the hub has `ROLE_ATTACH`ed a role onto it. No autonomous PWM, no
   autonomous servo motion, no LED programs on power-up.
2. **One protocol surface, no board-specific packets.** Every expander
   speaks only the core lifecycle (`0xEF..0xFF`), `PortServicePolicy`, and
   `RoleServicePolicy` packets that the shared library supplies. A "LightFX
   board" and a "GearControl board" differ only in which ports they declare
   and which roles the hub attaches — never in their wire protocol. There is
   no `serial/lightfx/lightfx.h` or `ExpanderPacket` header.
3. **The hub owns the high-level state machines.** The servo motion
   profile, the motor stall guard, and the LED animator tick run **on the
   role, on the expander** (timing-precise per-actuator runtime). Anything
   multi-channel and sequenced (gear-deploy choreography, recoil composites,
   landing-light groups, RC program-select) runs **on the hub** as an
   effect-service policy that drives the roles by `PortRef`.
4. **Hardware safe-state on master loss.** When master traffic stops for
   `keepaliveTimeout_ms` while attached, the board parks every attached role
   at its safe state (servos → safe pulse, motors → coast/brake, LEDs →
   blank). The expander is the safety floor even with the master crashed.
5. **Compile-time port layout.** Port kinds, counts, GPIO pins, and the
   `PortCapacity<servo, pwm, hbridge, input>` that sizes the registry — all
   `static constexpr` descriptors. No runtime port-count negotiation; the hub
   discovers the layout via `PORT_LIST_REQ`. **Port DIRECTION is fixed at
   declaration** (Rule 31): `Servo`/`Pwm`/`HBridge` are output-only,
   `Input` is the single input kind — no runtime input/output swap.
6. **C++20 concepts over polymorphism** ([Rule 18](../.github/copilot-instructions.md)).
   Every policy satisfies the `SystemServicePolicy` concept; port and role
   templates are gated by `requires`-clauses. No virtual interfaces inside
   the expander runtime — dispatch is `std::apply` over the policy tuple.
7. **Native hardware only** ([Rule 55](../.github/copilot-instructions.md)).
   No Arduino I/O in `controllers/lib/`. The USB-CDC wire is wrapped as an
   `sfx::Stream` (`PicoSerialStream` on the Pico path); I²C via `SfxI2cBus`,
   GPIO/PWM via the native port classes. `<Arduino.h>` is permitted only in
   the Pico **sketch** (`#if SFX_PLATFORM_PICO`).

---

## 2 · Anatomy of an expander firmware

The board-specific code is the **board class** + the **port descriptors**.
Everything else — INIT lifecycle, port enumeration, role attach + drive,
keepalive watchdog, safe-state, status broadcast — is library-supplied by the
auto-prepended policies. Skeleton (compare with the two real sketches):

```cpp
#include <Arduino.h>                       // Pico path only (Rule 55)
#include <type_traits>

#include <platform/sfx_platform.h>
#include <platform/pico_serial_stream.h>   // USB-CDC Serial → sfx::Stream wire
#include <serial/diag_log.h>
#include <server/board_of.h>               // BoardOf<...> + PortCapacity<...>

#include <ports/pwm_port.h>                // NativePwmPort
#include <ports/servo_port.h>             // MicroservoPort
// #include <ports/hbridge_port.h>        // DualPwmHBridgePort (motors)
// #include <ports/input_port.h>          // InputPort (RC/SBUS/Jeti)

#define FIRMWARE_VERSION "1.0.0"
#define BUILD_NUMBER     1

namespace Gpio { constexpr int LED_CONNECTION = 24, LED_ERROR = 25; }

// USB-CDC wire type — Serial's concrete class wrapped as sfx::Stream.
using MyWireStream = sfx::PicoSerialStream<std::remove_reference_t<decltype(Serial)>>;

// BoardOf<TBoard, TStream, PortCapacity<servo,pwm,hbridge,input>, ...ExtraPolicies>
//   auto-prepends BoardServicePolicy + IndicatorServicePolicy +
//   PortServicePolicy + RoleServicePolicy, then appends the user policies.
//   PortCapacity sizes the PortRegistry EXACTLY to this board's hardware.
class MyBoard : public sfx_core::BoardOf<MyBoard, MyWireStream,
                                         sfx_core::PortCapacity</*servo*/3, /*pwm*/8,
                                                                /*hbridge*/0, /*input*/0>> {
public:
    sfx_peripherals::NativePwmPort   led[8]    = { NativePwmPort{0}, /* …GP1..7 */ };
    sfx_peripherals::MicroservoPort  servoOut[3] = { {8}, {9}, {10} };

    // Static port descriptors — consumed by BoardOf<> at compile time.
    static constexpr auto kPwmPorts   = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&MyBoard::led, 8>());
    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&MyBoard::servoOut, 3>());

    static constexpr const char* kName = "MyBoard";
};

MyBoard      board;
MyWireStream wireStream{Serial};

void setup() {
    Serial.begin(115200);                  // baud ignored over USB; brings endpoint up

    // (optional) bring up local hardware BEFORE board.begin() so the
    // registry sees it live when it walks each port's begin().

    // Policy-pack lifecycle: wire / DiagLog / indicator pins / port-registry
    // binding / every policy's begin() / IDENTIFY capability advertisement.
    board.begin(wireStream, FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);

    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);   // boot/attach trace on the wire
    SFX_LOG_INFO("MyBoard v%s build %u", FIRMWARE_VERSION, (unsigned)BUILD_NUMBER);
}

void loop() {
    board.process();   // drives the wire (RX framer + role drive) + ticks every policy
    busy_wait_ms(1);
}
```

The board contributes:

- the **port layout** — the `PortCapacity<>` + the static `kXxxPorts`
  descriptors + the concrete port member arrays;
- the **hardware-specific glue** — native port classes for the MCU pins, any
  I²C sensors (current/voltage sense for motor stall, ADC battery divider),
  and any board-local indicator/status LEDs;
- optional **ExtraPolicies** on the `BoardOf<>` pack (e.g.
  `BatteryServicePolicy<…>` for a board with an onboard pack sensor).

Everything else is library-supplied and uniform across boards.

### 2.1 The two shipping boards, side by side

| Board | `PortCapacity<>` | Port descriptors | Roles the hub attaches | Local-only hardware |
|---|---|---|---|---|
| **LightFX** | `<3, 8, 0, 0>` | `kPwmPorts` (8 LED PWM, GP0..7), `kServoPorts` (3, GP8/9/10) | `LedAnimator` ×8, `ServoActuator` ×3 | ADC battery divider on GP29 (via `BatteryServicePolicy`) |
| **GearControl** | `<7, 0, 3, 0>` | `kServoPorts` (7, door/yaw), `kHBridgePorts` (3 dual-PWM bridges) | `ServoActuator` ×7, `BiDcMotor` ×3 | 3× INA226 current/voltage sense; 6 per-motor status LEDs |

Both declare ZERO input ports — RC input is centralized on the HubFX master,
which drives these boards' roles over the wire (Rule 31: input ports are
HubFX-only).

---

## 3 · Core protocol — what every expander inherits

`BoardServicePolicy` (auto-prepended by `BoardOf<>`) handles the core
lifecycle (`0xEF..0xFF`). These behave identically on every board — expander
or hub. The board firmware supplies callbacks only for the hooks it cares
about (`board.core().onStatusData(...)`, `board.onInit(...)`, …).

### 3.1 Lifecycle

| Packet        | ID    | Direction         | Meaning |
|---|---|---|---|
| `INIT`        | 0xF0  | master → expander | `[mode:u8][flags:u8]` — start operating |
| `INIT_READY`  | 0xFE→ | expander → master | response to INIT (and to IDENTIFY) — board info + capabilities |
| `SHUTDOWN`    |       | master → expander | graceful shutdown — board parks safely, returns to IDLE |
| `KEEPALIVE`   |       | master → expander | resets the master-traffic watchdog |
| `IDENTIFY`    | 0xFE  | master → expander | non-activating query — same payload as `INIT_READY`, no transition |

**Lifecycle rules:**

- Power-on → `BoardState::IDLE`. No actuators energised, no LEDs lit.
- `INIT` → operating; the board accepts `ROLE_ATTACH` and starts the
  keepalive watchdog.
- `SHUTDOWN` or master-loss timeout → safe-state → back to `IDLE`.
- `IDENTIFY` returns the same `INIT_READY` payload but does **not**
  transition state — used by the master's `ExpanderService` for safe type
  detection at enumerate-time before deciding whether to INIT.

The `INIT_READY` / `IDENTIFY` payload is length-prefixed:

```
[nameLen:u8][name][verLen:u8][version][platLen:u8][platform]
[cpuMHz:u32LE][freeRam:u32LE][buildNum:u32LE][capabilities:u32LE]
```

`capabilities` is a [Rule 11](../.github/copilot-instructions.md) append-only
bitmask, computed at compile time by `BoardServer<...>` as the OR of every
policy's `kCapabilityBits`. The board GUID is the `"<Prefix>-<4 hex>"` suffix
of the device name (see CLAUDE.md "Board GUID") — that suffix is how the hub
addresses this board's ports.

### 3.2 STATUS

`STATUS_REQ` → `STATUS`: a 22-byte core header
(`counter, uptime, freeRam, lastActivity_ms, keepaliveCount, boardState,
initFlags`) followed by any bytes a board appends via `onStatusData(cb)`.
LightFX uses this to ride a battery section (voltage / cells / low+critical
flags) so the hub can read the expander's pack — see `appendBatteryStatus`
in the LightFX sketch.

### 3.3 Reset / bootloader

`REBOOT` (software reset, used by `scalefx-flash` to reflash without BOOTSEL)
and `BOOTSEL` (enter the Pico ROM bootloader / ESP32 download mode) ship as
defaults in `BoardServicePolicy`. Boards rarely override them. `scalefx-flash`
sends `BOOTSEL` over the active CDC link then waits for the RPI-RP2 drive —
**expanders MUST keep this command working** or every flash needs the physical
button.

### 3.4 I²C scan / diagnostics

`board.enableI2CScan(...)` + `board.addExpectedI2CDevice(...)` wires the
`I2C_SCAN` surface (which expected addresses + a live bus probe). The
`DiagLog` singleton buffers recent log lines and streams them on the wire
(`DiagLog::instance().setWireMinLevel(DiagLevel::INFO)` in the sketch) so the
CLI / Studio console see the boot + role-attach + seek/stall trace. Use
`SFX_LOG_INFO/WARN/ERROR(...)`; avoid per-loop `debug` calls.

---

## 4 · Ports — the board's compile-time identity

A port is a typed, fixed-direction output (or input) slot. The board declares
its ports as `static constexpr` descriptor lists; `BoardOf<>`'s
`PortServicePolicy` enumerates them on `PORT_LIST_REQ` and `PortServicePolicy`
+ `RoleServicePolicy` together bind them into a `PortRegistry` sized by
`PortCapacity<servo, pwm, hbridge, input>`.

### 4.1 Port kinds + their native classes

| Kind | Native port class | Descriptor helper | Drives |
|---|---|---|---|
| Servo | `MicroservoPort{gpio}` | `ports::servo_array<&Board::member, N>()` | hobby servo PWM (1000–2000 µs) |
| Pwm | `NativePwmPort{gpio}` | `ports::pwm_array<&Board::member, N>()` | LED / generic / heater PWM |
| HBridge | `DualPwmHBridgePort{fwdPwm, revPwm}` | `ports::hbridge_array<&Board::member, N>()` | bi-directional DC motor (signed duty) |
| Input | `InputPort{gpio}` | `ports::input_array<&Board::member, N>()` | RC: PPM / SBUS / Jeti EX / CRSF (HubFX-only) |

Each descriptor list is built with `sfx_core::ports::list(...)`:

```cpp
static constexpr auto kServoPorts = sfx_core::ports::list(
    sfx_core::ports::servo_array<&MyBoard::servoOut, 3>());

static constexpr auto kHBridgePorts = sfx_core::ports::list(
    sfx_core::ports::hbridge_array<&MyBoard::motor, 3>()
        .with_iSense_array<&MyBoard::iSense>()    // INA226 current → stall detect
        .with_vSense_array<&MyBoard::vSense>());   // INA226 voltage → reported
```

### 4.2 Port metadata — voltage rails

Every output descriptor carries an optional `voltageMv`
([Rule 37](../.github/copilot-instructions.md)) set with
`.with_voltage_mV<N>()` — the rail the port is wired to. It flows through
`PORT_LIST_RESP` → Go `ports.PortDescriptor.VoltageMv` → Studio's port-picker
labels, and lets sub-rail effects scale duty (a 5 V heater element on an 8 V
rail). `0` = unknown (no label, no scaling).

### 4.3 `PortCapacity<>` sizes the registry

`PortCapacity</*servo*/N, /*pwm*/M, /*hbridge*/K, /*input*/J>` is a template
parameter on `BoardOf<>` that sizes the `PortRegistry`'s slot arrays exactly
to this board's hardware — no over-allocation. It MUST match the totals across
the `kXxxPorts` descriptors (GearControl: `<7, 0, 3, 0>` for 7 servo + 3
hbridge).

---

## 5 · Roles — attached at runtime by the hub

The board declares ports; the **hub attaches roles** onto them. A role is the
[Rule 42](../.github/copilot-instructions.md) actuator-mechanism layer — it
owns the motion profile / stall guard / element-voltage scaling and exposes an
INTENT surface (`setTarget(us)`, `setPct`, `setNormalizedTarget`). Roles
emplace into a `std::variant` slot in the `PortRegistry`.

### 5.1 Role attach surface (`RoleServicePolicy`)

| Packet | Direction | Meaning |
|---|---|---|
| `ROLE_ATTACH` (`TOPOLOGY_ROLE_ATTACH`) | hub → expander | attach ONE role onto a port; ACK'd. Used for live Studio edits. |
| `ROLE_BULK_ATTACH` (0x57) | hub → expander | attach the board's FULL role set in one declarative packet; ONE ACK. Used at bringup. |

The hub pushes roles **declaratively at bringup**: when an expander reaches
`Ready`, `ExpanderService::onReady(...)` builds the board's full role set and
sends it in ONE `ROLE_BULK_ATTACH` (the standard pattern — see CLAUDE.md
"Config aliases for expander ports"; the old per-port forward loop raced on
reconnect and never updated the hub's cached roster). `RoleServicePolicy::
handleBulkAttach` loops the same `applyAttach` the single path uses and sends
one ACK; an empty block (count=0) is a valid no-op for an unconfigured board.

The expander firmware writes **none** of this — `RoleServicePolicy` (auto via
`BoardOf<>`) handles attach + drive + query + telemetry generically. The
sketch's job ends at declaring ports.

### 5.2 Role families (shipping)

| Role | Attaches onto | Owns |
|---|---|---|
| `ServoActuator` | Servo port | `MotionProfile1D` (trapezoidal + optional S-curve), calibrated `[min,max]` µs, REV flag; intent via `SERVO_SET_POS_NORM` (normalized `[0..10000]`) |
| `LedAnimator` | Pwm port | LED program tick (ON/OFF/flash/fade/beacon), phase-locked to `EffectClock` |
| `BiDcMotor` | HBridge port | signed-duty drive, endstop seek, stall guard (LiveRatio + ceiling from INA226 current) |
| `Heater` / `DcMotor` | Pwm port | element-voltage scaling (`scaleDuty`), bang-bang / duty |

To expose a NEW role family, add its codec (`protocol/roles` + a firmware role
handler) + ONE line in `roleKindFor<>()` in
[`role_registry.h`](../controllers/lib/sfx_board/server/role_registry.h) + a
one-line `RoleTarget` wrapper on the Go side — **nothing else**
([Rule 58](../.github/copilot-instructions.md)). Never add a per-role `switch`
to the hub's TopologyService, the forward/query/event transport, or the Go
event dispatch.

### 5.3 How the hub drives an attached role

The hub addresses a role by `PortRef{guid, kind, idx}` (guid `""` → hub-local;
else the expander GUID) and uses three role-AGNOSTIC wire primitives that
forward the inner role packet as **opaque bytes** (the hub never decodes it):

- **command** — `TOPOLOGY_ROLE_FORWARD` (0x8F, ACK/NACK);
- **query** — `TOPOLOGY_ROLE_QUERY` → `RESPONSE` (0xA6/0xA7);
- **telemetry** — `TOPOLOGY_ROLE_EVENT` (0x8E, GUID-tagged async; re-emit
  gated on `hostVerboseActive()` so a PC-less hub doesn't flood the wire).

On the Go side this is `client.RoleTarget` (`client.Role(guid)`) — ONE role-I/O
path for local and remote. See [`32-ARCHITECTURE-DIAGRAMS.md`](32-ARCHITECTURE-DIAGRAMS.md)
§3–4 for the full diagram.

---

## 6 · Hardware safety — safe-state

Triggered by: keepalive watchdog timeout (while attached), an explicit
`SHUTDOWN`, or a re-INIT (master rejoin parks first). Each attached role parks
to its safe state — servos ramp to their safe pulse, motors coast/brake, LED
animators blank. The `BiDcMotor` role additionally latches its seek timeout,
which the GearControl sketch reads back from the registry to blink the local
status LEDs (a board-local concern, not driven by the hub).

DIRECT/CLI sessions do not enforce keepalive — a CLI can pause seconds between
commands. A board needing hard fail-safe under a CLI session layers its own
watchdog.

---

## 7 · Local hardware vs hub-driven ports

Two clearly separate concerns live in an expander sketch:

- **Hub-driven ports** — the `kXxxPorts` descriptors. The hub attaches roles
  and drives them. Energised only after `ROLE_ATTACH`.
- **Board-local hardware** — indicator LEDs (the blue/error pair via
  `IndicatorServicePolicy`), per-motor status LEDs, I²C sensors. Driven
  **locally** in the sketch, never commanded by the hub. GearControl's
  `GearStatusLeds` (CW/CCW direction blink off the H-bridge `signedDuty()`)
  and its 3× INA226 bus are the canonical example.

Keep them separate: a status LED that reflects local drive state is NOT a
`Pwm` port (it's not hub-addressable); an INA226 wired to a port's
`.with_iSense_array<>()` feeds the role's stall guard (it IS part of the port).

---

## 8 · Persistence

An expander is **stateless from the master's view** apart from its
hardware-derived GUID. There is no `/board.yaml` identifier file, no
`ConfigServicePolicy`, no general file-system surface (`FILE_*`, SD, dir ops)
— those are HubFX-only. All board configuration (which roles attach to which
ports, servo calibration windows, motor guard thresholds) is **pushed by the
hub** from `/hubfx.yaml`'s `expanders:` block at bringup via
`ROLE_BULK_ATTACH`. Friendly per-board aliases (alias → GUID) live in
`/hubfx.yaml`, never on the board.

Expanders DO NOT advertise `SD`, `AUDIO`, `USB_HOST`, `ENGINE`, or `CONFIG`
capabilities. A battery-equipped board MAY advertise `BATTERY` (LightFX does,
via `BatteryServicePolicy<AdcDividerBatteryT<…>>`). Studio's file-manager /
config paths gate on these bits and skip expanders — don't OR in capabilities
the board doesn't serve.

---

## 9 · Battery monitoring — an ExtraPolicy

A board with an onboard pack sensor adds `BatteryServicePolicy<TBattery>` as
an ExtraPolicy on its `BoardOf<>` pack and binds the sensor before
`board.begin()`. LightFX:

```cpp
using LightFxBattery        = AdcDividerBatteryT<6180>;          // ÷6.18 divider, milli-units
using LightFxBatteryService = BatteryServicePolicy<LightFxBattery>;

class LightFxBoard : public sfx_core::BoardOf<LightFxBoard, LightWireStream,
                                              sfx_core::PortCapacity<3, 8, 0, 0>,
                                              LightFxBatteryService> {
    LightFxBattery battery;   // ADC + divider on GP29
    // …ports…
};

void setup() {
    board.battery.begin(Gpio::VSENSE);                              // default LiPo, auto cell-detect
    board.policy<LightFxBatteryService>().bindBattery(board.battery);
    board.begin(wireStream, FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);
    board.core().onStatusData(appendBatteryStatus);                 // ride STATUS broadcast
}

void loop() {
    board.process();
    board.battery.update();   // ADC read + state machine (throttled)
    busy_wait_ms(1);
}
```

`BatteryServicePolicy` advertises `CoreCapability::BATTERY`, handles the
chemistry / cell-count config from the hub, and the board appends a battery
section to STATUS so the hub's `ExpanderService` can poll voltage and surface
it in `system-info`. Backends satisfy the `BatteryPolicy` concept:
`AdcDividerBatteryT<MultiplierMilli>` (cheap resistor divider into an ADC),
`Ina226Battery` (INA226 on a power-diag bus), or no policy at all.

---

## 10 · Build setup (PlatformIO, Pico)

```ini
[env:pico]
platform  = https://github.com/maxgerhardt/platform-raspberrypi.git
board     = pico
framework = arduino
build_flags =
    -std=gnu++20
    -DBUILD_NUMBER=1
    -DFW_VERSION=\"1.0.0\"
lib_deps =
    sfx_platform     ; cross-platform macros + PicoSerialStream wire adapter
    sfx_serial       ; protocol headers (core, ports, roles) + DiagLog
    sfx_board        ; BoardOf<> / PortServicePolicy / RoleServicePolicy / roles
    sfx_peripherals  ; native port classes (servo/pwm/hbridge) + I²C + INA226
    ; sfx_storage    ; ONLY if a board needs FlashModule (most don't)
```

- `-std=gnu++20` is non-negotiable ([Rule 18](../.github/copilot-instructions.md)).
- `BUILD_NUMBER` is auto-incremented by `scalefx-flash` on every flash; the
  firmware echoes it in `INIT_READY`.
- `FW_VERSION` follows semver — MAJOR for wire-breaking, MINOR for additive
  ([Rule 11](../.github/copilot-instructions.md)), PATCH for logic-only.

Forbidden on an expander: `sfx_audio`, `sfx_usb`, `sfx_config` — no use case.

Build / flash via the Flash CLI:

```bash
scalefx-flash build lightfx --no-clean
scalefx-flash flash lightfx --port COM10
```

---

## 11 · Recipe — building a new expander board

1. **Inventory the physical I/O.** Servos, motors (H-bridges), LED channels,
   any input. Map each to a port kind (Rule 31 fixes direction at declaration).
   Any HIGH-LEVEL behaviour (sequences, choreography, RC logic) is NOT yours —
   it goes on the hub as an effect-service policy that drives your roles by
   `PortRef`.
2. **Pick the role per port.** Servo → `ServoActuator`; LED PWM →
   `LedAnimator`; motor → `BiDcMotor`; heater → `Heater`. If you need a role
   family that doesn't exist, see §5.2 (codec + one `roleKindFor<>` line +
   one `RoleTarget` wrapper).
3. **Write the board class.** Derive `BoardOf<TBoard, TStream,
   PortCapacity<...>, ...ExtraPolicies>`; declare the concrete port member
   arrays; declare the static `kServoPorts` / `kPwmPorts` / `kHBridgePorts` /
   `kInputPorts` descriptors; set `kName`. Size `PortCapacity<>` to match.
4. **Bring up local hardware** before `board.begin()` (I²C sensors, ADC
   battery) so the registry sees it live when it walks each port's `begin()`.
   Wire any board-local indicator/status LEDs as a LOCAL concern.
5. **`setup()` = `Serial.begin` + (optional hardware) + `board.begin(wire,
   ver, build, ledPin, errPin)`** + `DiagLog::setWireMinLevel(INFO)`.
6. **`loop()` = `board.process()` + per-loop maintenance (sensor poll,
   battery update, local LEDs) + a 1 ms yield.** No high-level logic.
7. **Promote reusable drivers into `sfx_peripherals/`** rather than inlining
   hardware I/O ([Rule 7](../.github/copilot-instructions.md)).
8. **Wire the hub side.** Add the board's `expanders:` alias + per-port roles
   to `/hubfx.yaml`; the hub `ROLE_BULK_ATTACH`es at bringup. Drive + verify
   from the CLI (`scalefx-cli`, role drive commands take a trailing
   `guid=XXXX`) — see CLAUDE.md "Config aliases for expander ports".
9. **Run the test harness** under [`tests/`](../tests/) before merging.

---

## 12 · Things to avoid

- **No high-level state machines on an expander.** A "trigger sequence" or
  "phase enum" in expander code is a smell — move it to a HubFX effect-service
  policy that drives your roles by `PortRef`.
- **No board-specific wire packets.** If a feature can't be expressed through
  ports + roles, add a role codec (§5.2), not a per-board header.
- **No `STANDALONE` / autonomous mode.** Boot quietly into `IDLE`; energise a
  port only after the hub attaches a role.
- **No Arduino hardware I/O in `controllers/lib/`** ([Rule 55](../.github/copilot-instructions.md)) —
  `SfxI2cBus`, native port classes, `sfx::Stream`. `<Arduino.h>` is the
  sketch's privilege only.
- **No raw `delay()` / `sleep_ms()` / `delayMicroseconds()`** — use
  `SFX_DELAY_MS` if you genuinely must block; prefer non-blocking polling.
- **No `volatile` for cross-core variables** ([Rule 15](../.github/copilot-instructions.md)) —
  `std::atomic<T>` with explicit memory order.
- **No raw `millis()` in a role's motion/sync timing** ([Rule 40](../.github/copilot-instructions.md)) —
  `EffectClock` (the framework latches it in `process()`).
- **No file-system commands on the expander wire** (§8) — config comes from
  the hub at bringup.
- **No virtual interfaces inside the expander runtime** — concepts +
  `requires`, not polymorphism.
- **No emojis in code or comments. No marketing prose.**

---

## 13 · Cross-references

- [`controllers/lightfx/pico/src/lightfx_pico.ino`](../controllers/lightfx/pico/src/lightfx_pico.ino)
  + [`controllers/gearcontrol/pico/src/gearcontrol_pico.ino`](../controllers/gearcontrol/pico/src/gearcontrol_pico.ino)
  — the two shipping worked examples.
- [`controllers/lib/sfx_board/server/board_of.h`](../controllers/lib/sfx_board/server/) —
  `BoardOf<>` / `PortCapacity<>` / `PortServicePolicy` / `RoleServicePolicy`.
- [`controllers/lib/sfx_board/server/role_registry.h`](../controllers/lib/sfx_board/server/role_registry.h)
  — the single role-class ⇄ `RoleKind` map (`roleKindFor<>` / `forEachAttachedRole`).
- [`controllers/lib/sfx_board/roles/`](../controllers/lib/sfx_board/roles/) —
  `ServoActuatorRole`, `LedAnimator`, `BiDcMotorRole`, `HeaterRole`, …
- [`controllers/lib/sfx_peripherals/ports/`](../controllers/lib/sfx_peripherals/) —
  native port classes (`MicroservoPort`, `NativePwmPort`, `DualPwmHBridgePort`, `InputPort`).
- [`app/go/client/roletarget.go`](../app/go/client/roletarget.go) — the Go
  `RoleTarget` (one role-I/O path).
- [`17-SYSTEM-SERVICES.md`](17-SYSTEM-SERVICES.md) — the policy framework + RFC history.
- [`31-GUID-PORT-ROUTING.md`](31-GUID-PORT-ROUTING.md) — how the hub addresses a remote port.
- [`32-ARCHITECTURE-DIAGRAMS.md`](32-ARCHITECTURE-DIAGRAMS.md) §3–4 — port/role data-flow diagrams.
- [`15-GENERIC-EXPANDER-REFACTOR.md`](15-GENERIC-EXPANDER-REFACTOR.md) — the pivot's historical planning doc.
- [`01-ARCHITECTURE.md`](01-ARCHITECTURE.md) — system-wide context.
