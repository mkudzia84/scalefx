# HubFX rev B (pcb-nextver) — bring-up issues & fixes log

> **Status:** hardware findings log · **Read when:** reworking a rev B board,
> speccing REV C, or debugging a rev B power/I²C anomaly.
> **TL;DR:** one board-killing assembly fault (C2 shorted → smoke), one
> design bug carried since rev A (PCA9685 hardware address collides with
> U43 INA226 at 0x40), one unprotected power path (raw VBAT on USB1/USB4).
> Board is operational after removing C2; firmware runs battery-only INA
> telemetry until REV C.

Source of truth for the analysis: the Telesis netlist in
[exports/Netlist_Schematic1_2_2026-07-01.tel](exports/Netlist_Schematic1_2_2026-07-01.tel),
cross-referenced against the ESP32-S3 QFN-56, PCA9685 HVQFN-28 and INA226
MSOP-10 datasheet pinouts. Firmware pin map:
[PINOUT.md §Rev B](../../controllers/hubfx/esp32s3/PINOUT.md).

---

## 1. 🔴 FIXED — C2 (4.7 µF MLCC across VBAT) failed as a dead short

**Symptom.** First battery connect: smoke, battery hard-shorted
("shorted big time"). Board never enumerated USB0 even before the event.

**Cause.** C2 — a 4.7 µF 0805 ceramic wired directly across VBAT↔GND —
was internally shorted (classic cracked-MLCC failure: flex damage from
assembly/depaneling/connector stress makes the cap fail as a near-0 Ω
short). Every pack connect drove the full battery current through
D11/D13 → C2 → GND.

**Fix applied (2026-07-02).** C2 removed (pad lifted — left unpopulated).
Its twin C3 plus the rest of the VBAT bank (6 × 10 µF C164–C169,
2 × 470 µF C163/C170) provide ample decoupling; no firmware impact.
Board powers up and enumerates (CH343 on USB0) after removal.

**Post-fix verification.** D11/D13 (reverse-protection Schottkys that
carried the fault current): forward drop 0.2 V ✓, no reverse short ✓.

**REV C action.** Use soft-termination MLCCs (or tantalum) for caps
directly across the battery rail; keep them clear of board-flex zones.
Check C3 on every rev B board (same part, same region, same stress).

---

## 2. 🔴 DESIGN BUG — I²C address collision at 0x40 (PCA9685 vs U43 INA226)

**The bug.** The PCA9685 (U54) has **all six address pins grounded**
(HVQFN pins 26/27/28/1/2/21 = A0..A5 under the datasheet rotation
confirmed by SCL=23 / SDA=24 / VDD=25 / VSS=11 / LED0-7=3-10), making its
**hardware address 0x40**. U43 (INA226, A1=GND A0=GND) is **also 0x40**.
The firmware's `0x70` for the PCA is only its default-enabled **all-call
alias** — which is why the collision stayed invisible.

**Manifestation.**
- Reads at 0x40: both chips drive SDA → wire-ANDed garbage
  (the origin of rev A's "counterfeit INA, `mfg=0x0001 die=0x0020`" —
  full re-interpretation in
  [instructions/18](../../instructions/18-HUBFX-INA-CLONE-WEDGE.md)).
- Writes at 0x40: land in **both** chips — INA CONFIG (reg 0x00) aliases
  PCA **MODE1** (reg 0x00): a set bit 6 = sticky EXTCLK → PWM dead until
  SWRST (the historic "wedge"); a cleared bit 0 = ALLCALL off → PCA
  vanishes from 0x70.
- Electrically harmless (open-drain) — data/state corruption only.

**Bench confirmation (2026-07-02, `tests/hw/i2c_probe` on the recovered
board):**

```
0x40   ACKs — NOT a canonical INA226      ← the collision
0x41   INA226  mfg=0x5449 die=0x2260 ✓    ← U44 battery monitor, genuine TI
0x4C   TAS5825P
0x70   PCA9685 (all-call alias)
```

**Firmware state (v2.35.0).** `kInaAddrs = {0x41}` — battery telemetry
only (U44). U43 is never addressed; 0x40 is registered as an *expected*
scan address (it's the PCA) but never driven. The `INA226::begin()`
canonical-ID gate remains the load-bearing defence against accidental
writes to 0x40.

**REV C fix (required to use U43 / expander-rail telemetry):**
- Strap PCA9685 A2 → 3V3 (address becomes 0x44, out of the INA range),
  **and/or** strap U43 A0 → SDA (0x42).
- Rev B board rework alternative: lift U43 pin 2 (A0), jumper to SDA
  → 0x42; then firmware `kInaAddrs = {0x42, 0x41}`.

---

## 3. 🔴 DESIGN HAZARD — raw VBAT on USB1/USB4 VBUS, unprotected

**Intent (correct):** USB1/USB4 are the expander ports — VBAT feeds the
expander boards down the USB-C cable; ground returns through GND_1 →
R83 (10 mΩ) so U43 can meter the expanders' combined draw; data goes
through ESD protectors D7/D8 into U41 hub downstream ports 1/4, upstream
to the ESP32-S3 USB-OTG host (GPIO19/20).

**The gaps:**
1. **No fuse / current limit anywhere in the VBAT→VBUS path.** A shorted
   cable, connector debris, or failed expander pulls unlimited pack
   current until something burns.
2. **Misplug:** the ports are standard USB-C receptacles with no CC
   resistors — physically identical to USB0. A PC or 5 V charger plugged
   in meets the raw pack (12.6 V on 3S). Only USB0 (CH343, proper 5.1 kΩ
   CC pull-downs) is PC-safe.

**REV C action.** Per-port e-fuse (TPS2595x-class, ~2–3 A programmable
limit, FAULT flag routable to the ESP) — or minimum polyfuses + loud
silkscreen / keyed connectors. Verify the expander boards' VBUS input
stage tolerates the full pack range (12.6 V max on 3S).

---

## 4. 🟡 Minor observations (no action required to operate)

| # | Finding | Note |
|---|---|---|
| 4.1 | **GPIO45 (VDD_SPI strap) = SD DAT3** | Card's internal DAT3 pull-up vs ESP internal pull-down race at reset could strap VDD_SPI to 1.8 V → no boot with card inserted. Carried over from rev A (empirically works). Symptom to remember: "won't boot with SD card in". |
| 4.2 | **3V3 LDO (U23) fed from the +6 V servo rail** | Servo stall transients couple into logic supply; on USB-only power the servos can draw from the PC port through D4. |
| 4.3 | **INP / TELEM center (power) pins unconnected** | RC receiver must be powered elsewhere; only GND + signal on the input headers. |
| 4.4 | **CH7/CH8 connectors are SPEAKER outputs** | TAS5825P OUT_A/OUT_B through the class-D LC filter — not PWM. PWM channels land on connectors CH1–CH6 + CH9/CH10 (PCA outputs 0–7). |
| 4.5 | **Reverse-battery protection = 2 × parallel SMA Schottky (D11/D13)** | They carry full pack current; marginal at high draw. Orientation not verifiable from netlist — verified good on board #1 (0.2 V fwd, no reverse short). |
| 4.6 | **No CC resistors on USB1/USB4** | Fine for hard-wired ScaleFX expanders; nonconforming for generic Type-C sinks (folds into issue 3). |
| 4.7 | **470 µF electrolytics (C163/C170) polarity unverifiable from netlist** | Confirm against layout if a board smokes near the bulk caps. |

---

## 5. 🔴 FIXED — app-only reflash leaves a FOREIGN partition table (fresh-board "frozen"/empty Studio)

**Symptom.** Fresh-flashed rev B board: every config operation NACKed
`NOT_INITIALIZED`, Studio connected but rendered empty/frozen, config
seeding refused to run.

**Root cause chain (2026-07-02):**
1. `scalefx-flash flash` wrote ONLY the app (`write_flash 0x10000
   firmware.bin`) — never the partition table. The board had last been
   flashed with the `tests/hw/i2c_probe` firmware, whose DEFAULT
   PlatformIO partition table has no `littlefs` partition.
2. `esp_vfs_littlefs_register` → `ESP_ERR_NOT_FOUND (0x105)` → flash
   backend dead → every config load/save/seed `NOT_INITIALIZED`.
3. Studio additionally had an event-ordering race that could blank an
   already-loaded device model (empty `devicemodel:changed` broadcast
   landing after the populated snapshot), and the EngineFX validator
   flagged the DISABLED default draft as an error — red gate on a
   virgin board.

**Fixes (all landed 2026-07-02):**
- `scalefx-flash` now writes the **factory image at 0x0** (bootloader +
  partition table + app) whenever the build produced one — the
  partition table always matches the firmware; the LittleFS data region
  is beyond the image, so existing configs still survive reflashes.
- `FlashModule::begin()` retries through an explicit
  `esp_littlefs_format()` and records `lastBeginError()`; the boot log
  prints the error code; `FLASH_STATUS_REQ` now actually self-recovers
  (retries `begin()`) as bring_up.h always claimed.
- Studio: empty `devicemodel:changed` broadcasts are suppressed
  (Go) / ignored (frontend) while connected; a disabled effect no
  longer gates the global Apply.

Bench-verified end-to-end: factory flash → LittleFS self-formats →
`/hubfx.yaml` auto-seeded → Studio shows all 20 ports / 5 domains /
"in sync".

**The intermittent "UI locks on connect" (root mechanism, 2026-07-02):**
a component update THROWS during a Svelte flush in the connect cascade and
the invoking context swallows the error (svelte-hmr's dev proxy does) —
Svelte 3.59 then leaves `update_scheduled=true` with no flush pending, so
every component stops updating while timers/stores keep running (UI looks
frozen; the wizard modal not appearing was collateral of the same wedge,
not a wizard bug — it renders fine on healthy runs). Isolation test:
connect with FULL configs → never wedged; empty-config connects wedged
intermittently. Mitigation shipped: a **flush watchdog** in
`diag.ts` (1 s manual `flush()` — no-op when healthy, self-heals the wedge
within a second and logs the culprit's stack as `FE.FLUSH`). The
underlying throwing component gets fixed when `FE.FLUSH` first names it.

**Also fixed during this session:** ESP32 uploads into a not-yet-existing
directory (e.g. `/lightfx/programs/<n>.yaml` on a fresh board) failed with
`FILE_IO_ERROR` — POSIX `fopen` doesn't create parents (the Pico Arduino
wrapper does). `NativeFile::openWriteFile` now retries through an
`mkdir -p` of the parent chain (covers flash AND SD backends).

## Firmware/tooling changes made during this bring-up

- `HUBFX_PCB_REV` compile-time pin-map switch (rev B default; `=1`
  rebuilds for rev A) — `controllers/hubfx/esp32s3/src/hubfx_esp32s3.cpp`,
  v2.35.0.
- `EspInputPort` refactor: explicit `(rxPin, uartNum)` single-wire and
  `(rxPin, txPin, uartNum)` split TX/RX constructors for the rev B
  bridged INP/TELEM headers (2.2 kΩ TX→RX bridge, TX idles high-Z,
  slot-gated via `txEnable`/`txDisable`).
- `tests/hw/i2c_probe/` — minimal noop bring-up firmware: scans the bus,
  reads INA MFG/DIE IDs, prints over UART0@115200. Used for both the
  address confirmation and the collision demonstration.
- Probe (`hubfx_hw_probe.h`) parameterized by INA count + labels
  (rev B logs `[INA] battery @ 0x41`, "1/1 monitors up").
- Rail monitoring (undervoltage alert + Jeti V/I telemetry) still
  disabled pending bench validation of U44 readings under load.


## REV C reassessment — TELEM pull-up (2026-07-15)

The 4.7 kΩ pull-up on the TELEM line was recommended while diagnosing the
Jeti EX-Bus DOWNSTREAM mode (Kolibri as EX slave, replies never decoding).
That mode is now REMOVED from the firmware (HubFX 2.39.0), and the
diagnosis is retrospectively uncertain: the downstream path had no TX-echo
drain at all and the frame parser accepted any type byte, so the Kolibri's
replies may have been SWALLOWED by parser desync (the same mechanism as
the 2026-07-15 input-gap saga on INP) rather than electrically lost.
Native Kontronik telemetry (push-pull) runs error-free with no pull-up.

→ TELEM pull-up: downgraded from "required fix" to OPTIONAL robustness.
→ INP pull-up: still recommended (spec: the bus master owns the idle level).
