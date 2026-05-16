# On-board Co-processor Slave (UART-attached)

> **TL;DR.** When you need to add slave functionality (PWM expansion, extra
> GPIO, sensor frontend, motor control) **on the same PCB** as the HubFX
> master rather than as a separate USB-attached board, use an **ESP32-C3
> over UART** with **master-bridged flashing**. This document captures the
> architectural decision, the wiring, the flash workflow, and the protocol
> allocation so the pattern can be recreated without re-deriving it.

This complements [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md), which covers
the standard pattern: slave Pico → USB-A jack → ESP32-S3 USB-OTG host.
That pattern stays the canonical one for **detachable** slaves (LightFX,
GunFX, GearControl). The pattern below is for **fixed** co-processors
that ride on the master's PCB.

---

## When to use this pattern

| | Detachable USB slave (existing) | On-board UART co-processor (this doc) |
|---|---|---|
| Physical attachment | Separate PCB, USB-A cable | Same PCB, on-board UART |
| Use cases | LightFX, GunFX, GearControl | PWM expander, sensor frontend, motor co-controller |
| Cabling | USB hub between master and slave | 5 traces, ~50 mm typical |
| Hot-swap | yes | no — soldered |
| Flashing | unplug from hub, plug to PC | master acts as passthrough bridge |
| Bus | USB CDC at 6 Mbps | UART at 1–2 Mbps (typical) |
| Protocol overhead | TinyUSB + CDC framing | bare COBS over UART |
| Master-side enumeration | dynamic via `UsbRegistry` | static — registered at boot |

Pick the on-board pattern when the slave's role is intrinsic to the
master's product (e.g., the HubFX always needs more PWM channels) and
the cost / complexity / failure surface of a separate USB-cabled board
is not justified.

---

## Bus choice — UART, not I²C

ScaleFX's protocol is byte-stream / COBS-framed: `BusServer` /
`BusClient` are designed for streaming transport. UART is a 1:1 fit;
nothing in the existing slaves changes. I²C is a transactional
master-polled bus and would force re-engineering the entire
server/client framework around polled transactions.

| | UART (TX/RX, 2 wires) | I²C (SDA/SCL, 2 wires) |
|---|---|---|
| Maps to existing `BusServer` | yes, drop-in | no — needs polled-transaction wrapper |
| Async events (slave → master) | native, anytime | requires INT pin + master polling |
| Bandwidth | 1–5 Mbps trivially | 400 kHz → ~30 KB/s effective |
| Software complexity | minimal | clock stretching, NACKs, multi-master arbitration |
| Master-bridged flashing | trivial via ROM bootloader | impossible (no chip with I²C bootloader) |

**Decision: always UART for ScaleFX co-processors on the same PCB.** Keep
I²C for what it's good at (peripheral chips like AW9523B, INA226,
TAS5825) — not for inter-MCU protocol traffic.

---

## MCU choice — ESP32-C3

For ScaleFX-on-same-PCB co-processors, **ESP32-C3** is the recommended
slave silicon.

| | ESP32-C3 ⭐ | RP2040 | STM32G031 |
|---|---|---|---|
| Same toolchain as ESP32-S3 master | **yes** (ESP-IDF / Arduino-ESP) | partial (Arduino-Pico) | new (STM32 HAL/CubeMX) |
| Hardware PWM channels | 6 LEDC + 6 MCPWM = **12** | **16** | ~16 |
| PWM resolution | 14-bit | 16-bit | 16-bit |
| ROM-resident UART bootloader | **yes** (esptool — same as S3) | **no** (PICOBOOT is USB-only) | yes (DFU + UART) |
| Master-bridged flash trivial? | **yes** | needs custom UART loader app | works via STM32 ROM |
| RAM / Flash | 400 KB / 4 MB | 264 KB / external flash | 8 KB / 32 KB (cheap variant) |
| Cost | ~$1 | ~$1 | ~$0.60 |
| Existing ScaleFX library port | needs port (small) | already in tree | needs port (significant) |

**ESP32-C3 wins on the master-bridge flash story**, which is the whole
reason to put a slave next to the master rather than as a USB device.
RP2040 has more PWM channels but its boot ROM only does USB
(PICOBOOT) — flashing it through a master means writing a custom UART
bootloader application, defeating the simplicity goal.

**Use RP2040 instead** only if:
- You need >12 hardware PWM channels in one slave, AND
- You can add an SWD header for in-circuit programming with a
  picoprobe (i.e., master-bridged flashing is not a hard requirement).

---

## PCB topology — five wires

```
   ESP32-S3 (master)                ESP32-C3 (slave)

   any GPIO  ───── slave_RST  ◄── chip RESET (active low)
   any GPIO  ───── slave_BOOT ◄── chip GPIO9 (ROM strap: LOW = UART download)
   UART1_TX  ───── UART_RX
   UART1_RX  ───── UART_TX
   GND       ───── GND
   3V3       ───── VDD                              (or its own LDO)
```

GPIO selection on the master:

- `slave_RST`, `slave_BOOT` — pick any two free ESP32-S3 GPIOs (per
  [HubFX free-GPIO inventory](../controllers/hubfx/esp32s3/README.md)
  the available ones are 14, 15, 16, 17, 18, 21, 46, 47).
- `UART1_TX` / `UART1_RX` — ESP32-S3 UART1 (UART0 is reserved for the
  USB-UART bridge to the PC). Any two of the free GPIOs above can be
  matrix-routed to UART1 via the GPIO-MUX; no fixed pin requirement.
- Pull-ups: `slave_RST` and `slave_BOOT` should have weak external
  pull-ups (10 kΩ to 3V3) so the C3 boots normally on cold start when
  the master GPIOs are still high-impedance.

ESP32-C3 strap pin notes (datasheet §3):

- **GPIO9** is the BOOT strap. LOW at reset → UART/USB bootloader.
  HIGH (default via internal weak pull-up + external 10 kΩ) → SPI
  flash boot.
- GPIO2, GPIO8 are also straps; leave them at their default (pull-up
  for GPIO2, no pull-down for GPIO8).
- Do NOT use GPIO12/13 for anything — they are USB D±. Even if you're
  not exposing the C3's USB on a connector, leave them un-routed.

---

## Flashing — master as a UART passthrough for esptool

The C3's boot ROM implements the same UART download protocol the master
itself uses (esptool talks to it). The master temporarily becomes a
transparent UART bridge while holding the C3 in download mode.

### Sequence

1. PC sends a `SLAVE_FLASH_BEGIN` ScaleFX command to the master.
2. Master enters **passthrough mode**:
   - Drives `slave_BOOT` LOW.
   - Toggles `slave_RST` LOW → HIGH (slave reboots into ROM bootloader).
   - Configures UART1 to bridge bytes between the PC's USB-UART and the
     slave's UART. ~10 lines of `loop()`-tight forwarder code.
3. PC runs `esptool.py --port <master-COM> --before no_reset --after no_reset write_flash 0x0 firmware.bin`.
   esptool doesn't know it's not talking to the chip directly — same
   protocol bytes flow through.
4. Master detects `SLAVE_FLASH_END` from the PC (or a timeout), drops
   `slave_BOOT` HIGH, pulses `slave_RST`. Slave boots the new firmware.
5. Master returns to ScaleFX-protocol mode on UART1.

### Master passthrough mode — code sketch

```cpp
// Entered via SLAVE_FLASH_BEGIN ScaleFX command from the PC.
void slavePassthroughLoop() {
    digitalWrite(SLAVE_BOOT, LOW);                 // strap for UART download
    digitalWrite(SLAVE_RST,  LOW); delay(10);
    digitalWrite(SLAVE_RST,  HIGH); delay(20);     // slave is now in ROM bootloader

    // Match the slave's bootloader baud (115200 default; auto-detects others)
    Serial1.begin(115200, SERIAL_8N1, SLAVE_RX_PIN, SLAVE_TX_PIN);

    // Tight bidirectional forwarder. ~3 ms latency end-to-end.
    while (!flashEndRequested) {
        while (Serial.available())  Serial1.write(Serial.read());
        while (Serial1.available()) Serial.write(Serial1.read());
    }

    digitalWrite(SLAVE_BOOT, HIGH);
    digitalWrite(SLAVE_RST,  LOW); delay(10);
    digitalWrite(SLAVE_RST,  HIGH);
    Serial1.updateBaudRate(SCALEFX_BUS_BAUD);      // back to protocol mode
}
```

### `scalefx-flash` integration

Wrap the workflow in a new flash CLI subcommand:

```bash
scalefx-flash flash <coproc>          # e.g. "pwmfx"
```

The CLI:

1. Opens the master's COM port.
2. Sends `SLAVE_FLASH_BEGIN` via the ScaleFX protocol.
3. Closes the protocol connection (master is now in raw bridge mode).
4. Re-opens the same COM port at the bootloader baud (115200) and shells
   out to `esptool.py write_flash …`.
5. After esptool completes, re-opens the protocol connection and sends
   `SLAVE_FLASH_END`.

This lives in `app/go/flash/coproc.go` next to the existing Pico-BOOTSEL
and ESP32-S3 download-mode entry points.

### Alternative: SWD/JTAG header

If cable-swap is unacceptable AND master-bridged flashing latency is too
high (e.g., for rapid iterate-flash-iterate dev loops), expose the C3's
JTAG over its native USB-Serial-JTAG peripheral via 4 test pads
(JTAG_TCK, TMS, TDI, TDO + GND). Any FT2232 or ESP-Prog flashes the C3
in-circuit without master involvement. Useful for board bring-up;
master-passthrough should be the production path.

---

## Protocol allocation

Use the **0x30–0x3F packet range** for the first on-board co-processor
(currently unallocated; sits between GunFX 0x01–0x2F and LightFX
0x40–0x5F).

```yaml
PWMFX_or_first_coproc:
  packet_range: "0x30-0x3F"
  error_range:  "0x70-0x7F"   # available between GearControl (0x60-0x6F) and System (0xF0-0xFF)
```

If multiple on-board co-processors are added later, allocate them
sequentially within the available ranges (see
[README.md § Critical Constants](README.md)).

The HubFX's existing **type-range slave routing** (build 1.0.0,
documented in [13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md))
forwards any packet whose type lies in a slave range to the matching
attached slave automatically. **This already works for UART-attached
slaves** — the routing layer doesn't care whether the slave is on USB
or UART. The only master-side change is registering the UART transport
in `SlaveManager` instead of going through `UsbRegistry`.

---

## Implementation checklist

For an on-board co-processor at packet range `0x30-0x3F` (using "pwmfx"
as the worked example), follow the standard 7-file pattern from
[02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) plus three master-side
additions for the UART transport and flash bridge:

```
# Standard 7-file pattern (per Rule 2)
controllers/lib/sfx_serial/serial/pwmfx/pwmfx.h     ← packet types, error codes, command set
controllers/pwmfx/c3/src/pwmfx_c3.ino              ← C3 firmware: BusServer + LEDC/MCPWM driver
controllers/pwmfx/c3/platformio.ini                ← board = esp32-c3-devkitm-1
controllers/pwmfx/c3/README.md                     ← board doc, pinout, capabilities
app/go/protocol/pwmfx/pwmfx.go                     ← Go mirror of the wire format
app/go/api/pwmfx.go                                ← typed PwmFxApi
app/go/engine/handlers/pwmfx/handler.go            ← CLI commands + observer wiring
app/go/engine/handlers/pwmfx/parsers.go            ← query-response renderers (CLI-only)

# Additional master-side files for UART transport
controllers/hubfx/esp32s3/src/coproc/uart_slave_bus.h    ← BusClient<PwmFx> backed by Serial1
controllers/hubfx/esp32s3/src/coproc/passthrough.cpp     ← slavePassthroughLoop() + GPIO setup
app/go/flash/coproc.go                                   ← scalefx-flash flash <coproc> command
```

Master `setup()` registers the UART slave statically (no
`UsbRegistry::onReady` callback — the slave is always present):

```cpp
// controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino — in setup()
Serial1.begin(SCALEFX_BUS_BAUD, SERIAL_8N1, SLAVE_RX_PIN, SLAVE_TX_PIN);
pinMode(SLAVE_RST,  OUTPUT); digitalWrite(SLAVE_RST,  HIGH);
pinMode(SLAVE_BOOT, OUTPUT); digitalWrite(SLAVE_BOOT, HIGH);
delay(50);                              // let the C3 finish its own boot

slaveManager.registerUartSlave(SlaveType::PWMFX, &Serial1);
slaveManager.identify(SlaveType::PWMFX);   // sends IDENTIFY, awaits INIT_READY
```

Total master-side delta: ~50 lines of slave registration + transport
setup, ~150 lines for `slavePassthroughLoop()` and the
`SLAVE_FLASH_BEGIN/END` ScaleFX commands. Per-board firmware on the C3
is a normal ScaleFX BusServer implementation — same pattern as any
existing slave.

---

## Reference: ESP32-C3 PWM capabilities

For PWM-expander use cases (the canonical reason to add an on-board
co-processor):

- **LEDC peripheral** — 6 channels, up to 14-bit, configurable frequency
  per channel (typical 1–10 kHz for LED dimming, 50–333 Hz for servos).
  See ESP-IDF `driver/ledc.h`.
- **MCPWM peripheral** — 1 unit, 3 timers, 6 outputs, with deadtime,
  carrier modulation, and trip zones (built for motor control). See
  ESP-IDF `driver/mcpwm_prelude.h`.
- **Combined** — 12 hardware PWM outputs is the practical ceiling.

For >12 outputs, either:
- Use multiple C3 slaves (each on its own UART) — ESP32-S3 has 3 UARTs,
  so up to 3 × 12 = 36 PWM outputs is feasible without I²C.
- Switch to RP2040 (16 outputs in one slave, but accept the SWD-header
  flashing trade-off).

---

## See also

- [01-ARCHITECTURE.md](01-ARCHITECTURE.md) — system overview, packet
  format, BusServer / BusClient class hierarchy.
- [02-NEW-CONTROLLER.md](02-NEW-CONTROLLER.md) — canonical 7-file
  template for adding any slave (USB or UART).
- [03-PROTOCOL-EXTENSION.md](03-PROTOCOL-EXTENSION.md) — adding commands
  to a slave's packet range.
- [05-BUILD-AND-FLASH.md](05-BUILD-AND-FLASH.md) — `scalefx-flash` CLI
  internals; extend with `coproc.go` for the passthrough flow.
- [13-PASSTHROUGH-ROUTING.md](13-PASSTHROUGH-ROUTING.md) — type-range
  routing on HubFX (works unchanged for UART-attached slaves).
