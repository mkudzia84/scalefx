# Creating a New Controller / Service Policy

> **ACTION DOCUMENT:** How to add a new board (Pico expander) or a new protocol-exposed subsystem (`*ServicePolicy`) to an existing board.

There are two distinct "new things" you might be creating. Pick the right one:

| Goal | What you build | Read |
|------|----------------|------|
| A new **physical expander board** (Pico) that exposes ports the hub drives | A board class via `BoardOf<…>` + a thin `*_pico.ino` sketch | This doc, § "New expander board" |
| A new **effect / subsystem on the hub** (gun, gear, landing, audio, …) | A `*ServicePolicy` satisfying `SystemServicePolicy`, composed into the hub's `BoardServer<…>` | This doc, § "New service policy" + [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) |

> **There are no more standalone effect boards.** GunFX / LightFX effect logic lives on the hub. A Pico board is a *thin port + role expander* — it exposes hardware and lets the hub attach roles. Live examples: [`controllers/lightfx/pico/src/lightfx_pico.ino`](../controllers/lightfx/pico/src/lightfx_pico.ino), [`controllers/gearcontrol/pico/src/gearcontrol_pico.ino`](../controllers/gearcontrol/pico/src/gearcontrol_pico.ino).

---

## Framework Overview

Every board is composed from `sfx_core::BoardServer<TStream, ...UserPolicies>` (`controllers/lib/sfx_board/server/board_server.h`). Each protocol-exposed subsystem is a `*ServicePolicy` satisfying the `sfx_core::SystemServicePolicy` concept:

```cpp
struct MyServicePolicy {
    static constexpr uint32_t kCapabilityBits = CoreCapability::SOMETHING;  // OR'd into IDENTIFY caps
    bool begin(sfx_core::BoardServerBase* ctx);
    bool ownsType(uint8_t type) const;                        // claim packet bytes
    sfx_core::CommandHandleResult handle(uint8_t type, const uint8_t* p, size_t len);
    void update();                                            // ticked from loop()
};
```

`BoardServer` reads COBS frames, verifies CRC-8, and offers each packet to every policy's `ownsType()` — **first owner wins, no per-board ranges**. `BoardServicePolicy` (INIT/STATUS/IDENTIFY lifecycle) and `IndicatorServicePolicy` (status LEDs) are **prepended automatically** — never list them.

For boards with physical ports, declare via `sfx_core::BoardOf<TBoard, TStream, PortCapacity<NServo,NPwm,NHBridge,NInput>, ...ExtraPolicies>` (`board_of.h`), which additionally auto-prepends `PortServicePolicy` + `RoleServicePolicy`:

```
BoardServicePolicy → IndicatorServicePolicy → PortServicePolicy → RoleServicePolicy → …ExtraPolicies…
```

---

# Part A — New expander board (Pico)

## Step 1: Directory structure

```bash
mkdir -p controllers/newfx/pico/src
```

```
controllers/newfx/pico/
├── src/newfx_pico.ino
├── platformio.ini
└── README.md
```

## Step 2: platformio.ini

Copy [`controllers/lightfx/pico/platformio.ini`](../controllers/lightfx/pico/platformio.ini) — Arduino-Pico framework, `monitor_speed = 6000000`, and the `lib_deps` pointing at the `sfx_*` library roots. The board sketch is the ONLY Arduino-using layer (Rule 55); shared `lib/` code stays Arduino-free.

## Step 3: The board class + sketch

A board lists its drivers as members and its ports as static `kServoPorts` / `kPwmPorts` / `kHBridgePorts` / `kInputPorts` descriptor tuples. `BoardOf<>` derives the registry size, binds the descriptors, and wires every policy. `PortCapacity<>` sizes the registry **exactly** to the board's hardware (over-generous caps waste DRAM — each slot embeds a role `std::variant`).

This is the live LightFX shape (a USB-CDC wire stream + an ExtraPolicy battery monitor):

```cpp
#include <Arduino.h>
#include <type_traits>
#include <platform/sfx_platform.h>
#include <platform/pico_serial_stream.h>   // USB-CDC Serial → sfx::Stream adapter (Rule 55)
#include <serial/diag_log.h>
#include <server/board_of.h>
#include <ports/pwm_port.h>
#include <ports/servo_port.h>
#include <power/battery_monitor.h>          // AdcDividerBatteryT
#include <power/battery_server.h>           // BatteryServicePolicy

#define FIRMWARE_VERSION "1.0.0"
#define BUILD_NUMBER     1

namespace Gpio { constexpr int LED_CONNECTION = 24, LED_ERROR = 25, VSENSE = 29; }

using NewWireStream     = sfx::PicoSerialStream<std::remove_reference_t<decltype(Serial)>>;
using NewFxBattery      = AdcDividerBatteryT<6180>;                 // ADC + ÷6.18 divider
using NewFxBatteryService = BatteryServicePolicy<NewFxBattery>;

class NewFxBoard : public sfx_core::BoardOf<NewFxBoard,
                                            NewWireStream,
                                            sfx_core::PortCapacity</*servo*/3, /*pwm*/8,
                                                                    /*hbridge*/0, /*input*/0>,
                                            NewFxBatteryService> {     // ← ExtraPolicy
public:
    sfx_peripherals::NativePwmPort  led[8]      = {{0},{1},{2},{3},{4},{5},{6},{7}};
    sfx_peripherals::MicroservoPort servoOut[3] = {{8},{9},{10}};
    NewFxBattery                    battery;

    static constexpr auto kPwmPorts   = sfx_core::ports::list(
        sfx_core::ports::pwm_array<&NewFxBoard::led, 8>());
    static constexpr auto kServoPorts = sfx_core::ports::list(
        sfx_core::ports::servo_array<&NewFxBoard::servoOut, 3>());

    static constexpr const char* kName = "NewFx";
};

NewFxBoard    board;
NewWireStream wireStream{Serial};

void setup() {
    Serial.begin(115200);                                   // baud ignored over USB CDC

    // Bind external dependencies BEFORE board.begin() so the policy's
    // begin() sees them (and the capability bit is live).
    board.battery.begin(Gpio::VSENSE);
    board.policy<NewFxBatteryService>().bindBattery(board.battery);

    // Wire / DiagLog / indicator pins / port registry / every policy begin()
    // / IDENTIFY capabilities — one call.
    board.begin(wireStream, FIRMWARE_VERSION, BUILD_NUMBER,
                Gpio::LED_CONNECTION, Gpio::LED_ERROR);

    // Optional: append module data to the periodic STATUS broadcast.
    board.core().onStatusData(appendBatteryStatus);

    DiagLog::instance().setWireMinLevel(DiagLevel::INFO);   // stream boot/attach trace
}

void loop() {
    board.process();        // frame read + dispatch + EffectClock latch + policy update()
    board.battery.update();
    busy_wait_ms(1);
}
```

> **Roles are NOT attached in the sketch.** `RoleServicePolicy` (auto via `BoardOf`) accepts `ROLE_ATTACH` / `ROLE_BULK_ATTACH` from the hub at runtime and emplaces the `LedAnimator` / `ServoActuator` / `BiDcMotor` variants. Port direction is fixed at declaration (Rule 31).

### setup() sequence (the canonical order)

| Step | What |
|------|------|
| 1 | `Serial.begin()` — bring the USB-CDC endpoint up before the framer reads it |
| 2 | Hardware init that must precede policy `begin()` (sensors, I²C) + `board.policy<P>().bindXxx(...)` for policies needing external deps |
| 3 | `board.begin(wireStream, ver, build, connPin, errPin)` — wires everything |
| 4 | `board.core().onStatusData(cb)` (optional module STATUS tail) |
| 5 | `DiagLog::instance().setWireMinLevel(...)` (optional log streaming) |

`loop()` is `board.process()` + any per-frame driver updates + a 1ms yield.

## Step 4: Capabilities & detection

The board's IDENTIFY capability word is the OR of every policy's `kCapabilityBits` plus the port-presence bits `BoardOf` adds. The Go side detects the controller from the IDENTIFY/INIT name; surface a controller-type constant in `app/go/protocol/core/core.go` and (if the board has CLI-visible commands) gate them via `RequiresCap` in `app/go/console/`.

## Step 5: README

Create `controllers/newfx/pico/README.md` documenting the pinout, the ports exposed, and which roles the hub attaches.

---

# Part B — New service policy (hub subsystem)

To add an effect/subsystem on the hub:

1. **Define the policy.** Create `controllers/hubfx/esp32s3/src/effects/<name>/<name>_service.h` (effect) or a `lib/` policy. Implement the `SystemServicePolicy` surface; claim your packet bytes in `ownsType()` against the dispatch map in `CLAUDE.md` (append at the next truly-free value — grep the real `ownsType()` predicates first, the map drifts).

2. **Define the wire surface.** Packet types + error codes go in a `<name>_protocol.h` next to the policy. Mirror them in `app/go/protocol/<name>/<name>.go` (the **source of truth** for the master protocol — the firmware header is no longer mirrored anywhere else).

3. **Compose it.** Add the policy type to the hub's `BoardServer<…>` pack (`controllers/hubfx/esp32s3/src/hubfx_esp32s3.cpp`). Bind external deps in `setup()` via `board.policy<MyPolicy>().bindXxx(...)`.

4. **Go side.** Add a typed wrapper in `app/go/client/<name>.go` (`sendExpectACK` / `sendForResp`) and a self-registering CLI command file `app/go/console/cmd_<name>.go` (see [07-CLI-UPDATES.md](07-CLI-UPDATES.md)).

Full worked example: [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md).

---

## Validation Checklist

```yaml
After_Completion:
  Build:
    - [ ] "scalefx-flash build newfx --no-clean" succeeds (new Pico board)
    - [ ] "scalefx-flash build hubfx --no-clean" succeeds (new hub policy)
    - [ ] All existing controllers still build (lightfx, gearcontrol, hubfx)
    - [ ] "cd app/go && go build ./..." succeeds (protocol mirror sync check)
  Runtime:
    - [ ] CLI/Studio detects the board on connect (IDENTIFY name → controller type)
    - [ ] IDENTIFY advertises the expected capability bits
    - [ ] Hub attaches roles to the new board's ports (topo-roles / Studio IO tab)
  Documentation:
    - [ ] README.md created (board) or dispatch map updated in CLAUDE.md (policy)
```

---

## Component Libraries (`controllers/lib/`)

**Always check here before writing hardware-specific code** (Rule 7). New I2C drivers extend `I2CDeviceT<>`; new hardware adds a native `{esp,pico}_*` pair (Rule 55). Highlights:

| Component | Header | Purpose |
|-----------|--------|---------|
| `BoardServer<…>` | `server/board_server.h` | The board composer (REQUIRED) |
| `BoardOf<…>` | `server/board_of.h` | Port-aware board shorthand |
| `PortServicePolicy` / `RoleServicePolicy` | `server/port_service.h` / `role_service.h` | Port registry + role attach/drive |
| `NativePwmPort` / `MicroservoPort` / `HBridgePort` | `ports/*.h` | Output port kinds |
| `AdcDividerBatteryT<Multiplier>` | `power/battery_monitor.h` | ADC battery voltage (templated divider) |
| `BatteryServicePolicy<TBattery>` | `power/battery_server.h` | BATTERY_CONFIG (0xEE) handler |
| `INA226` / `I2CDeviceT<>` | `i2c/*.h`, `power/ina226.h` | I²C current/voltage monitor + base |
| `EffectClock` | `server/effect_clock.h` | Latched effect-layer clock (Rule 40) |
| `DiagLog` | `serial/diag_log.h` | Diagnostic logging over the wire |
