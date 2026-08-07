# tas5825m_beep — TAS5825M codec bring-up probe

Standalone pure-ESP-IDF sketch that brings up the TAS5825**M** amplifier
(the smart-amp variant — bench boards turned out to carry M silicon
despite the BOM's P) and plays a 1 kHz beep, 200 ms on / 800 ms off,
with every step instrumented on the serial console.

No ScaleFX libraries — the point is to bisect board problems independent
of the production firmware. Every register address and bit is taken from
the **TI TAS5825M datasheet SLASEH7H** (the gold standard; the library
mirror is
[tas5825_regs.h](../../../controllers/lib/sfx_audio/codec/tas5825_regs.h)).
The init sequence is the M-strict flow of
[tas5825_m_codec.cpp](../../../controllers/lib/sfx_audio/codec/tas5825_m_codec.cpp):
DIS_DSP held until clocks are proven, FS_MON gate before HIZ→PLAY,
GPIO1→FAULTZ (0x62=0x0B + output enable), triple FAULT_CLEAR.

## Build + flash

```bash
cd tests/hw/tas5825m_beep
pio run -e esp32s3 -t upload --upload-port COMxx
pio device monitor -p COMxx -b 115200
```

Console is UART0 at 115200, plain text (no COBS). Uses the production
partition table, so LittleFS configs survive. Reflash the real firmware
afterwards: `app/go/scalefx-flash.exe flash hubfx`.

## What it does

1. I2C bus scan (SDA=8 SCL=9) — expect `0x4C` (codec), plus `0x40`/`0x70`
   (PCA9685) and `0x41` (INA226) on a HubFX.
2. **DIE_ID identity check** — reg 0x67 reads `0x95` on genuine TAS5825M
   silicon; anything else is loudly flagged.
3. Phase 1 (pre-clock): park with the DSP held in reset
   (DEVICE_CTRL2 = DIS_DSP|MUTE|DEEP_SLEEP) → RESET_CTRL 0x11 →
   re-park → FAULT_CLEAR (boot clock-fault latch). Every write is read
   back and verified (`rb=0x.. OK` / `** MISMATCH **`).
4. Starts I2S TX (BCLK=17 LRCLK=18 DOUT=16, 48 kHz 16-bit stereo, no
   MCLK) and the beep writer task — zeros stream in the beep gaps so
   the clocks never stop (clock loss = fault + HiZ).
5. Phase 2: analog gain (AGAIN 0x54 = −8 dB for the 12 V rail), SAP
   word length 16-bit (0x33 — silicon default is 24!), GPIO1→FAULTZ
   (select + output enable), volume −20 dB, then **polls FS_MON** until
   the codec locks (expect **`0x09`** = 48 kHz per Table 9-19), then
   releases the DSP into HIZ → PLAY → post-PLAY UVP clear, and verifies
   POWER_STATE. On a failed lock it decodes CLKDET_STATUS (which of
   FS/SCLK/PLL is unhappy) + BCK_MON.
6. Heartbeat every 2 s: POWER_STATE / FS_MON / **live PVDD voltage**
   (PVDD_ADC 0x5E) / CLKDET_STATUS / GLOBAL_FAULT1+2 / CHAN_FAULT /
   OT warnings, all bit-decoded, + beep/error counters. If the codec
   falls out of PLAY it auto-recovers (FAULT_CLEAR + HIZ→PLAY) and
   logs the attempt.

## Interpreting the output

| Symptom | Points at |
|---|---|
| Bus scan shows no `0x4C` | I2C wiring, codec DVDD supply |
| `DIE_ID != 0x95` | Not TAS5825M silicon, or a flaky bus |
| Writes OK, `FS_MON NEVER LOCKED` | BCLK/LRCLK path (GPIO17/18 → codec); the CLKDET_STATUS decode names the unhappy clock |
| FS locks, `PLAY FAILED` + `PVDD_UV` decode | Amp supply rail sagging/absent — cross-check the heartbeat's PVDD voltage |
| FS locks, `PLAY FAILED` + `OTSD` decode | Over-temp shutdown — check for shorts on OUT_A/OUT_B |
| `L_OC`/`R_OC` in CHAN_FAULT | Output over-current — speaker/wiring short |
| `PLAY` but silent | Speaker wiring, scope OUT_A/OUT_B, DIG_VOL |
| Repeated `recovery #N` in heartbeat | Intermittent fault — read which fault bit re-latches |
