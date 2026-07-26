# HubFX INA226 Clone Wedges the PCA9685

> **Status:** debugging gotcha &middot; **Read when:** the PCA9685 LED driver goes silent at boot, or an I²C chip wedges another chip on the shared bus.
> **TL;DR:** A counterfeit INA226 @ 0x40 corrupts the PCA9685 @ 0x70 when written to; `INA226::begin()` now refuses any chip failing the canonical TI MFG/DIE ID check, so it never writes to a clone.
>
> ⚠️ **2026-07-02 RE-INTERPRETATION (rev B netlist + bench probe):** the
> "clone" was almost certainly never a counterfeit chip. The PCA9685's
> **hardware address is 0x40** (all six A-pins grounded; the firmware's
> 0x70 is only its default-enabled all-call alias) — so 0x40 hosts TWO
> chips, and every symptom below is an address collision, not a bad part.
> See [§Re-interpretation](#re-interpretation-2026-07-02--it-was-never-a-clone)
> at the end. The ID-gate fix stands either way — it's exactly what keeps
> 0x40 unwritten.

Investigation log + fix for the bring-up issue where the PCA9685 LED driver
@ I²C `0x70` on HubFX would consistently go silent during `board.begin()` and
only come back via a general-call SWRST + re-init. Landed 2026-05-20 in
[ina226.cpp:14](../controllers/lib/sfx_peripherals/power/ina226.cpp#L14).

This file is the reference for: **why the INA226 driver now refuses to drive
chips that fail the canonical TI ID check.**

## Symptom (pre-fix)

Every HubFX boot, this triplet appeared in the diag log:

```
[WARN] [PCA] post-init probe failed — chip wedged during board.begin() port-init.
              Running recovery: SWRST → re-init → re-push duties.
[WARN] [PCA] recovery OK — chip back at 0x70 MODE1=0x21 (reinit succeeded)
[INFO] [PCA] PCA9685 @ 0x70: post-board.begin() OK  MODE1=0x21
```

The recovery worked, so PWM was functional, but the chip was being
software-recovered on every boot. That's not a steady state — it indicated
something was wedging the chip's address comparator during normal
initialisation.

## TL;DR

- **Root cause.** HubFX 8-channel rev ships a counterfeit "INA226" at I²C
  address `0x40` (boot MFG/DIE IDs `0x0001` / `0x0020` — genuine TI returns
  `0x5449` / `0x2260`). Two boards out of two checked have the same chip at
  the same address with the same fake fingerprint → batch-level PCB assembly
  issue, not a one-off bad solder.
- **Mechanism (confirmed).** Reading the clone's registers `0xFE` / `0xFF`
  (MFG/DIE) is **safe**. Writing CONFIG `0x8000` to it during the INA226
  driver's `reset()` step wedges the **real** PCA9685 at `0x70` until a
  gen-call SWRST recovers it.
- **Mechanism (hypothesis, unconfirmed).** The clone is almost certainly a
  PCA9685-family die. POR default for `ALLCALLADR` is `0xE0` (7-bit `0x70`,
  which equals the real PCA's strap address) and `MODE1.ALLCALL = 1`. The
  bytes the INA226 driver puts on the wire to write CONFIG `0x8000` are
  `[0x00, 0x80, 0x00]`. On a PCA9685 those land as `MODE1 ← 0x80` =
  RESTART. The clone matches its own strap (`0x40`) AND ALLCALL (`0x70`)
  simultaneously, so the same write also propagates to the real PCA via
  ALLCALL and triggers `MODE1.RESTART` on it — which leaves the real
  PCA's address comparator in an undefined post-restart state until
  gen-call SWRST clears it. We did not bench-prove this last step (would
  require a logic analyser + a confirmed PCA9685 part on a known-good
  board), but it's the simplest theory that fits every observed bisection
  step.
- **Fix.** `INA226::begin()` now reads MFG/DIE **before** any write and
  bails (returns `false`, leaves `_available = false`) if they don't match
  canonical TI IDs. Boot IDs are still readable via `bootMfgId()` /
  `bootDieId()` / `isCanonical()` for diagnostics. No firmware-side
  recovery is needed any more on HubFX — the PCA never gets wedged in the
  first place.
- **Hardware action item.** Replace U43 (the chip at `0x40`) on every HubFX
  board with a genuine TI INA226 to restore 8/8 V/I sense. Until then
  ch1's voltage / current readings are zero. Tracked in
  [99-HW-TODO.md](99-HW-TODO.md).

## Bisection trail

The investigation is the interesting part — none of the obvious early
hypotheses survived contact with the evidence. The bisection happened
across builds 79 → 88 of `hubfx_esp32s3.ino`.

### Initial state (build 79)

Pre-existing instrumentation showed the wedge happening "during
`board.begin()` port-init". Build 79's `logHardwareStatus()` already had
the SWRST + re-`pca.begin()` + duty-restore recovery and logged "recovery
OK". The recovery worked on every boot but didn't tell us *what* was
wedging the chip.

### Hypothesis #1 (REJECTED): ALLCALL collision

Reasoning: PCA9685's `ALLCALLADR` POR is `0xE0` (7-bit `0x70`), which
equals the strap address on HubFX (A4–A6 high). With `MODE1.ALLCALL=1`,
the chip's address comparator matches both strap and ALLCALL on every
write to `0x70`. A documented quirk on some PCA9685 silicon revisions is
that this double-match can latch the comparator into a bad state after
several back-to-back writes.

**Test:** Disabled `MODE1.ALLCALL` in the PCA9685 driver
(`pca9685.cpp`). Removed the bit from every `MODE1` write in `begin()` /
`setFrequency()` / `sleep()` / `wake()`.

**Result (build 82):** Made things **worse**. The wedge moved earlier
(to the first per-channel `setChannel(0, 0)` write inside
`initHardware()`) and the SWRST recovery itself failed. Reverted.

**Why it failed:** The hypothesis was wrong about the trigger. ALLCALL is
not the mechanism; the real PCA at `0x70` is not the chip being
"primed" into the bad state by the ALLCALL match. (We now know the
trigger is upstream — a *different* chip on the bus, the clone at
`0x40`.)

### Hypothesis #2 (REFUTED): per-channel writes are the trigger

Reasoning: the wedge happens "during port-init", and port-init's job
is calling `Pca9685PwmPort::begin()` for each of the 8 PWM ports, which
issues a `setDuty(0)` per port (5-byte I²C write to `0x70`). Plausibly,
8 back-to-back PCA register writes do the wedging.

**Test (build 83/84):** Added a "pre-fire" loop in `initHardware()` that
calls `pca.setChannel(k, 0)` for each of the 8 channels with a bus probe
between each. These are the exact same writes `BoardOf::begin()`'s
port-init will later make.

**Result:** All 8 pre-fire writes ACKed (probe ACKed between each), but
**post-`board.begin()` still showed the chip silent**. So the 8 PCA
writes are *not* the wedge trigger.

This was the first major direction change: the wedge isn't caused by
writing to `0x70`.

### Hypothesis #3 (CONFIRMED): a different chip wedges the PCA

Now we knew the wedge happens somewhere between "end of the per-channel
pre-fire in initHardware" (chip OK) and "after `board.begin()` returns"
(chip silent). Things that happened between those two probes:

1. The 8 INA226 inits (still in `initHardware`).
2. `bringUpStorage()` (SD_MMC bring-up, different GPIO domain).
3. `board.begin()` → `BoardOf::begin()`:
   a. 11 × `MicroservoPort::begin()` (LEDC on ESP32).
   b. 8 × `Pca9685PwmPort::begin()` (the same writes pre-fire already
      ran; idempotent).
   c. 1 × `EspInputPort::begin()` (no-op until a mode is set).
   d. `BoardServer::begin()` → each policy's `begin()` (Audio, Storage,
      USB host, …).

**Test (build 84):** Added a "bisection trail" — five PCA probes:
`after-INA`, `end-initHardware`, `after-storage`, `after-board.begin`,
`post-init`.

**Result:**

```
[PCA] PCA9685 @ 0x70: 8/8 per-channel writes ACKed (no address-comparator wedge)
[PCA] bisection:  after-INA=FAIL  end-initHardware=FAIL  after-storage=FAIL
                  after-board.begin=FAIL  post-init=FAIL
```

The PCA died during the 8 INA inits. Everything later in setup() just
inherited the wedge. The trigger is in `initHardware()`'s INA loop.

### Hypothesis #4 (CONFIRMED): INA[0] (the clone @ 0x40) is the trigger

**Test (build 85):** Added a per-INA probe — `pcaAfterInaK[8]`, captured
immediately after each `ina[k].begin()` call.

**Result:**

```
[PCA] post-INA[k] probe:  ch1=FAIL ch2=FAIL ch3=FAIL ch4=FAIL ch5=FAIL
                          ch6=FAIL ch7=FAIL ch8=FAIL
```

The PCA is silent right after INA[0] — and INA[0] is the clone (in our
existing logs as `mfg=0x0001 die=0x0020`). All subsequent probes inherit
the wedge.

**Confirmation test (build 86):** Skipped `INA[0].begin()` entirely.

**Result:** Clean boot. No wedge anywhere. No recovery triggered.
`8/8 per-channel writes ACKed`, `after-INA=OK`, `post-init=OK`.

That nailed the trigger: the clone @ 0x40.

### Hypothesis #5 (CONFIRMED): destructive writes, not the ID reads

Now we needed to know **which** I²C operation by the INA226 driver wedges
the PCA, because the user's prior intent was to keep the driver lenient
toward clones. If the wedge is in the destructive writes
(`reset` / `setCalibration` / `configure`), we can detect the clone
via its IDs before any write and only refuse to drive non-canonical
chips. If the wedge is in the ID reads (`MFG_ID` / `DIE_ID`), we can't
detect the clone safely and have to skip the address entirely (which is
much more invasive).

**Test (build 87):** For `INA[0]`, did the bare-Wire MFG/DIE register
reads by hand (same wire pattern `INA226::begin()` uses, **no writes**),
then probed the PCA.

**Result:** Clean boot, all 8 per-channel writes ACK, all post-INA[k]
probes OK. The ID reads are safe.

### Fix (build 88)

`INA226::begin()` was changed from "always lenient, configure on every
ACK regardless of identity" to "gate the destructive writes on a
canonical-ID check":

```cpp
bool INA226::begin(TwoWire& wire, uint8_t address, ...) {
    _wire = &wire; _address = address; _available = false;

    if (!probe(wire, address)) return false;  // nothing here

    _bootMfgId = readRegister16(INA226Reg::MFG_ID);   // safe (read-only)
    _bootDieId = readRegister16(INA226Reg::DIE_ID);   // safe (read-only)
    _idMatches = (_bootMfgId == MANUFACTURER_ID) &&
                 (_bootDieId == DIE_ID);

    if (!_idMatches) {
        // Clone — refuse to issue destructive writes that would
        // corrupt other chips on the shared bus.  Bootmfg/die remain
        // readable via bootMfgId()/bootDieId()/isCanonical().
        return false;
    }

    _available = true;
    reset();
    SFX_DELAY_MS(1);
    setCalibration(shuntResistance_ohms, maxCurrent_A);
    configure(INA226Averaging::AVG_16, INA226ConvTime::CT_1100US,
              INA226ConvTime::CT_1100US, INA226Mode::SHUNT_BUS_CONTINUOUS);
    return true;
}
```

Result on build 88 (and confirmed on a second board, build 89):

```
[INFO] [I2C] Wire up: SDA=GP8 SCL=GP9 @ 400 kHz
[INFO] [PCA] PCA9685 @ 0x70: 1526 Hz MODE1=0x21 MODE2=0x04 PRESCALE=0x03
[INFO] [PCA] PCA9685 @ 0x70: 8/8 per-channel writes ACKed (no address-comparator wedge)
[INFO] [PCA] PCA9685 @ 0x70: post-board.begin() OK MODE1=0x21
[WARN] [INA] ch1 @ 0x40: NOT DRIVEN — non-canonical IDs mfg=0x0001 die=0x0020
              (expected 0x5449/0x2260, TI INA226). Clone detected — refusing
              to drive to protect other chips on the shared I²C bus.
[INFO] [INA] ch2..ch8 @ 0x41..0x4F: OK mfg=0x5449 die=0x2260 (TI INA226)
[WARN] [INA] 7/8 monitors up (1 clone skipped — replace to restore full V/I sense)
```

No `post-init probe failed`. No recovery path triggered. The PCA9685
stays alive throughout the entire boot.

## Behaviour change for other callers

`INA226::begin()` is now strict where it used to be lenient. Callers
that relied on "begin() returns true on any chip that ACKs at the
address" will see `false` for clones. Known callers:

- **HubFX** (the board this investigation was for) — already updated to
  surface the clone in the boot log via `bootMfgId()` / `bootDieId()`.
- **GearControl Pico** ([gearcontrol_pico.ino:680](../controllers/gearcontrol/pico/src/gearcontrol_pico.ino#L680)) —
  uses `ina226Available[i] = ina226[i].begin(Wire, cfg)`. With the new
  driver, a counterfeit at any of the GearControl INA addresses will
  flip `ina226Available[i]` to false and the chip won't be driven. The
  console log shows `INA226[i] (0xAA): NOT FOUND`. Behaviour change:
  before the fix, a counterfeit could have been driven and (in
  principle) corrupted other chips on the GearControl Pico's bus; now
  it's safely skipped.
- **`Ina226VoltageSensor` / `Ina226CurrentSensor` adapters** — both
  check `chip.isAvailable()`, so they correctly report unavailable for
  clones with no further changes.

## What lives where after the fix

- **Driver-level gate.** [ina226.cpp:14](../controllers/lib/sfx_peripherals/power/ina226.cpp#L14)
  — reads MFG/DIE before any write, bails on non-canonical. The comment
  block in that file is the cross-reference back to this document.
- **HubFX boot log.** [hubfx_esp32s3.ino:logHardwareStatus](../controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino) —
  surfaces non-canonical chips as `[INA] ch1 @ 0x40: NOT DRIVEN …`.
- **PCA9685 per-channel pre-fire instrumentation.**
  [hubfx_esp32s3.ino:initHardware](../controllers/hubfx/esp32s3/src/hubfx_esp32s3.ino) —
  kept as a regression detector. If the wedge ever returns we want a
  per-channel breadcrumb showing exactly when. The summary line `8/8
  per-channel writes ACKed (no address-comparator wedge)` is the
  passing-boot signature.
- **Recovery path.** Same file, still in place. Should never fire on
  this board with the fix, but stays as a defence-in-depth net for
  future hardware variants.
- **Memory.** [`hubfx-clone-ina-wedges-pca`](C:/Users/marti/.claude/projects/c--data-code-scalefx/memory/project_hubfx_clone_ina_wedge.md)
  — terse summary for cross-session recall.

## If the wedge ever returns

If a future build re-introduces the `post-init probe failed` warning on
HubFX, the bisection harness is no longer in the sketch but is easy to
re-add. The signatures to look for, in order:

1. `[PCA] PCA9685 @ 0x70: 8/8 per-channel writes ACKed` line **missing**
   → the 8 `setChannel(k, 0)` writes themselves regressed; check the
   PCA9685 driver.
2. The summary line shows `address comparator WEDGED after pre-fire
   setChannel(N, 0)` → write N is the trigger; treat the PCA9685
   driver / Wire-bus state at that index.
3. Pre-fire passes but `[PCA] WENT SILENT after board.begin()` →
   something between `initHardware()` end and `logHardwareStatus()`
   start wedged the chip. Re-add the `pcaAfterIna` / `pcaAfterInaK[8]`
   probes from build 85 to bisect through the INA loop, the storage
   bring-up, and the policy `begin()` pack.

The bisection breadcrumbs were removed from the sketch after the fix
landed to keep the production boot clean; the instrumentation pattern
is preserved in this document.

## Re-interpretation (2026-07-02) — it was never a clone

While pinning down the **rev B** (pcb-nextver) Telesis netlist, the PCA9685's
address strapping finally got derived from first principles: the HVQFN-28's
A0..A5 pins (26, 27, 28, 1, 2, 21 under the confirmed package rotation —
SCL=23 / SDA=24 / VDD=25 / VSS=11 / LED0-7=3-10 all match) are **all
grounded**. All-A-low puts the PCA9685's hardware address at `1000000b` =
**0x40**. The `0x70` the firmware has always used is the chip's **all-call
alias** (ALLCALLADR `0xE0`>>1, MODE1 ALLCALL default-enabled) — nobody ever
had a reason to ask what the real address was.

That means 0x40 hosted **two chips** — the (genuine) INA226 U43 *and* the
PCA9685 — and re-explains every observation in this file:

| Original observation | Actual mechanism |
|---|---|
| "Clone" at 0x40 with impossible IDs `mfg=0x0001 die=0x0020`, identical on **every** board | Two chips answering a read at once — SDA wire-ANDs both bit streams into garbage. Deterministic garbage, hence the identical "fingerprint" across boards (looked like a batch-level substitution). |
| Writing CONFIG `0x8000` to "the clone" wedges the PCA at 0x70 | The write **is** to the PCA — 0x40 is its hardware address. INA register 0x00 (CONFIG) aliases PCA register 0x00 (**MODE1**); config bytes flip MODE1's sticky EXTCLK bit (PWM dead until SWRST) and/or clear ALLCALL (chip vanishes from 0x70). |
| Gen-call SWRST recovers it | SWRST is precisely the EXTCLK/MODE1 antidote. |
| Reads of 0xFE/0xFF are "safe" | Reads corrupt data, never state — open-drain contention is electrically harmless. |

**Bench confirmation (rev B, 2026-07-02, `tests/hw/i2c_probe`):** scan shows
`0x40` ACKing with non-canonical IDs (the collision), `0x41` a canonical TI
INA226 (U44 battery monitor), `0x4C` TAS5825P, `0x70` PCA all-call — on a
board whose netlist proves both chips strap to 0x40. Same signature as
rev A's "clone".

**Consequences:**
- The canonical-ID gate in `INA226::begin()` remains the correct and
  load-bearing defence — it is what guarantees no write ever reaches 0x40.
- Rev B firmware drives only U44 (`kInaAddrs = {0x41}`, battery telemetry);
  U43 (expander-rail monitor) is dead silicon until the address collision is
  fixed.
- **REV C hardware fix:** strap the PCA9685 out of the INA address range
  (e.g. A2 → 3V3 = 0x44) and/or restrap U43 (A0 → SDA = 0x42). Rev B rework
  alternative: lift U43 pin 2 (A0), jumper to SDA → 0x42.
- The rev A "replace U43 with a genuine TI part" advice (PINOUT.md, 99-HW-TODO)
  is moot — replacing the chip can't fix an address collision.
