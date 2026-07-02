# HubFX Hardware Pin Map

Authoritative pin assignment for the HubFX PCB. The firmware selects the
map at compile time via `-DHUBFX_PCB_REV` (platformio.ini):

- **Rev B (2026-07, `HUBFX_PCB_REV=2`, DEFAULT)** — summarized in the next
  section. Netlist: `hardware/pcb-nextver/exports/Netlist_Schematic1_2_2026-07-01.tel`.
- **Rev A (2025, `HUBFX_PCB_REV=1`)** — the original 8-channel rev; the
  remainder of this document describes it in full. Netlist:
  `Netlist_Schematic1_2026-05-14.tel`.

Both are derived from the EasyEDA Telesis netlist cross-referenced against
the Espressif ESP32-S3 chip-level QFN-56 pin numbering. Update this file in
the same commit as any board re-spin.

## Rev B (2026-07) — deltas vs rev A

Unchanged: I²C (SDA=GPIO8 / SCL=GPIO9), I²S (DOUT=16 / BCLK=17 / LRCLK=18),
SD 4-bit SDIO (CMD=38 CLK=39 D0=40 D1=41 D2=42 D3=45), USB-OTG host
(D-=19 / D+=20 → U41 hub), UART0 console/COBS wire (TX=43 / RX=44 → CH343),
PCA9685 @ 0x70, TAS5825P @ 0x4C.

| Function | Rev A | Rev B |
|---|---|---|
| Status LED | GPIO48 | **GPIO46** (`GREEN_LED`, via R21) |
| RC input IN_1 | GPIO5, single-wire | **INP header: TX=GPIO1 on the line, RX=GPIO2 via 2.2 kΩ (R8)** |
| Telemetry IN_2 | GPIO6, single-wire | **TELEM header: TX=GPIO3 on the line, RX=GPIO21 via 2.2 kΩ (R4)** |
| Servo headers ×10 | IN_3..IN_12 = GPIO {7,10,11,12,13,14,15,4,3,2}, 5 V rail | **SRV1..SRV10 = GPIO {5,6,7,10,11,12,13,14,15,4}, +6 V rail** |
| INA226 monitors | 8 × per-PWM-channel (0x40–0x45, 0x4A, 0x4F), 0.1 Ω high-side | **2 × rail monitors, 10 mΩ low-side (see below)** |
| PWM outputs | PCA CH1..8 → connectors CH1..CH8 | PCA CH1..8 → connectors **CH1..CH6, CH9, CH10** (+8 V rail) |
| Speaker outputs | dedicated amp stage | **connectors CH7/CH8** = TAS5825P OUT_A/OUT_B through the class-D LC filter — NOT PWM channels |

**Split TX/RX input topology.** Each input header's signal pin sits
DIRECTLY on the TX GPIO and reaches the RX GPIO through a 2.2 kΩ series
bridge, so RX listens continuously while TX idles high-Z and drives only
its half-duplex reply slot (`EspInputPort` constructor takes the optional
`txPin`; `txEnable()`/`txDisable()` gate the slot). The headers' center
(power) pins are NOT connected — the RC receiver is powered elsewhere.

**Rev B INA226 pair** (strapping from the netlist; bench-confirmed
2026-07-02 with `tests/hw/i2c_probe`):

| Ref | Addr (A1/A0) | Shunt | Measures | Usable? |
|---|---|---|---|---|
| U43 | 0x40 (GND/GND) | R83 10 mΩ, low-side in GND_1 | expander/USB rail return current (USB1+USB4 grounds); VBUS pin reads VBAT | **NO — address collision, see below** |
| U44 | 0x41 (GND/VS) | R9 10 mΩ, low-side at the BAT− terminal | total battery current; VBUS pin reads VBAT | yes — canonical TI IDs confirmed |

Both read **battery voltage** on the bus-voltage channel and positive
current under load. Confirmed rev B I²C scan: 0x40 (collision — ACKs,
garbage IDs), 0x41 (INA226 ✓), 0x4C (TAS5825P), 0x70 (PCA9685 all-call).

### 🔴 Rev B design bug — I²C address collision at 0x40 (fix in REV C)

The PCA9685 (U54) has **all six address pins grounded** (HVQFN pins 26,
27, 28, 1, 2, 21 = A0..A5, every one on GND), so its **hardware address
is 0x40** — the same address U43's strapping selects. The firmware's
`0x70` is only the PCA's default-enabled **all-call alias**, which is why
the collision went unnoticed. Consequences:

- Reads at 0x40 wire-AND both chips' replies → garbage (this is the real
  mechanism behind rev A's "counterfeit INA @ 0x40, `mfg=0x0001
  die=0x0020`" — see the re-interpretation in
  [instructions/18](../../../instructions/18-HUBFX-INA-CLONE-WEDGE.md)).
- Writes at 0x40 land in **PCA MODE1** (register 0x00 aliases the INA
  config register): a byte with bit 6 set flips the sticky EXTCLK bit →
  PWM dead until SWRST (the historic "wedge"); clearing bit 0 kills the
  ALLCALL alias → the PCA "vanishes" from 0x70.
- Harmless as long as nothing writes 0x40 — the `INA226::begin()`
  canonical-ID gate guarantees that, and rev B firmware drives only U44
  (`kInaAddrs = {0x41}`, battery telemetry only).

**REV C fixes:** strap the PCA9685 off the INA range (e.g. A2 → 3V3 =
0x44) and/or U43 A0 → SDA (= 0x42); then re-enable the expander-rail
monitor in `kInaAddrs`. Board-level rework on a rev B: lift U43 pin 2
(A0) and jumper to SDA → 0x42.

⚠️ **Rev B USB power hazard:** the `USB1`/`USB4` USB-C connectors carry
**raw VBAT on their VBUS pins** (expander power). Never plug a PC or any
5 V USB source into them — battery shorts into the host. Only `USB0`
(CH343) is a PC-safe port.

---

# Rev A (2025) — full reference

## Component summary

| Ref | Part | Package | I²C addr | Function |
|-----|------|---------|----------|----------|
| U26 | ESP32-S3 (bare die) | QFN-56 | — | Main MCU |
| U27 | SPI flash (16 MB) | SOIC-8 | — | External boot flash |
| U29 | CH343 USB-UART bridge | TQFN-16 | — | UART0 ↔ host PC (115200 by default) |
| U33 | (DC-DC / power IC) | PSO-8 | — | Boost / regulator |
| U34 | TAS audio / amp | HTSSOP-32 | — | Stereo amp output stage |
| U35 | (mixer / audio source) | TSSOP-20 | — | Audio source / input |
| U41 | USB hub controller | QFN-16 | — | Multi-port USB hub for slaves |
| U43–U48, U52, U53 | INA226 | MSOP-10 | 0x40–0x45, 0x4A, 0x4F | 8 × per-channel rail current/voltage monitor |
| U50 | (DC-DC / power) | PSO-8 | — | Boost / regulator |
| U54 | PCA9685BS | QFN-28 | 0x70 | 16-ch PWM driver (8 ch used → LED rails) |
| U55 | TAS5825P | VQFN-32 | 0x4C | Class-H + Hybrid-Pro stereo Class-D codec |

GPIO expander note: previous HubFX revisions used an AW9523B (0x58) for
local LED dim + codec PDN/MUTE control. **This rev does not populate
AW9523B.** Codec PDN/MUTE are statically pulled HIGH via 10 kΩ
resistors; LED channels are driven through PCA9685 (U54) PWM outputs
into MOSFET gates.

## ESP32-S3 (U26) full pin map

The bare ESP32-S3 in QFN-56 numbers pins around the package perimeter.
The mapping below is anchored to the netlist (`BOOT` → pin 5 = GPIO0,
`SDA` → pin 13 = GPIO8, `SCL` → pin 14 = GPIO9, SPI-flash group on
pins 30–35 = GPIO27–32, USB D±/EN at known pins). The GPIO sequence
runs linearly from pin 5 onward except where a fixed-function or power
pin breaks it: pin 20 (VDD33), pins 28–29 (VDDA/VDD_SPI), pin 46
(VDD33), pins 53–54 (XTAL2/1), pins 55–56 (VDD33).

| QFN pin | GPIO | Net (this PCB) | Function |
|---:|:---:|---|---|
| 1 | — | LNA_IN | RF antenna input (U5 SoC RF) |
| 2 | — | VDD3P3 | Analog supply |
| 3 | — | VDD3P3 | Analog supply |
| 4 | — | ESP_EN | CHIP_PU reset |
| 5 | GPIO0 | BOOT | Boot strap / button |
| 6 | GPIO1 | *(unconnected)* | Reserved — was I²S DOUT on prev rev |
| 7 | GPIO2 | IN_12 | RC input header 12 |
| 8 | GPIO3 | IN_11 | RC input header 11 (was I²S LRCLK on prev rev) |
| 9 | GPIO4 | IN_10 | RC input header 10 (was I²S BCLK on prev rev) |
| 10 | GPIO5 | IN_1 | RC input header 1 (I1) |
| 11 | GPIO6 | IN_2 | RC input header 2 (I2) |
| 12 | GPIO7 | IN_3 | RC input header 3 (I3) |
| 13 | GPIO8 | SDA | I²C bus data |
| 14 | GPIO9 | SCL | I²C bus clock |
| 15 | GPIO10 | IN_4 | RC input header 4 (I4) |
| 16 | GPIO11 | IN_5 | RC input header 5 (I5) |
| 17 | GPIO12 | IN_6 | RC input header 6 (I6) |
| 18 | GPIO13 | IN_7 | RC input header 7 (I7) |
| 19 | GPIO14 | IN_8 | RC input header 8 (unused in firmware) |
| 20 | — | VDD33 | Digital supply |
| 21 | GPIO15 | IN_9 | RC input header 9 (unused in firmware) |
| **22** | **GPIO16** | **TAS_DI** | **I²S DOUT → codec DIN** |
| **23** | **GPIO17** | **TAS_BCK** | **I²S BCLK → codec BCK** |
| **24** | **GPIO18** | **TAS_FS** | **I²S LRCLK → codec FS** |
| 25 | GPIO19 | (via R19) USB_D- | USB OTG host port — to U41 hub D- |
| 26 | GPIO20 | (via R20) USB_D+ | USB OTG host port — to U41 hub D+ |
| 27 | GPIO21 | *(unconnected)* | Available |
| 28 | — | *(VDDA / internal)* | Analog/RTC supply |
| 29 | — | VDD_SPI | SPI flash supply |
| 30 | GPIO27 | SPIHD | External flash (U27) |
| 31 | GPIO28 | SPIWP | External flash (U27) |
| 32 | GPIO29 | SPICS0 | External flash (U27) |
| 33 | GPIO30 | SPICLK | External flash (U27) |
| 34 | GPIO31 | SPIQ | External flash (U27) |
| 35 | GPIO32 | SPID | External flash (U27) |
| 36 | GPIO33 | *(unconnected)* | Available |
| 37 | GPIO34 | *(unconnected)* | Available |
| 38 | GPIO35 | *(unconnected)* | Available |
| 39 | GPIO36 | *(unconnected)* | Available |
| 40 | GPIO37 | *(unconnected)* | Available |
| **41** | **GPIO38** | **SD_DCMD** | **SD_MMC command** |
| **42** | **GPIO39** | **SD_CLK** | **SD_MMC clock** |
| **43** | **GPIO40** | **SD_DATA0** | **SD_MMC D0** |
| **44** | **GPIO41** | **SD_DATA1** | **SD_MMC D1** |
| **45** | **GPIO42** | **SD_DATA2** | **SD_MMC D2** |
| 46 | — | VDD33 | Digital supply |
| 47 | GPIO43 | U0_TX | UART0 TX → CH343 (U29) |
| 48 | GPIO44 | U0_RX | UART0 RX ← CH343 (U29) |
| **49** | **GPIO45** | **SD_DATA3** | **SD_MMC D3** |
| 50 | GPIO46 | *(unconnected)* | Available |
| 51 | GPIO47 | *(unconnected)* | Available |
| 52 | GPIO48 | GREEN_LED | Status / heartbeat LED |
| 53 | — | XTAL2 | 40 MHz crystal |
| 54 | — | XTAL1 | 40 MHz crystal |
| 55 | — | VDD33 | Digital supply |
| 56 | — | VDD33 | Digital supply |
| (pad) | — | GND | Exposed thermal pad |

**Bold rows mark assignments that differ from the previous HubFX rev**
and from the production firmware as it stood at FIRMWARE_VERSION 1.1.2.

## I²C bus map (SDA = GPIO8, SCL = GPIO9, 100 kHz)

The runtime scan should find exactly these ten devices:

| Addr | Device | Function |
|-----:|--------|----------|
| 0x40 | INA226 ch 1 (U43) ⚠️ | Channel-1 LED-rail current / voltage — **counterfeit on shipped boards** (see below) |
| 0x41 | INA226 ch 2 (U44) | Channel-2 rail |
| 0x42 | INA226 ch 3 (U45) | Channel-3 rail |
| 0x43 | INA226 ch 4 (U46) | Channel-4 rail |
| 0x44 | INA226 ch 5 (U47) | Channel-5 rail |
| 0x45 | INA226 ch 6 (U48) | Channel-6 rail |
| 0x4A | INA226 ch 7 (U52) | Channel-7 rail |
| 0x4C | TAS5825P (U55) | Audio codec — ADDR pin to GND |
| 0x4F | INA226 ch 8 (U53) | Channel-8 rail |
| 0x70 | PCA9685 (U54) | 16-ch PWM, channels 0–7 drive LED MOSFET gates |

Battery monitoring uses INA226 channel 0 (0x40) — the rail-input
shunt. The other seven monitor per-channel LED current. The
firmware-level `BATTERY_INA226_CHANNEL` constant must stay 0.

> **⚠️ U43 (INA226 @ 0x40) is a counterfeit on every shipped 8-channel
> rev board checked so far.** Reports `mfg=0x0001 die=0x0020` instead of
> the canonical TI `0x5449 / 0x2260`. Writing to it corrupts the PCA9685
> @ 0x70 on the shared bus, so `INA226::begin()` refuses to drive it
> and channel-1 V/I sense reads zero. Replace U43 with a genuine TI
> INA226 to restore the channel. Full investigation in
> [instructions/18-HUBFX-INA-CLONE-WEDGE.md](../../../instructions/18-HUBFX-INA-CLONE-WEDGE.md).
> Tracked in [instructions/99-HW-TODO.md](../../../instructions/99-HW-TODO.md).

## LED channel signal chain

Hub-side LED outputs go via the **PCA9685 (U54)** PWM driver, not via
the legacy AW9523B. Each PCA9685 output drives a MOSFET gate that
switches the corresponding LED rail. Per-channel current is measured
by a dedicated INA226.

```
ESP32-S3 ──I²C 0x70──> PCA9685 LED0..LED7 ──MOSFET gate──> CH1..CH8 LED+
                                                            │
                                            shunt R83..R88, R109, R113
                                                            │
                                  CH1..CH8 LED- ──> INA226 V+/V- (0x40..0x45, 0x4A, 0x4F)
                                                            │
                                                  I²C readback ──> ESP32-S3
```

PCA9685 → channel pin map (per netlist `CH1..CH8` nets):

| PCA9685 output | CH | INA226 addr | INA226 ref |
|:--:|:--:|:--:|:--:|
| LED0 (pin 3) | CH1 | 0x40 | U43 |
| LED1 (pin 4) | CH2 | 0x41 | U44 |
| LED2 (pin 5) | CH3 | 0x42 | U45 |
| LED3 (pin 6) | CH4 | 0x43 | U46 |
| LED4 (pin 7) | CH5 | 0x44 | U47 |
| LED5 (pin 8) | CH6 | 0x45 | U48 |
| LED6 (pin 9) | CH7 | 0x4A | U52 |
| LED7 (pin 10) | CH8 | 0x4F | U53 |

PCA9685 frequency / duty resolution is 12-bit, default ~200 Hz; for
LED dimming the firmware should set ~430 Hz (matches the old AW9523B
PWM rate well enough to avoid visible flicker).

## Audio signal chain

```
ESP32-S3 ──I²S─> TAS5825P (U55) ──BTL pairs──> U34 (HTSSOP-32 amp / passive)
                       │                              │
                       └── I²C 0x4C ctrl              └── speaker outputs
```

I²S pins: **DOUT = GPIO16, BCLK = GPIO17, LRCLK = GPIO18**.
TAS5825P codec PDN and MUTE are statically pulled HIGH on this rev via
10 kΩ resistors — no GPIO drives them. State control is via
DEVICE_CTRL2 (0x03) over I²C. The codec's nFAULT pin (`TAS_FAULT`) is
also passive-pulled (R52) and not routed back to the ESP32 — fault
status is read via the codec's I²C fault registers.

The amp output side (U34) consumes I²S clock domain (`SCK`, `LRCK`,
`DIN` nets attached to R36/R37/R39/R38) — those are private to the
amp stage and not driven by the ESP32.

## RC input mapping

| Header | Net | GPIO | Used by firmware? |
|---|---|---|---|
| IN_1  | `IN_1`  | GPIO5  | yes — I1 |
| IN_2  | `IN_2`  | GPIO6  | yes — I2 |
| IN_3  | `IN_3`  | GPIO7  | yes — I3 |
| IN_4  | `IN_4`  | GPIO10 | yes — I4 |
| IN_5  | `IN_5`  | GPIO11 | yes — I5 |
| IN_6  | `IN_6`  | GPIO12 | yes — I6 |
| IN_7  | `IN_7`  | GPIO13 | yes — I7 |
| IN_8  | `IN_8`  | GPIO14 | no — header present but firmware caps at 7 |
| IN_9  | `IN_9`  | GPIO15 | no |
| IN_10 | `IN_10` | GPIO4  | no |
| IN_11 | `IN_11` | GPIO3  | no |
| IN_12 | `IN_12` | GPIO2  | no |

`InputDispatcher::PINS` in
[input_dispatcher.h:42](src/inputs/input_dispatcher.h#L42) already
matches IN_1..IN_7. Adding IN_8..IN_12 support is a future change.

## SD card (SDIO 4-bit)

```
PIN_SD_MMC_CMD  = GPIO38
PIN_SD_MMC_CLK  = GPIO39
PIN_SD_MMC_D0   = GPIO40
PIN_SD_MMC_D1   = GPIO41
PIN_SD_MMC_D2   = GPIO42
PIN_SD_MMC_D3   = GPIO45
```

The ESP32-S3 SDMMC slot 1 is routable through the GPIO matrix on this
chip, so any GPIO set works — the PCB drove the change.

## UART / USB / status

| Function | GPIO | Notes |
|---|---|---|
| UART0 TX (console) | GPIO43 | → CH343 USB-UART bridge (U29) → USB-C |
| UART0 RX (console) | GPIO44 | ← CH343 |
| USB-OTG D- | GPIO19 | Hub controller (U41) downstream port |
| USB-OTG D+ | GPIO20 | Hub controller (U41) downstream port |
| Status LED  | GPIO48 | Heartbeat / connection state (active-high) |

The ESP32-S3 acts as a USB host (OTG) and feeds the U41 hub controller,
which fans out to the slave controllers. The CH343 on USB-C is the
console / programming UART — flashed firmware logs over `UART0` at
115200 baud.

## PCA9685 bring-up reference

The clean init sequence for the PCA9685 — including the latch-up
recovery procedure, the SWRST-via-general-call trick, the
`Wire.begin(sda,scl,freq)` vs `setClock()` gotcha, and the
flicker-free PWM frequency choice — is documented in the test
fixture's README:

[`tests/hw/hubfx_pca9685_hwtest/README.md`](../../../tests/hw/hubfx_pca9685_hwtest/README.md)

That document is the source of truth for what the production driver
needs to do.

## Deltas vs production firmware (FIRMWARE_VERSION 1.1.2)

The production firmware was written for the previous HubFX rev. Items
this PINOUT.md changes:

- ✅ **I²S pins** — DOUT/BCLK/LRCLK move from GPIO1/4/3 → GPIO16/17/18.
  Validated against the `sfx_test_p` bring-up firmware (codec locks at
  192 kHz FS_MON readout, PLAY confirmed).
- ✅ **SD card pins** — CMD/CLK/D0–D3 shift to GPIO40/41/42/43/44/47.
  Not yet runtime-validated on this rev; pin numbers derived directly
  from the netlist's `SD_DCMD`, `SD_CLK`, `SD_DATA0..3` nets.
- ⚠️ **GPIO expander driver** — board no longer has AW9523B @ 0x58.
  The PCA9685 (0x70) replaces it for LED channel control. A new
  `controllers/lib/sfx_peripherals/pwm/pca9685.h` driver is needed
  before LED output works on this rev. Until then `gpioExpander.begin()`
  will fail at I²C probe and the firmware skips LED output (current
  behaviour — see `initI2CDevices`).
- ⚠️ **INA226 count** — board has 8 monitors (one per LED channel)
  at addresses 0x40–0x45, 0x4A, 0x4F. Firmware currently initialises
  only the first 6. Expanding to 8 requires extending the broadcast
  `BOARD_STATUS` payload — Rule 11 (append-only). Battery channel
  (`BATTERY_INA226_CHANNEL = 0` at 0x40) does not change.

## Verification recipe

After any pin-related firmware change, re-run the `sfx_test_p` bring-up
fixture and confirm in its periodic diagnostic:

```
==== TAS5825P DIAGNOSTICS ====
  DEVICE_CTRL  (0x03): 0x03 → PLAY
  POWER_STATE  (0x68): 0x03 → PLAY
  FS_MON       (0x37): 0x?? → 48kHz (or 192kHz — codec reports BCLK÷64)
  GLOBAL_FAULT1(0x71): 0x00      ← critical: bit 2 (clock fault) must be clear
```

And on the host:

```
$ ./app/go/scalefx-flash.exe ports
ℹ Detected ScaleFX serial ports (1):
  COM??       ESP32   VID:1A86 PID:55D3
```

(VID:1A86 = WCH CH343, the U29 USB-UART bridge.)
