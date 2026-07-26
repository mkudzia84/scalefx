# HubFX I2C probe (noop bring-up firmware)

A minimal ESP32-S3 app that inits I2C on **GPIO8/9** (the HubFX I2C bus, same on
every rev), scans `0x08–0x77`, and identifies each chip — reading the **INA226
manufacturer/die ID** so counterfeits are flagged, not trusted. It brings up
**nothing else** (no PCA/codec/USB-host/audio), so it's safe on a fresh,
unproven board. Output goes over **UART0 (GPIO43/44 → CH343 → USB0) @ 115200**.

Use it to confirm the I2C device map (e.g. how many INA226s and at what
addresses) on a new PCB revision before touching the full firmware.

## 1. Plug in the right port
Connect the board's **USB0** connector (the CH343 programming/console port) —
**not** USB1/USB4, which are the USB-hub downstream ports for expanders and
don't present a COM port. Use a **data** USB-C cable and confirm the board's
power LED is on. A COM port should appear even on a blank ESP32-S3 (the CH343
enumerates independently).

## 2. Flash
```bash
cd tests/hw/i2c_probe
pio run -e i2cprobe -t upload --upload-port COMxx
```
(If auto-reset doesn't enter the bootloader on a blank board: hold **BOOT**, tap
**RST**, release **BOOT**, then run the upload.)

## 3. Read the scan
```bash
pio device monitor -e i2cprobe -p COMxx      # 115200
```
Confirmed rev B scan (bench, 2026-07-02):

```
0x40   ACKs, NOT a canonical INA226   ← U43 colliding with the PCA9685's
                                        HARDWARE address (all A-pins GND;
                                        0x70 is only its all-call alias)
0x41   INA226 mfg=0x5449 die=0x2260   ← U44 battery monitor, genuine TI
0x4C   TAS5825P
0x70   PCA9685 (all-call)
```

The firmware's rev B `kInaAddrs` is `{0x41}` (battery only) until the
0x40 collision is fixed in PCB rev C — see
[hardware/pcb-nextver/ISSUES.md](../../../hardware/pcb-nextver/ISSUES.md) §2.

## Restore the real firmware afterwards
```bash
app/go/scalefx-flash.exe flash hubfx
```
