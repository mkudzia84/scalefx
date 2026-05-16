# HubFX PCA9685 Hardware Test

Bring-up fixture for the PCA9685BS (U54) on the 8-channel HubFX board.
Doubles as the canonical reference for what a *clean* PCA9685 init
sequence looks like — every step that turned out to matter during
bring-up is in here, with the symptom that motivated it.

Test target: PCA9685 at 7-bit I²C address `0x70` on the HubFX shared
I²C bus (SDA=GPIO8, SCL=GPIO9, 100 kHz). Drives LED0..LED7 → MOSFET
gates → 8 LED rails. See
[../../../controllers/hubfx/esp32s3/PINOUT.md](../../../controllers/hubfx/esp32s3/PINOUT.md)
for the full bus map and signal chain.

## What the firmware does

1. Boot banner + system info.
2. Pre-Wire bus health check (steps 1–3 of the verification log).
3. Wire driver bring-up + chip probe (steps 4–5).
4. Boot-time chip verification (steps 6–9): a 17-check sequence that
   exercises every register access path the production driver will
   need — SWRST round-trip, MODE1/MODE2/PRESCALE writes with read-back
   compare, LED0 single-channel burst, ALL_LED broadcast.
5. Step 10: wake → sleep round-trip.
6. Main loop: continuous gamma-corrected sin² breathing across all 8
   channels via ALL_LED broadcast, or a constant-duty diagnostic mode
   (toggled by `CONSTANT_OUTPUT`).
7. Status line every 1 s with frame rate, current duty, peak duty,
   NACK count.

Compile-time knobs (top of the .ino):

| Constant | What it does |
|---|---|
| `PWM_FREQUENCY_HZ` / `PRESCALE_VALUE` | Operating PWM frequency. Default 1526 Hz (chip max). 200 Hz POR has visible flicker. |
| `BREATHING_PERIOD_MS` / `BREATHING_FRAME_HZ` / `BREATHING_GAMMA` | Animation shape. |
| `PEAK_FRACTION` | Peak duty as a fraction of 4095. Diagnostic knob — drops the LED current at peak to isolate supply-side flicker from chip-side. |
| `CONSTANT_OUTPUT` | `true` = constant duty at PEAK_FRACTION (no animation, useful for steady-state visual + scope inspection). `false` = continuous breathing. |

Build / flash:

```
pio run -t upload
pio device monitor
```

## Clean PCA9685 init sequence

Everything below is what the production firmware's PCA9685 driver
needs to do. Numbers and rationale come from this session's hardware
bring-up.

### 1. Pre-Wire bus health check (GPIO mode)

```cpp
pinMode(SDA_PIN, INPUT_PULLUP);
pinMode(SCL_PIN, INPUT_PULLUP);
delayMicroseconds(50);
bool sdaHigh = digitalRead(SDA_PIN);   // expect HIGH (external pull-up)
bool sclHigh = digitalRead(SCL_PIN);   // expect HIGH
```

If SCL is LOW, the bus is wedged — pull-ups missing, short, or a slave
holding it. Don't proceed without fixing.

If SDA is LOW, a slave is holding it from a previous unfinished
transaction. Recovery (step 2) will release it.

### 2. Manual bus recovery (9 SCL clocks + STOP)

Standard recovery sequence. Clocks SCL up to 9 times so any slave
that's mid-byte sees enough clocks to finish, NACK, and release SDA.
Then a manual STOP condition resets all slaves to idle.

```cpp
pinMode(SDA_PIN, INPUT_PULLUP);
pinMode(SCL_PIN, OUTPUT);
digitalWrite(SCL_PIN, HIGH);
delayMicroseconds(10);
for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    if (digitalRead(SDA_PIN) == HIGH) break;   // slave released — done
}
// STOP: SDA L→H while SCL HIGH
pinMode(SDA_PIN, OUTPUT);
digitalWrite(SDA_PIN, LOW);   delayMicroseconds(5);
digitalWrite(SCL_PIN, HIGH);  delayMicroseconds(5);
digitalWrite(SDA_PIN, HIGH);  delayMicroseconds(5);
pinMode(SDA_PIN, INPUT_PULLUP);
pinMode(SCL_PIN, INPUT_PULLUP);
```

The SDA-sampling early-exit makes this fast when nothing is stuck.

### 3. Wire.begin — 3-arg form only

```cpp
Wire.begin(SDA_PIN, SCL_PIN, 100000);   // set clock during init
```

**Do NOT call `Wire.setClock()` after `Wire.begin()`.** On ESP-IDF
I²C-NG (Arduino-ESP32 ≥ 3.x), `setClock()` after `begin()` can leave
the driver in `ESP_ERR_INVALID_STATE` — every subsequent transaction
returns NACK with no actual bus activity. Always use the 3-arg form.

100 kHz works reliably across this board's bus. 400 kHz observed
intermittent NACKs during the initial bring-up; the chip supports up
to 1 MHz but the lower speed gives margin against trace capacitance.

### 4. Probe with retries

```cpp
for (int attempt = 1; attempt <= 5; attempt++) {
    Wire.beginTransmission(0x70);
    if (Wire.endTransmission() == 0) return true;   // ACK
    delay(50);
}
```

Five 50 ms-spaced attempts. A freshly powered chip can take one or two
attempts to come up; without retries you'll occasionally see a clean
board "fail to find" the chip.

If all five fail, the chip is either (a) latched into the dead-silent
state (recoverable only by a long VDD discharge, see "Gotchas" below)
or (b) missing/damaged. Either way, log and abort — don't keep writing
to a chip that's not ACKing.

### 5. Software-reset via general-call

```cpp
Wire.beginTransmission(0x00);   // I²C general call address
Wire.write(0x06);                // PCA9685 SWRST opcode (datasheet §7.1.4)
Wire.endTransmission();
delay(2);
```

This works **even when the chip is silent at its assigned 0x70** —
the general-call address (0x00) bypasses the per-device address
comparator. Every PCA9685 on the bus that has its GC-ACK bit enabled
(the POR default) will perform a full reset.

It's harmless to run on a healthy chip — verified by reading MODE1
before and after: both equal 0x11 (the POR default), so SWRST is
non-destructive. Issue once on init; issue again at runtime if a NACK
ever appears (cheap recovery attempt before any other action).

After SWRST the chip's state is exactly POR:

| Register | POR value | Meaning |
|---|---|---|
| MODE1 (0x00) | `0x11` | SLEEP=1, ALLCALL=1 |
| MODE2 (0x01) | `0x04` | OUTDRV=1 (push-pull), INVRT=0, OCH=0 |
| PRESCALE (0xFE) | `0x1E` | ~200 Hz update rate |

### 6. Register configuration

PRESCALE can only be written while `MODE1.SLEEP=1`. The chip wakes up
in SLEEP after POR / SWRST, so this just falls naturally:

```cpp
// SLEEP entry (already there after POR, but write it anyway for clarity)
i2cWriteReg(REG_MODE1, MODE1_SLEEP | MODE1_ALLCALL);     // 0x11
delay(1);

// PRESCALE — 1526 Hz target = 25 MHz / (4096 × 1526) − 1 = 3
i2cWriteReg(REG_PRESCALE, 0x03);

// MODE2 — push-pull OUTDRV (default), explicit so we don't depend on POR
i2cWriteReg(REG_MODE2, MODE2_OUTDRV);                    // 0x04

// Wake: clear SLEEP, enable AI (auto-increment) for burst writes
i2cWriteReg(REG_MODE1, MODE1_AI | MODE1_ALLCALL);        // 0x21
delayMicroseconds(500);                                  // §7.3.1.1 osc settle

// Trigger RESTART — re-enables any outputs that were running before SLEEP
i2cWriteReg(REG_MODE1, MODE1_AI | MODE1_ALLCALL | MODE1_RESTART);  // 0xA1
delay(1);
```

Each write should be followed by a register read-back compare during
bring-up. The production driver can drop the read-back once the
sequence is proven; for diagnostic builds keep it.

### 7. Driving the LEDs

Always use plain PWM mode — never set the FULL_ON / FULL_OFF flag bits
during animation. The chip glitches visibly at the transition into and
out of those modes.

```cpp
// Single channel:
Wire.beginTransmission(0x70);
Wire.write(REG_LED0_ON_L);            // burst-write target (AI enabled)
Wire.write(0x00);                      // ON_L  = 0
Wire.write(0x00);                      // ON_H  = 0 (no FULL_ON)
Wire.write(duty & 0xFF);               // OFF_L = duty low byte
Wire.write((duty >> 8) & 0x0F);        // OFF_H = duty high nibble (no FULL_OFF)
Wire.endTransmission();

// All 16 channels in one atomic write:
Wire.beginTransmission(0x70);
Wire.write(REG_ALL_LED_ON_L);          // ← the only difference
Wire.write(0x00); Wire.write(0x00);
Wire.write(duty & 0xFF); Wire.write((duty >> 8) & 0x0F);
Wire.endTransmission();
```

Clamp `duty` to `[1, 4095]`. At `duty=0` the chip's behaviour with
ON==OFF is undefined; at `duty=4095` the output is HIGH for 4095 of
4096 ticks per cycle (99.976 %) which is visually identical to
FULL_ON but doesn't trigger the mode-switch glitch. The 1-tick LOW per
cycle is 160 ns at 1526 Hz — physically imperceptible.

The ALL_LED broadcast writes all 16 channels in a single transaction.
With `MODE2.OCH=0` (default), all 16 OFF registers latch atomically at
the I²C STOP. This is what keeps the 8 LED rails phase-locked during
animation.

## Gotchas (learned the hard way)

### Latch-up after stress — and how to recover

**Symptom.** The chip stops ACK'ing its assigned 7-bit address `0x70`.
A bus scan shows the other 9 devices (8× INA226 + TAS5825P) but
0x70 is silent. Survives:

- MCU soft-reset (RESET button / `esp_restart()` / Watchdog).
- `Wire.end()` followed by `Wire.begin()` from scratch.
- The 9-clock SCL bus-recovery toggle.
- General-call SWRST (`write 0x06 to addr 0x00`) — the chip ignores
  even the broadcast that's supposed to bypass its address comparator.
- Re-flashing the ESP32-S3 (because that doesn't drop VDD on the
  PCA9685).

**Triggers seen in this session.** Both are now closed off by the
conservative init flow, but they're worth knowing:

1. `Wire.setClock(...)` called after `Wire.begin(...)` — on
   Arduino-ESP32 3.x the I²C-NG driver landed in
   `ESP_ERR_INVALID_STATE` and started a NACK storm against the chip.
   The chip's state machine apparently latched after one too many
   malformed transactions.
2. Reflashing the ESP32-S3 while the bus was mid-transaction. The
   `esptool` reset sequence toggles GPIO8/9 through indeterminate
   states for several hundred milliseconds. A sensitive chip can
   interpret the noise as fragmentary bus activity and stick.

**Recovery — full VDD discharge.** This is the *only* recovery we've
found that works.

The HubFX board has substantial bulk capacitance on the VBAT rail:
6× 10 µF + 1× 470 µF + 2× 1000 µF + supporting bulk caps ≈ **3.4 mF
of total reservoir**. With the chip drawing ~10 mA at idle, that
discharges through any incidental loads at less than ~3 V/s — a brief
unplug-replug doesn't get VDD anywhere near 0 V, so the chip stays
powered enough to retain its stuck state.

**Procedure:**

1. Unplug USB-C completely.
2. **Wait at least 60 seconds.** Longer is safer.
3. (Optional but informative) Probe the on-board 3V3 test point with
   a multimeter and confirm it has fallen below 0.3 V. If it's still
   above ~1 V, wait longer — the chip won't reset.
4. Re-plug USB-C.
5. Flash and run `tests/hw/hubfx_i2c_scan/` (the minimal scanner) to
   confirm `0x70` is now on the bus. **Don't flash the PCA9685
   hwtest until the bare scanner reports 10/10 devices** — the
   hwtest's PRESCALE write could re-trigger the stuck state if the
   chip is still marginal.
6. Once the scanner is clean, flash this fixture. The boot-time
   verification will then complete its 17 checks against a known-good
   chip.

If 60 seconds doesn't recover it, escalate the unplug to 5 minutes.
If 5 minutes doesn't recover it, suspect physical damage (ESD, a
bridged solder pad, or a manufacturing defect) and probe the chip's
VDD / OE / address-strap pins with a meter.

### Will SWRST disturb other devices on the bus? — No.

The general-call SWRST (`START · 0x00 · 0x06 · STOP`) is a broadcast
on the shared HubFX I²C bus. The natural question is whether it
affects the TAS5825P codec or the eight INA226 rail monitors.

**Short answer: only the PCA9685 responds. The other nine devices
ignore the transaction entirely.**

Per the I²C spec (NXP UM10204 §3.1.13), a device responds to the
general-call address only if its general-call ACK feature is enabled.
Devices that don't enable it NACK the address byte on the START and
take no further action — the data byte `0x06` simply doesn't reach
them.

| Device | Address | Responds to general call? | Behaviour on `0x00 ← 0x06` |
|---|---|---|---|
| PCA9685 (U54) | 0x70 | **Yes** — datasheet §7.1.4 ALLCALL enabled at POR | Full chip reset to POR state |
| TAS5825P (U55) | 0x4C | No — TI datasheet does not document general-call support | Ignores the transaction |
| INA226 ×8 (U43–U48, U52, U53) | 0x40–0x45, 0x4A, 0x4F | No — INA226 datasheet describes only direct addressing via A0/A1 pins | Ignores the transaction |

**Empirical confirmation from this session:**

- During the canary-loop development, this fixture broadcast a SWRST
  every 2 seconds while `sfx_test_p` had the codec in **PLAY mode**.
  If the SWRST were reaching the codec, it would have dropped the
  codec back to `DEEP_SLEEP` (per the TAS5825 datasheet state
  machine), and the audio output would have stopped. The codec stayed
  in PLAY across thousands of SWRSTs, audio uninterrupted.
- INA226 rail-voltage reads continued returning sane values across
  the same SWRST loop — no register-default resets observed.
- The boot-time verification confirms the PCA9685 *does* reset on
  SWRST: pre-SWRST inherited state shows the last-run's MODE1 (e.g.
  `0x21` or `0x31`), post-SWRST shows `0x11` (POR default). Visible
  in verification step 1 vs step 3.

**Caveat for future hardware revisions.** If a future HubFX rev adds
another I²C device that *does* enable general call (some EEPROMs,
some other LED drivers, some hub controllers), the SWRST broadcast
will reset it too. The mitigation is to issue SWRST only when the
PCA9685 is actually stuck (i.e. it failed its probe), not as a
prophylactic on every init. The current fixture errs on the side of
prophylactic for diagnosis; a production driver should issue SWRST
only as a recovery action after the first probe NACKs.

### setClock after begin → INVALID_STATE

Don't. Use `Wire.begin(sda, scl, freq)`.

### Flicker at high duty was supply-side, not chip-side

During the breathing animation, visible flicker at the peak and during
fade-out turned out to be **input voltage sag under LED load** — the
LED rail couldn't supply peak current to all 8 channels simultaneously
on a low-voltage input.

**Fix: run on 3S LiPo input (11.1 V nominal).** Lower input
voltages don't leave enough buck headroom for the LED rail to stay
regulated when 8 channels are at peak. The chip's PWM at 1526 Hz is
flicker-free on its own; if you see flicker, suspect the supply first.

Diagnostic shortcut: drop `PEAK_FRACTION` (e.g. to 0.6, 0.3, 0.2). If
flicker disappears proportionally, it's supply-side.

### POR vs operating values

POR defaults differ from what we actually want:

| Field | POR | Operating |
|---|---|---|
| PRESCALE | `0x1E` (200 Hz) | `0x03` (1526 Hz) — above flicker threshold |
| MODE1 | `0x11` (SLEEP=1) | `0x21` (SLEEP=0, AI=1) during play |
| MODE2 | `0x04` (OUTDRV=1) | `0x04` (no change needed) |

After SWRST always re-apply the operating values. Don't assume the
chip is left as the previous run configured it — SWRST is the only
deterministic state.

### MODE1.AI bit must be set for burst writes

Burst writes (4 bytes per LED channel, 5 bytes for ALL_LED) rely on
the chip auto-incrementing the register pointer. Without
`MODE1.AI=1`, every byte after the first would land in the same
register, overwriting the previous bytes. Enable AI before any LED
register write.

### MODE2.OCH = 0 is what keeps channels in sync

With `OCH=0` (the default), all 16 LEDn_OFF registers latch
simultaneously at I²C STOP. With `OCH=1`, they latch on each byte's
ACK, giving 16 separate update times — which would un-sync the
channels. Don't change the default.

## Verification recipe

The fixture's boot-time verification covers every path:

1. Read inherited register state (didn't crash, can read)
2. SWRST broadcast (general-call ACK works)
3. POR defaults match datasheet (MODE1=0x11, MODE2=0x04, PRESCALE=0x1E)
4. MODE2 INVRT toggle + restore (single-register write/readback)
5. MODE1 SLEEP entry (write path with bit field)
6. PRESCALE write while in SLEEP (operating frequency)
7. MODE1 AI enable (enables burst writes)
8. LED0 single-channel burst (4-byte write, AI verified)
9. ALL_LED broadcast across 8 channels (read back from each, all match)
10. Wake/sleep round-trip (mode transitions clean)

All 17 checks must pass. If any fail, halt and dump state — don't
proceed to drive outputs against a chip that's misbehaving.

## Cross-references against the PCA9685 datasheet

Everything in this document has been cross-checked against:

- **NXP UM10851 — PCA9685 datasheet**, Rev. 4 (16 April 2015). The
  authoritative spec. Section numbers cited inline above (`§7.1.4`,
  `§7.3.1.1`, `§7.3.2`, `§7.3.3`, `§7.3.5`) refer to that document.
- The Linux kernel `drivers/pwm/pwm-pca9685.c` register definitions
  and init flow (authoritative open-source implementation).
- Adafruit's `Adafruit_PWMServoDriver` library (the widely-used
  community reference).

### Register addresses

| Symbol | Address | Source confirmed |
|---|---|---|
| `MODE1`         | `0x00`        | datasheet §7.3.1, Linux, Adafruit |
| `MODE2`         | `0x01`        | datasheet §7.3.2, Linux, Adafruit |
| `LEDn_ON_L`     | `0x06 + 4·n`  | datasheet §7.3.3, Linux, Adafruit |
| `LEDn_ON_H`     | `0x07 + 4·n`  | datasheet §7.3.3, Linux, Adafruit |
| `LEDn_OFF_L`    | `0x08 + 4·n`  | datasheet §7.3.3, Linux, Adafruit |
| `LEDn_OFF_H`    | `0x09 + 4·n`  | datasheet §7.3.3, Linux, Adafruit |
| `ALL_LED_ON_L`  | `0xFA`        | datasheet §7.3.3, Linux, Adafruit |
| `ALL_LED_ON_H`  | `0xFB`        | datasheet §7.3.3, Linux, Adafruit |
| `ALL_LED_OFF_L` | `0xFC`        | datasheet §7.3.3, Linux, Adafruit |
| `ALL_LED_OFF_H` | `0xFD`        | datasheet §7.3.3, Linux, Adafruit |
| `PRESCALE`      | `0xFE`        | datasheet §7.3.5, Linux, Adafruit |

### MODE1 (0x00) bit layout

Per datasheet §7.3.1 Table 5:

| Bit | Mask | Symbol | Function | POR |
|---|---|---|---|---|
| 7 | `0x80` | `RESTART` | Resume paused PWM after wake | 0 |
| 6 | `0x40` | `EXTCLK`  | Use external clock input | 0 |
| 5 | `0x20` | `AI`      | Auto-increment register pointer | 0 |
| 4 | `0x10` | `SLEEP`   | Oscillator off / low-power | **1** |
| 3 | `0x08` | `SUB1`    | Respond to sub-address 1 | 0 |
| 2 | `0x04` | `SUB2`    | Respond to sub-address 2 | 0 |
| 1 | `0x02` | `SUB3`    | Respond to sub-address 3 | 0 |
| 0 | `0x01` | `ALLCALL` | Respond to LED All Call address | **1** |

POR = `0x11` (SLEEP + ALLCALL). Confirmed by reading the chip on this
PCB immediately after SWRST — see verification step 3 output:
`R MODE1 0x00 = 0x11 (expect 0x11) ✓ match`.

### MODE2 (0x01) bit layout

Per datasheet §7.3.2 Table 6:

| Bit | Mask | Symbol | Function | POR |
|---|---|---|---|---|
| 4 | `0x10` | `INVRT`  | Invert output logic (used with external driver) | 0 |
| 3 | `0x08` | `OCH`    | 0 = outputs change on STOP, 1 = on ACK | 0 |
| 2 | `0x04` | `OUTDRV` | 0 = open-drain, 1 = totem-pole (push-pull) | **1** |
| 1:0 | `0x03` | `OUTNE`  | Behaviour when `~OE` is HIGH | 00 |

POR = `0x04` (OUTDRV push-pull). Confirmed by readback after SWRST.
`OCH=0` is the default and what we rely on for the atomic all-channel
broadcast latch — see "MODE2.OCH = 0 is what keeps channels in sync".

### PRESCALE (0xFE)

Per datasheet §7.3.5:
- Formula: `prescale = round(25 MHz / (4096 × f_PWM)) − 1`
- Range: `3..255` (i.e. ~24 Hz to ~1526 Hz)
- **Writable only while `MODE1.SLEEP = 1`** — writes to PRESCALE while
  awake are silently ignored.
- POR value: `0x1E` (30) → 25e6 / (4096 × 31) = **200 Hz**. Confirmed
  on this chip.

### Software reset via I²C general call (datasheet §7.1.4)

> "The PCA9685 also responds to the I²C-bus General Call address. The
> least-significant bit of the General Call command byte is set to
> logic 0. The PCA9685 will only respond to General Call command bytes
> = `0x06` (SWRST) ... All PCA9685 devices on the I²C-bus reset their
> internal state and return to a power-on default state."

Exact byte sequence on the wire: `START · 0x00 · ACK · 0x06 · ACK · STOP`.

In Arduino-Wire:

```cpp
Wire.beginTransmission(0x00);   // I²C general call address
Wire.write(0x06);                // SWRST opcode
Wire.endTransmission();
```

**Notable: neither the Linux kernel driver nor the Adafruit library
implements this.** Adafruit's `reset()` writes `MODE1 ← 0x80` (the
`RESTART` bit), which is a per-device "resume paused PWM" command —
*not* a chip reset. The general-call SWRST is the only path that:

1. Resets all internal registers to POR.
2. Bypasses the per-device address comparator — works when the chip
   is silent at its assigned address (the recovery scenario we hit
   twice during this session's bring-up).

We deliberately include it as the first I²C transaction after probe.

### MODE1.RESTART (§7.3.1.1)

> "When restart is enabled and the user clears the SLEEP bit, the
> PCA9685 will restart the PWM channels with the configuration they
> had before SLEEP was set. ... A delay of 500 µs maximum is required
> after the SLEEP bit is cleared to allow the oscillator to
> stabilize."

Sequence we follow (matches the datasheet's flow chart Fig. 23):

```
MODE1 ← SLEEP=1                  // park
write PRESCALE                   // only valid in SLEEP
MODE1 ← SLEEP=0, AI=1            // wake, enable burst writes
delayMicroseconds(500);          // oscillator settle (§7.3.1.1 max)
MODE1 ← SLEEP=0, AI=1, RESTART=1 // resume paused PWM (auto-clears)
```

Adafruit's library uses `delay(5)` here (5 ms). That's ~10× the
datasheet minimum but functionally fine — just slower boot.

### FULL_ON / FULL_OFF (§7.3.3)

Bit 4 of `LEDn_ON_H` is the `FULL_ON` override (output driven
permanently HIGH). Bit 4 of `LEDn_OFF_H` is `FULL_OFF` (output driven
permanently LOW). Both Linux and Adafruit confirm this as
`LED_FULL = BIT(4) = 0x10`.

The datasheet states (Table 7):
> "If both LEDn_ON and LEDn_OFF have their respective `LEDn_FULL` bits
> set, the `LEDn_FULL_OFF` function takes precedence."
>
> "If LEDn_ON ≥ LEDn_OFF, the output is HIGH for 4096 × t_PWM − (ON −
> OFF) × t_PWM seconds per cycle."

In practice the cleanest path is to **never set either FULL_ flag
during animation** and clamp the duty to `[1, 4095]` — see the
"Driving the LEDs" section above and the "Gotchas" entry on flicker.

### ALL_LED broadcast registers (§7.3.3)

Writing the `ALL_LED_*` registers (`0xFA..0xFD`) updates the
corresponding fields of every LED0..LED15 channel simultaneously. The
update latches on I²C STOP (when `MODE2.OCH = 0`). Reads from these
registers return `0x00`.

This is the only way to update multiple channels phase-locked.

### Where this README's procedure differs from Linux / Adafruit

| Step | This README | Linux | Adafruit |
|---|---|---|---|
| Pre-Wire bus health check | yes | no | no |
| 9-clock SCL bus recovery | yes | no | no |
| General-call SWRST `0x00 ← 0x06` | yes | no | no |
| Multi-retry probe (5 × 50 ms) | yes | no | retry once |
| Oscillator settle delay | 500 µs (datasheet minimum) | 500 µs | 5 ms |
| Use `MODE1.RESTART` after wake | yes | yes | yes |
| FULL_ON / FULL_OFF clamping | always-PWM mode | n/a (uses period/duty math) | sets FULL flags at extremes |
| `Wire.begin(sda,scl,freq)` 3-arg form | required | n/a (kernel I²C subsystem) | n/a |

The differences exist because both Linux's PWM subsystem and
Adafruit's library assume a chip on a known-good bus that just powered
on — neither needs to recover from a stuck-state chip. Our fixture
does, hence the extra defensive steps.

## What still needs doing

The production HubFX firmware
([../../../controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino](../../../controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino))
still uses the old AW9523B driver (`#include <gpio/aw9523b.h>`) for
local LED control. On this PCB the AW9523B is absent and PCA9685 has
replaced it. A new driver at
`controllers/lib/sfx_peripherals/pwm/pca9685.h` should:

- Implement the init sequence from this README.
- Expose `setChannel(uint8_t ch, uint16_t duty)` and
  `setAllChannels(uint16_t duty)`.
- Apply `MODE1.AI=1` so per-channel burst writes work.
- Use 1526 Hz PWM by default (`PRESCALE=0x03`).
- Refuse duty values outside `[0, 4095]` and internally clamp to
  `[1, 4095]` to avoid the FULL_ON / FULL_OFF / ON==OFF edge cases.
- Survive a stuck-chip event by retrying with general-call SWRST.

The LED runtime
([../../../controllers/lib/sfx_peripherals/led/led_manager.h](../../../controllers/lib/sfx_peripherals/led/led_manager.h))
can then take this driver as its `LedBrightnessExpander` template
parameter — same call shape as the AW9523B side, so the rest of the
LED program / event-sequence code stays unchanged.
