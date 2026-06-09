# 24 — Crash Debugging with ESP32 Coredumps

> **Status:** debugging gotcha &middot; **Read when:** the HubFX firmware panics / reboots and you need the crashed task's backtrace.
> **TL;DR:** A HubFX panic writes an ELF coredump to flash; pull + decode it with `scalefx-flash coredump hubfx` (the ELF must match the FLASHED build, so pull BEFORE reflashing) — the flash coredump is the panic-debug path since the console is NONE.

When the HubFX firmware panics (Guru Meditation / `LoadProhibited` / stack
overflow / TWDT), it doesn't just reboot silently: ESP-IDF writes a full **ELF
coredump** to a dedicated flash partition. That coredump has the crashed task's
backtrace, every task's stack, and the register dump — the fastest way to find
*where* a firmware crash happened, without having to reproduce it under a
debugger.

This is configured in [controllers/hubfx/esp32s3/sdkconfig.defaults](../controllers/hubfx/esp32s3/sdkconfig.defaults):

```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECK_BOOT=y
```

and a `coredump` partition in [partitions.csv](../controllers/hubfx/esp32s3/partitions.csv)
(`0x7F0000`, 64 KB). The console is **USB-Serial-JTAG**, so live panic text goes
to the ESP32-S3's *native* USB port (not the CH343/UART0 the 6 Mbps protocol
uses) — but you rarely need that, because the coredump persists in flash across
the reboot.

## One command (recommended)

```bash
app/go/scalefx-flash.exe coredump hubfx [--port COMx]
```

It reads the coredump partition with esptool and decodes it against the built
firmware ELF with `espcoredump` + xtensa gdb, printing the crash backtrace.
Flags:

- `--port COMx` — serial port (auto-detects the CH343 otherwise).
- `--save FILE` — also keep the raw `.bin`.
- `--raw` — skip decode; just save the raw coredump + print the manual command.

**The ELF must match the FLASHED build.** espcoredump validates this
(`coredump SHA256 != app SHA256` → it refuses, correctly). So pull the coredump
**before** rebuilding/reflashing, or the backtrace is meaningless. If you've
already reflashed, the stored coredump predates it — reproduce on the current
build, then pull.

A board that hasn't panicked since its partition was erased reports
`No coredump stored`.

## Manual method (if the tools move)

```bash
# 1. read the partition
esptool --chip esp32s3 -p COMx read-flash 0x7F0000 0x10000 coredump.bin
# 2. decode against the built ELF
espcoredump.py --chip esp32s3 info_corefile --gdb <xtensa-esp32s3-elf-gdb> \
    -c coredump.bin -t raw \
    controllers/hubfx/esp32s3/.pio/build/esp32s3/firmware.elf
```

PlatformIO ships all three (python, `espcoredump.py` under
`framework-espidf/components/espcoredump/`, gdb under `tool-xtensa-esp-elf-gdb`)
after any firmware build — the `coredump` command just locates them under
`~/.platformio`.

## Worked example — the rapid-toggle audio crash (2026-05-31)

Rapid engine on/off toggling eventually reset the board. The coredump pinned it
instantly:

```
exccause 0x1c (LoadProhibitedCause), excvaddr 0x0   ← NULL / freed-pointer deref
#0 refillDrainBuffer  audio_mixer.ipp:987   (decoder task)
#1 decoderTaskFunc    audio_mixer.ipp:2193
```

**Root cause** — a cross-task use-after-free (Rule 15). `WavState::source` is a
raw `IAudioSource*` shared between the **decoder task** (Core 0, reads it in
`refillDrainBuffer`) and the **producer/command task** (which frees it in
`play()`/`stop()`/EOF via `destroyAudioSource`). `refillDrainBuffer`'s entry
guard `if(!ws.source)` was **TOCTOU**: the decode loop runs for ~ms, and a
concurrent teardown freed the source *mid-loop* → the next
`ws.source->readFrames()` dereferenced freed memory. The "set `active=false`
before destroy" comment only protected the decoder's *next* pass, not one
already inside the refill.

**Fix** — a seq_cst busy-flag handshake (`WavState::decoderBusy` +
`destroyChannelSourceSafe()`): the decoder claims the channel before refilling
and re-checks `active` under the claim; teardown clears `active` (seq_cst) then
waits (bounded) for `decoderBusy` to clear before freeing. Peterson-style mutual
exclusion — either the decoder skips, or teardown waits; never a free-during-
read. Verified with [tests/host/go_integration/engine_stress](../tests/host/go_integration/engine_stress)
(650 rapid start/stop cycles, 0 crashes).

**Lesson:** any raw pointer shared between the audio decoder and producer/command
tasks is a Rule 15 hazard. Tear down sources only through
`destroyChannelSourceSafe()`, never raw `destroyAudioSource(ws.source)`.
