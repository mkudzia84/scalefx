# input_monitor — IN_1 (GPIO5) RC-input bring-up rig

Standalone ESP32-S3 sketch that watches the raw inbound RC signal on the
HubFX **IN_1** header (**GPIO5**) and prints the decoded value over
Serial, before trusting the production input path (`InputPort` /
`RcPwmInputRole`). One sketch, three compile-time modes selected by
`-DMONITOR_MODE` in [platformio.ini](platformio.ini):

| `MONITOR_MODE` | Mode | Capture method | Status |
|---|---|---|---|
| `0` | **PWM**  | edge IRQ, single channel | ✅ verified |
| `1` | **PPM**  | edge IRQ, composite sum-signal → N channels | ✅ verified |
| `2` | **SBUS** | hardware UART1 (100000 8E2 inverted) → 16 channels | ✅ verified |
| `3` | **JETI EX Bus** | hardware UART1 (125000 8N1, half-duplex, listen-only) | ✅ verified |

Output is plain ASCII, CRLF-terminated, on `Serial` @ 115200 (UART0 via
the CH343 bridge) so PlatformIO's monitor / any terminal renders it
clean. `monitor_rts=0` / `monitor_dtr=0` → opening the monitor does **not**
reset the board.

## Wiring

Signal → **GPIO5** (IN_1), ground → board **GND** (common ground with the
receiver is required). RC logic is 3.3–5 V; GPIO5 is 3.3 V — keep a 5 V
receiver output < 3.6 V if unsure (most modern Rx are 3.3 V). SBUS needs
**no external inverter** — the ESP32 UART inverts in silicon.

## Build / flash / monitor

```bash
# from tests/hw/input_monitor/
pio run -t upload          # build + flash (set the mode in platformio.ini first)
pio device monitor         # 115200, no reset
```

> On Windows the build/flash needs `IDF_COMPONENT_MANAGER=0` in the
> environment (component-manager / git workaround). In a bash shell:
> `IDF_COMPONENT_MANAGER=0 pio run -t upload`. PlatformIO's own
> "Advanced Memory Usage" table uses box-drawing glyphs that a cp1252
> PowerShell can't encode — harmless (`[SUCCESS]` still prints); run
> `chcp 65001` once if you want it gone.

## Mode 0 — PWM (single channel)

Times the HIGH pulse (rising→falling) on a `CHANGE` interrupt.

```
PWM GPIO5 | 1496 us  OK |  50 Hz | [........|:.........] | age=2ms
```

`OK`/`??` = inside the 800–2200 µs window · `Hz` = refresh rate · bar maps
1000–2000 µs across 20 cells (`:` marks centre) · `age` = ms since last edge.

## Mode 1 — PPM (composite sum-signal)

A `FALLING`-edge ISR times each channel slot (gap between edges); a gap
> 3000 µs (`PPM_SYNC_US`) is the **frame boundary**. PPM is
**self-describing**, so the rig auto-detects:

- **Channel count** = number of pulse slots between two sync gaps.
- **Frame width** = sync-to-sync period (→ refresh rate).

```
PPM [16ch] | 1505 1502 1501 1504 1500 1500 1000 1000 1000 1500 1500 1500 1500 1500 1500 1500 | frame=26.9ms 37Hz fps=40 age=2ms err=0
```

All channel values (µs) on one line. `-DPPM_EXPECTED_CH=N` only flags a
mismatch (`[16ch?]`) — detection runs regardless. Decode capacity is 24
(`PPM_MAX_CHANNELS`, the project-wide PPM max).

### Verified finding (2026-05-29)

On the bench transmitter the rig auto-detected a **16-channel** PPM frame
at **26.9 ms / ~37 Hz**, `err=0`. The radio emits a **fixed 16-slot
frame** regardless of how many channels are mapped — channels 1–9 were
the live/used channels, **10–16 sat rock-steady at 1500 µs (padding)**.
Confirmed not a decode-cap artifact by raising the cap to 24 and still
reading 16. Takeaway: a PPM "9-channel" setup still carries 16 wire slots;
the production role decodes all of them and config binds the named subset.

## Mode 2 — SBUS (inverted UART)

SBUS is a **UART** protocol, not edge-timed: 100000 baud, 8E2, signal
**inverted**. This mode claims a real hardware UART (`Serial1` = UART1)
with RX on GPIO5 and `invert=true`, letting the silicon do framing +
inversion — the same way the production `InputPort` SBUS mode consumes a
UART peripheral (Rule 31). No ISR; the UART driver buffers RX and `loop()`
parses 25-byte frames (`[0x0F][22 ch bytes = 16×11 bit][flags][footer]`),
resyncing on the header + footer-low-nibble signature.

```
SBUS [16ch] | 1500 1499 1001 1500 ... | flags=OK fps=140 age=2ms err=0
```

Values are µs (`raw·5/8 + 880`). `flags`: `OK` · `FL` frame-lost · `FS`
failsafe · `d17`/`d18` digital channels 17/18. Switch your receiver's
output to SBUS to drive this mode.

### Verified finding (2026-05-29)

Decoded a live SBUS stream on the first try: **16 channels**, `flags=OK`
(no frame-lost / failsafe), `err=0` (header + footer-nibble resync
locked). Same channel layout as the PPM bench (1–9 used, 10–16 padding at
1500). The reading showed `fps≈40` — lower than the standard SBUS 71 Hz
(14 ms) / 142 Hz (high-speed); that reflects the receiver's output rate,
not a decode issue (decode correctness is independent of frame rate). The
ESP32 UART inverts in silicon (`invert=true`), so no hardware inverter is
needed.

## Mode 3 — JETI EX Bus (half-duplex UART, listen-only)

Jeti EX Bus is a UART protocol like SBUS but **not inverted**: 125000 or
250000 baud, 8N1, **half-duplex** on a single TTL wire. This mode is
**listen-only** — the receiver (master) transmits channel-data packets
continuously whether or not a device answers, so servo channels decode
without driving the bus. Set the receiver pin to **"EX Bus"**. If there's
no lock, rebuild with `-DJETI_BAUD=250000`.

```
JETI [12ch] | 1500 1499 1001 1500 ... | fps=50 age=2ms crcErr=0
```

Packet: `[0x3E/0x3D][resp][len][id][0x31 data-id][sublen][16-bit LE chans][CRC16]`.
Channels are 16-bit little-endian in 1/8 µs → µs = `raw/8`. CRC16 is
CCITT-reflected (poly `0x8408`, init 0); bad CRC increments `crcErr` and
the frame is dropped. Telemetry (`0x3A`) and JetiBox (`0x3B`) packets are
ignored — this rig only reads channels.

### Verified finding (2026-05-29)

Decoded a live Jeti EX Bus stream at **125000 baud**: **16 channels**,
`crcErr=0`, `fps≈90–100` (faster channel cadence than SBUS/PPM). Same
channel layout as the PPM/SBUS bench (1–9 used, 10–16 padding) — three
protocols, identical decode, cross-confirming correctness. Two gotchas:
- **Baud must be 125000**, not 250000. At 250000 the `rawBytes` counter
  floods with garbage (2× oversampling of the 125k line) yet yields zero
  valid frames — the fingerprint of a wrong baud. The byte counter in the
  no-signal line is what disambiguates "wrong baud" (bytes, no frames)
  from "no wire" (no bytes).
- The frame state machine must hold off the completion check until the
  length byte (`[2]`) is read; an off-by-one that treated byte `[1]` as
  body "completed" a zero-length packet and silently dropped every frame.

### What's needed for radio discovery (talking BACK to the radio)

Listen-only (above) covers using Jeti as an **input**. Making the HubFX
appear **on the radio** — in the telemetry sensor list, JetiBox menu, or a
custom transmitter app — is a separate, larger feature because EX Bus is
half-duplex and discovery is driven by the device *responding* to polls:

1. **Half-duplex responder (firmware).** When the Rx sends a master packet
   with byte[1] = `0x01` (response allowed), the device must, within the
   slot (~hundreds of µs), drive the same wire and send a `0x3D` response.
   On a single wire you tri-state TX except during the response slot (UART
   half-duplex / RS485-style DE, or a diode-OR'd RX+TX). Getting the
   turnaround timing right is the hard part.
2. **EX telemetry device description (firmware).** Respond with EX
   telemetry text frames (data-id `0x3A`, message type 0) declaring the
   **device name + per-sensor labels + units**. The radio reads these and
   the device **auto-appears** in the telemetry list — *this is the
   "discovery"; no file on the radio is required for it.* Then stream value
   frames (message type 1) at ~10 Hz.
3. **Config UI on the transmitter (optional, two routes):**
   - **JetiBox menu** (firmware only): respond to `0x3B` JetiBox packets
     emulating the 5-button 2×16 LCD menu — works on every Jeti radio, no
     files needed.
   - **Lua app** (file on the radio): JETI DC/DS transmitters run Lua.
     Drop a `<name>.lua` (often a folder under `Apps/`, e.g.
     `/Apps/SCALEFX/scalefx.lua`) on the radio's SD card; it reads/writes
     ScaleFX telemetry and renders a richer config screen. This is the only
     part that ships a **file to the radio** — and it's optional polish on
     top of the firmware responder, not required for basic discovery.

Scope note: the production path for (1)+(2) belongs in the multi-modal
`InputPort` / a new EX-telemetry role, not in this bench rig. This rig
stays listen-only.
