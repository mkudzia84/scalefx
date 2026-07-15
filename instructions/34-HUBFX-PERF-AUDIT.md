# HubFX Performance & Memory Audit — 2026-07-15 (overnight)

Requested scope: instrumentation cleanup (done), DRAM/PSRAM budget, stack and
member sizing, speed folds onto ESP-IDF/esp-dsp, memory alignment — across
`controllers/hubfx/esp32s3/` and the `controllers/lib/` code on the critical
path (input, telemetry, audio).  Bench context: v2.39.0 build 927 after the
input-gap saga (see RELEASES.md 2.39.0).

## 0. Done overnight (committed)

- **Mixer command-plumbing logs demoted to `MIXER_TRACE`** (compiled out by
  default, `-DAUDIO_LOG_TRACE=1` to re-enable): ~6 DiagLog lines per
  play/stop, each a ~2 KB-deep call on the emitting task.  Lifecycle
  one-shots and playback-state lines (`Playing/Stopped/Fading/Queued`)
  remain.  Sweep of `effects/` found no other per-event floods — the 2 s
  health lines (`[jexp]`, `[esctelem]`, `[mem]`, `[stack]`) are cadenced and
  stay; the `failed-frame` dump earns its keep (it cracked the input-gap
  case).
- NOT flashed (board was unplugged) — flash on next bench session.

## 1. DRAM budget (the real numbers)

Steady-state bench session: **25–34 KB free of 246 KB, largest block
16–24 KB; DMA-capable 18–26 KB free**.  Fresh boot: ~90 KB free.  The
session-time drop is (all pre-existing, identical on `main`):

| Consumer | Size | Pool | Lifetime |
|---|---|---|---|
| MP3 decoder pool — libhelix ctx | ~9 KB × channel | INTERNAL | lazy per channel, **never freed** (by design, `audio_mp3_decoder_pool.h`) |
| MP3 PCM scratch | 4.6 KB × channel | INTERNAL | with each slot |
| WAV SD read buffer | 32 KB | **DMA+INTERNAL** | first WAV streamed from SD (the 14 MB/s SD-SRAM fix) |
| Task stacks (jeti 6 K, usb 2×8 K, audio 4×4–8 K, loader 8 K, loopTask…) | ~50 KB | INTERNAL | static |
| UART driver rings (IN_1 1 K, ESC 4 K, +TX) | ~12 KB | INTERNAL | per attach |
| TelemetryHub singleton (6 dev × 16 sensors + msg) | ~4.5 KB | .bss INTERNAL | static |
| **DiagLog ring** | 68 KB (512 × 136 B) | **PSRAM** ✓ | `sfxPsramCalloc` in begin() |

The diag ring is **already in PSRAM** — no DRAM action needed; 512 entries is
a reasonable post-mortem depth (halving to 256 saves only PSRAM, which has
4.9 MB free — not worth it).

### DRAM opportunities, priority-ordered

1. **Cap the MP3 pool below `AUDIO_MAX_CHANNELS`** (already 6). Real
   concurrent MP3 use: engine ch1+ch2 (crossfade) + gun + alert ≈ 4.  A
   `kMp3PoolCap = 4` bounds worst case at 54 KB → **−27 KB**, zero risk
   beyond a 5th simultaneous MP3 gracefully failing to a skipped sound.
   *(Verify effect channel claims before setting: enginefx 2, gunfx ?,
   alerts ?, init 1-shot.)*
2. **TelemetryHub → PSRAM** (allocate `_devices` in `begin()` via
   `sfxPsramCalloc` instead of the static array): **−4.4 KB** .bss.  Access
   pattern is 2 Hz publishes + ~65 Hz reads of a few dozen bytes under a
   mutex — PSRAM latency is irrelevant here.  Low risk.
3. **MP3 PCM scratch → PSRAM** (`audio_mp3_decoder_pool.cpp:110`): written
   sequentially once per decoded frame, read once by the mixer —
   **−4.6 KB × slots**.  Medium risk: validate with the underrun counter
   during a dual-decode crossfade before adopting.  (The decoder *context*
   must stay INTERNAL — hot path, documented.)
4. **Idle-slot reclaim** (free a pool slot after ~60 s unused): only if 1–3
   prove insufficient; adds lifecycle complexity to a deliberately simple
   pool.
5. Task-stack trims: `[stack]` hwm shows headroom (loopTask 5.9 K free,
   producer 2.3–2.6 K, decoder/consumer ~1.2 K).  Decoder/consumer are
   already tight — **do not trim**; loopTask could give up 2 KB but it's the
   overflow-sensitive one historically.  Not recommended.

## 2. Members / stack-object audit (critical path)

- Hot-path stack frames are small and fine: `serveTelemetry` `buf[40]`,
  failed-frame `ff[64]`+`hex[129]` (2 s cadence only), decoder `_buf[24..48]`
  per role — all sub-200 B.
- `JetiExpander` additions from the saga: `_typeRank` 96 B, msg caches ~30 B
  — negligible.
- **Struct-packing wins (cosmetic, do opportunistically):**
  - `TelemetryHub::Sensor` (~40 B now): reorder to
    `value/lastMs` (4-aligned) first, then `label[21]+unit[6]+id+kind+decimals+active`
    → saves 2–3 B × 96 sensors ≈ 250 B.  Same idea for `Device` (`msg` last).
  - `TriggerMapping`/`TriggerValue`: already tight.
- `EscTelemData` (~64 B) lives inside the role variant — fine; do NOT return
  it by value anywhere hot (currently only referenced).

## 3. Speed folds (ESP-IDF / esp-dsp / instructions)

- **Mix kernel: already optimal** — `MixKernel::produceBlock` runs int16/Q15
  with **esp-dsp SIMD** (S3 PIE), float path also SIMD, dispatch via
  `if constexpr`.  No action.
- **MP3 decode**: libhelix is the fixed cost (~20-25 %/stream on Core 1 now).
  The only cheaper decode is *no* decode: pre-decoded PCM cache for short,
  frequent clips (gun/alert) — PSRAM budget allows it (43 % used).  Optional.
- **CRC folds** (cleanliness, not measurable speed — total CRC load is a few
  kB/s): `crc16_ccitt` (reflected 0x8408) ≡ `esp_rom_crc16_le` with
  invert-in/out wrapper; Kontronik `crc32r` (reflected 0xEDB88320) ≡
  `esp_rom_crc32_le` semantics.  If adopted, verify on the bench against
  live frames before deleting the bitwise versions — Pico builds still need
  the portable fallback (`sfx_platform` gate).
- **Reply TX is a 2.2 ms blocking wait** on the input task
  (`uart_wait_tx_done` in `sendExBusResponse`) ≈ 13 % duty at 65 Hz replies.
  A TX-done-driven `txDisable` (esp_timer one-shot at frame-time-µs, or the
  UART TX-done event) would free the task during transmission.  Medium
  complexity, touchy timing (the echo-drain choreography follows it) —
  **defer unless input latency ever matters again**; current lateSkip≈0
  says it doesn't.
- Parser/decoders are per-byte state machines at 4.8 kB/s and 4 kB/s — CPU
  noise; leave readable.

## 4. Memory alignment

- **PSRAM asset buffers**: allocate with `heap_caps_aligned_alloc(32, …)`
  (cache-line) instead of plain calloc — sequential decoder reads then fetch
  whole lines cleanly.  Minor, free.
- `LogEntry` is 136 B (8-aligned) — fine.  `sizeof` checks worth adding as
  `static_assert`s next to the hot structs (`Sensor`, `LogEntry`,
  `Mp3DecoderSlot`) so refactors don't silently bloat them.
- DMA buffers (I2S, SD, UART rings) are driver-allocated and already
  DMA-aligned.  int16 PCM rings are 4-aligned by type.
- Don't `__attribute__((packed))` anything on the hot path — Xtensa unaligned
  access costs more than the padding saves.

## 5. Recommended execution order

| # | Item | Win | Risk | Effort |
|---|---|---|---|---|
| 1 | MP3 pool cap 4 (after channel-claim audit) | −27 KB DRAM worst-case | low | XS |
| 2 | TelemetryHub → PSRAM | −4.4 KB DRAM | low | S |
| 3 | PCM scratch → PSRAM (+underrun bench check) | −18 KB @4 slots | med | S |
| 4 | Aligned PSRAM asset allocs + static_asserts | hygiene | none | XS |
| 5 | Sensor/Device struct reorder | ~0.3 KB + hygiene | none | XS |
| 6 | Pre-decoded PCM for gun/alert clips | decode CPU ↓ | low | M |
| 7 | Non-blocking reply TX | 2 ms/reply task time | med-high | M |
| 8 | CRC → ROM folds | cleanliness | low | S |

Items 1–5 are a single small PR; 6–8 only if a concrete need appears.
DRAM after 1–3: steady-state free ≈ 55–75 KB (from 25) — comfortable
headroom for the config export/import feature and future effects.
