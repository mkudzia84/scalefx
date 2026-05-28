/*
 * page_cache_test — ESP32-S3 firmware that exercises the cross-core
 * page-cache pattern with verbose plain-text telemetry.
 *
 * Boot flow
 * ─────────
 *   1. Init Serial @ 115200 (NOT 6 Mbps / NOT COBS).
 *   2. Mount SD via esp_vfs_fat_sdmmc_mount("/sdcard", ...).
 *   3. Start PageCache singleton (allocates slot table + spawns
 *      reader task on Core 1 prio MAX-3).
 *   4. Print free PSRAM + initial cache state, then drop into the
 *      command prompt.
 *
 * Commands (type one + ENTER on the serial monitor)
 * ─────────────────────────────────────────────────
 *   ls [/path]                       list a directory
 *   sd                               print SD status + card type
 *   verbose on|off                   toggle page-cache LOAD/EVICT lines
 *   linear <path>                    consume the file front-to-back
 *   loop   <path> <count>            consume in a loop N times
 *   multi  <pathA> <pathB>           two concurrent consumers
 *   stop                             stop any running consumers
 *   stats                            one-shot stats dump
 *   reset                            reset cache + consumer stats
 *
 * Telemetry every 1 second (always on while a consumer is running):
 *   [t=...] consumer ch0: 23456 KB total (190 KB/s), pos 23456789/17496208,
 *                         loop=1 underruns +0/12, max stall 0 ms
 *   [t=...] page-cache:  hit=512 miss=92 evict=2 rej=0 failed=0 resident=8/16,
 *                         bytes=2048/4096 KB, avg-load=83 ms max=124 ms, queue=0
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <esp_heap_caps.h>
#include <esp_psram.h>

#include "page_cache.h"
#include "test_consumer.h"
#include "audio_codec.h"
#include "audio_i2s.h"
#include "audio_player.h"

// ── HubFX pinout — matches controllers/hubfx/esp32s3 ──────────────────
namespace Pins {
    // SD card — 4-bit SDIO
    constexpr int SD_CLK   = 39;
    constexpr int SD_CMD   = 38;
    constexpr int SD_D0    = 40;
    constexpr int SD_D1    = 41;
    constexpr int SD_D2    = 42;
    constexpr int SD_D3    = 45;

    // I²C — TAS5825P codec
    constexpr int I2C_SDA  =  8;
    constexpr int I2C_SCL  =  9;

    // I²S to TAS5825P (Philips standard, 48 kHz / 16-bit)
    constexpr int I2S_DOUT = 16;
    constexpr int I2S_BCLK = 17;
    constexpr int I2S_LRCK = 18;
}

constexpr uint32_t kSampleRate = 48000;

static sdmmc_card_t* s_card = nullptr;

// Two consumers max for `multi` mode.
static TestConsumer s_consumers[2];
static int          s_consumerCount = 0;

// ── SD mount ─────────────────────────────────────────────────────────

static bool mountSd() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot          = SDMMC_HOST_SLOT_1;
    // 40 MHz HIGHSPEED is what the ESP-IDF default `SDMMC_FREQ_HIGHSPEED`
    // resolves to.  The 2026-05-27 SD read benchmark showed ~1 MB/s
    // throughput even though the card advertises 50 MHz tr_speed and
    // we have 4-bit width — investigating whether the driver actually
    // negotiates HS class or stays in default mode.  Per ESP-IDF docs,
    // setting max_freq_khz higher than 40 MHz silently caps to 40 MHz
    // on most ESP32-S3 cards; bump-test left at 40 MHz to compare
    // against the bench_dram (internal-SRAM buffer) result first.
    host.max_freq_khz  = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = (gpio_num_t)Pins::SD_CLK;
    slot.cmd = (gpio_num_t)Pins::SD_CMD;
    slot.d0  = (gpio_num_t)Pins::SD_D0;
    slot.d1  = (gpio_num_t)Pins::SD_D1;
    slot.d2  = (gpio_num_t)Pins::SD_D2;
    slot.d3  = (gpio_num_t)Pins::SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files              = 16;  // 8-stream bench_mix + reader cache (4) + spare
    mount.allocation_unit_size   = 0;

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        Serial.printf("[boot] SD mount FAILED: %s (0x%x)\n",
                      esp_err_to_name(err), (unsigned)err);
        return false;
    }
    const uint64_t cap = (uint64_t)s_card->csd.capacity *
                         (uint64_t)s_card->csd.sector_size;
    Serial.printf("[boot] SD ready: %lu MB, %s, %u kHz\n",
                  (unsigned long)(cap / (1024ULL * 1024ULL)),
                  s_card->is_mmc ? "MMC" : "SD",
                  (unsigned)s_card->max_freq_khz);
    return true;
}

// ── Helpers ──────────────────────────────────────────────────────────

static uint32_t fileSize(const char* userPath) {
    char full[160];
    snprintf(full, sizeof(full), "/sdcard%s%s",
             userPath[0] == '/' ? "" : "/", userPath);
    struct stat st;
    if (stat(full, &st) != 0) return 0;
    return (uint32_t)st.st_size;
}

static void cmdLs(const char* userPath) {
    char full[160];
    snprintf(full, sizeof(full), "/sdcard%s%s",
             userPath && userPath[0] == '/' ? "" : "/",
             userPath ? userPath : "");
    DIR* d = opendir(full);
    if (!d) { Serial.printf("opendir(%s) failed\n", full); return; }
    Serial.printf("Listing %s:\n", full);
    while (struct dirent* de = readdir(d)) {
        if (de->d_name[0] == '.') continue;
        char childPath[200];
        snprintf(childPath, sizeof(childPath), "%s/%s", full, de->d_name);
        struct stat st;
        if (stat(childPath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) Serial.printf("  d/   %s\n", de->d_name);
        else                     Serial.printf("  f %-10lu %s\n",
                                                (unsigned long)st.st_size,
                                                de->d_name);
    }
    closedir(d);
}

static void stopAllConsumers() {
    for (auto& c : s_consumers) c.stop();
    s_consumerCount = 0;
}

static bool startConsumer(int slot, const char* path, bool loop) {
    if (slot < 0 || slot >= (int)(sizeof(s_consumers)/sizeof(s_consumers[0])))
        return false;
    const uint32_t sz = fileSize(path);
    if (!sz) {
        Serial.printf("Path %s not found / empty\n", path);
        return false;
    }
    return s_consumers[slot].start(slot, path, sz, loop);
}

// ── Telemetry sweep ──────────────────────────────────────────────────

static uint32_t s_lastTelemMs = 0;

static void printTelemetry() {
    const uint32_t now = millis();
    const uint32_t dt  = now - s_lastTelemMs;
    s_lastTelemMs = now;

    for (int i = 0; i < s_consumerCount; ++i) {
        auto st = s_consumers[i].snapshot();
        const uint32_t rateKBs = dt > 0 ? (st.windowBytes * 1000) / dt / 1024 : 0;
        Serial.printf("[t=%lu] ch%d %s: %u KB total (%u KB/s), "
                      "pos %u, loops=%u, underruns=%u, max-stall=%u ms%s\n",
                      (unsigned long)now, s_consumers[i].channelId(),
                      s_consumers[i].path(),
                      (unsigned)(st.bytesConsumed / 1024), (unsigned)rateKBs,
                      (unsigned)st.cursor,
                      (unsigned)st.loopCount,
                      (unsigned)st.underruns,
                      (unsigned)st.maxStallMs,
                      st.done ? "  [DONE]" : "");
    }

    if (AudioPlayer::instance().isPlaying()) {
        auto ap = AudioPlayer::instance().snapshot();
        Serial.printf("[t=%lu] player: %u/%u KB (pos %u), loops=%u, "
                      "underruns=%u, max-stall=%u ms\n",
                      (unsigned long)now,
                      (unsigned)(ap.bytesWritten / 1024),
                      (unsigned)(ap.fileSize    / 1024),
                      (unsigned)ap.cursor, (unsigned)ap.loopCount,
                      (unsigned)ap.underruns, (unsigned)ap.maxStallMs);
    }

    auto p = PageCache::instance().stats();
    Serial.printf("[t=%lu] cache: hit=%u miss=%u evict=%u rej=%u failed=%u "
                  "resident=%u/%d (%u/%u KB) avg-load=%u ms max=%u ms queue=%d\n",
                  (unsigned long)now,
                  (unsigned)p.hits, (unsigned)p.misses,
                  (unsigned)p.evictions, (unsigned)p.budgetReject,
                  (unsigned)p.loadFailures,
                  (unsigned)p.residentPages, kMaxPageSlots,
                  (unsigned)(p.residentBytes / 1024),
                  (unsigned)(kPageBudgetBytes / 1024),
                  (unsigned)p.avgLoadMs, (unsigned)p.maxLoadMs,
                  p.queueDepth);
}

// ── Raw SD read benchmark (bypasses page cache + audio pacing) ───────
//
// `bench <path> [chunkKB]` — opens the file via POSIX fopen("/sdcard/..")
// and slurps it in `chunkKB`-sized chunks until EOF.  Reports total
// bytes, elapsed wall-clock ms, and average MB/s.  Chunks default to
// 32 KB; allocate from PSRAM so we don't blow the stack.  This measures
// what the SD subsystem can deliver on a sustained linear read — page
// cache is bypassed entirely, no rate limiting.
//
// Run multiple times back-to-back to spot first-cache-cold vs warm-FAT
// differences; the SDMMC driver caches FAT entries in RAM after the
// first walk so subsequent runs over the same file usually run faster.
static void cmdBench(const char* path, uint32_t chunkKB) {
    char fsPath[160];
    snprintf(fsPath, sizeof(fsPath), "/sdcard%s%s",
             (path[0] == '/') ? "" : "/", path);

    struct stat st;
    if (::stat(fsPath, &st) != 0) {
        Serial.printf("bench: stat(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }
    FILE* fp = ::fopen(fsPath, "rb");
    if (!fp) {
        Serial.printf("bench: fopen(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }

    if (chunkKB == 0) chunkKB = 32;          // default 32 KB chunks
    if (chunkKB > 256) chunkKB = 256;        // cap to keep PSRAM modest
    const size_t bufLen = chunkKB * 1024u;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(bufLen, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.printf("bench: PSRAM alloc %u KB failed\n", (unsigned)chunkKB);
        ::fclose(fp);
        return;
    }

    Serial.printf("bench: %s size=%lu KB, chunk=%u KB — reading…\n",
                  fsPath, (unsigned long)(st.st_size / 1024), (unsigned)chunkKB);

    const uint32_t startMs = millis();
    uint64_t totalRead = 0;
    uint32_t readOps   = 0;
    uint32_t maxOpUs   = 0;
    uint32_t cumOpUs   = 0;
    while (true) {
        const uint32_t opStart = micros();
        const size_t n = ::fread(buf, 1, bufLen, fp);
        const uint32_t opUs = micros() - opStart;
        if (n == 0) break;
        totalRead += n;
        ++readOps;
        cumOpUs += opUs;
        if (opUs > maxOpUs) maxOpUs = opUs;
        if (n < bufLen) break;     // short read = EOF
    }
    const uint32_t elapsedMs = millis() - startMs;

    ::fclose(fp);
    heap_caps_free(buf);

    // Compute throughput in MB/s ×100 to print as e.g. "12.34 MB/s"
    // without floats.  total_KB * 1000 / elapsed_ms = KB/s, then /1024
    // for MB/s (rounded ×100).
    const uint32_t totalKB = (uint32_t)(totalRead / 1024);
    const uint32_t kbPerSec = elapsedMs > 0
        ? (uint32_t)((totalRead / 1024ull * 1000ull) / elapsedMs)
        : 0;
    const uint32_t mbPerSecX100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
    const uint32_t avgOpUs = readOps ? (cumOpUs / readOps) : 0;

    Serial.printf(
        "bench: DONE — %llu B (%u KB) in %lu ms => %u KB/s = %u.%02u MB/s\n",
        (unsigned long long)totalRead,
        (unsigned)totalKB,
        (unsigned long)elapsedMs,
        (unsigned)kbPerSec,
        (unsigned)(mbPerSecX100 / 100),
        (unsigned)(mbPerSecX100 % 100));
    Serial.printf(
        "bench: ops=%u  avg-op=%u us  max-op=%u us  chunk=%u KB\n",
        (unsigned)readOps,
        (unsigned)avgOpUs,
        (unsigned)maxOpUs,
        (unsigned)chunkKB);
}

// `psram` — PSRAM diagnostic + memcpy throughput benchmark.
//
// Validates that PSRAM is configured at spec (ESP32-S3 with OPI PSRAM
// at 80 MHz should deliver ~40–80 MB/s memcpy throughput).  If PSRAM
// memcpy is at spec, the slow SD→PSRAM result is firmly a DMA bounce-
// buffer overhead (driver does SD→SRAM bounce → memcpy to PSRAM and
// the bottleneck is the round-trip + bookkeeping, not the PSRAM bus).
// If PSRAM memcpy is itself slow (~5 MB/s), then PSRAM is misconfigured
// (wrong mode / wrong clock).
//
// Build-time config check: ESP-IDF Kconfig macros — visible if the
// IDF headers expose them via `sdkconfig.h`.
static void cmdPsram() {
    // ── Capacity + cap-flag breakdown ────────────────────────────────
    const size_t psramTotal   = esp_psram_get_size();
    const size_t psramFree    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t dramTotal    = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t dramFree     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dramDmaFree  = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    Serial.printf("PSRAM: total=%u MB  free=%u KB  largest_block=%u KB\n",
                  (unsigned)(psramTotal / (1024 * 1024)),
                  (unsigned)(psramFree / 1024),
                  (unsigned)(psramLargest / 1024));
    Serial.printf("DRAM:  total=%u KB  free=%u KB  free(DMA-cap)=%u KB\n",
                  (unsigned)(dramTotal / 1024),
                  (unsigned)(dramFree / 1024),
                  (unsigned)(dramDmaFree / 1024));

    // ── Build-time mode + speed ──────────────────────────────────────
    // These are ESP-IDF Kconfig flags emitted into sdkconfig.h.  Print
    // whichever combination is active so misconfig (e.g. QIO @ 40 MHz)
    // is immediately visible.
#ifdef CONFIG_SPIRAM_MODE_OCT
    const char* psramMode = "OPI (octal, 8-bit)";
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
    const char* psramMode = "QIO (quad, 4-bit)";
#else
    const char* psramMode = "(mode flag absent)";
#endif
#if   defined(CONFIG_SPIRAM_SPEED_120M)
    const char* psramSpeed = "120 MHz";
#elif defined(CONFIG_SPIRAM_SPEED_80M)
    const char* psramSpeed = "80 MHz";
#elif defined(CONFIG_SPIRAM_SPEED_40M)
    const char* psramSpeed = "40 MHz";
#elif defined(CONFIG_SPIRAM_SPEED_20M)
    const char* psramSpeed = "20 MHz";
#else
    const char* psramSpeed = "(speed flag absent)";
#endif
    Serial.printf("PSRAM build config: mode=%s  speed=%s\n", psramMode, psramSpeed);

    // ── memcpy throughput benchmark ──────────────────────────────────
    // 64 KB working set is large enough to bust the cache and force
    // real PSRAM bus traffic on every iteration.  Repeat N times to
    // smooth out millis() granularity (~1 ms tick → need ≥10 ms total).
    constexpr size_t kBlock = 64 * 1024;     // 64 KB
    constexpr int    kIters = 64;            // 64 × 64 KB = 4 MB per test

    uint8_t* sram   = (uint8_t*)heap_caps_malloc(kBlock, MALLOC_CAP_INTERNAL);
    uint8_t* psramA = (uint8_t*)heap_caps_malloc(kBlock, MALLOC_CAP_SPIRAM);
    uint8_t* psramB = (uint8_t*)heap_caps_malloc(kBlock, MALLOC_CAP_SPIRAM);
    if (!sram || !psramA || !psramB) {
        Serial.printf("psram: alloc failed (sram=%p  psramA=%p  psramB=%p)\n",
                      sram, psramA, psramB);
        if (sram)   heap_caps_free(sram);
        if (psramA) heap_caps_free(psramA);
        if (psramB) heap_caps_free(psramB);
        return;
    }

    // Touch the source so we're not just measuring zero-fill.
    memset(sram,   0xA5, kBlock);
    memset(psramA, 0x5A, kBlock);

    auto bench = [&](const char* label, uint8_t* dst, uint8_t* src) {
        const uint64_t totalBytes = (uint64_t)kBlock * (uint64_t)kIters;
        const uint32_t t0 = micros();
        for (int i = 0; i < kIters; ++i) {
            memcpy(dst, src, kBlock);
        }
        const uint32_t dtUs  = micros() - t0;
        // Bytes/sec = totalBytes * 1e6 / dtUs.  Use ull math to avoid
        // overflow at 4 MB / few-ms timings.
        const uint32_t kbPerSec   = (uint32_t)((totalBytes * 1000000ull) / (uint64_t)dtUs / 1024ull);
        const uint32_t mbPerSec_x100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
        Serial.printf("  %-24s %llu B in %lu us => %u KB/s = %u.%02u MB/s\n",
                      label,
                      (unsigned long long)totalBytes,
                      (unsigned long)dtUs,
                      (unsigned)kbPerSec,
                      (unsigned)(mbPerSec_x100 / 100),
                      (unsigned)(mbPerSec_x100 % 100));
    };

    Serial.printf("Cache-hot memcpy (same 64 KB block × %d iters — measures cache, not PSRAM bus):\n",
                  kIters);
    bench("SRAM   -> SRAM   ",  sram,   sram);
    bench("SRAM   -> PSRAM  ",  psramA, sram);
    bench("PSRAM  -> SRAM   ",  sram,   psramA);
    bench("PSRAM  -> PSRAM  ",  psramB, psramA);

    heap_caps_free(sram);
    heap_caps_free(psramA);
    heap_caps_free(psramB);

    // ── Cache-busting bench: walk a 2 MB working set so each 64 KB
    //    memcpy hits unique cache lines.  ESP32-S3 L1 PSRAM cache is
    //    16-32 KB, so a 2 MB linear walk forces real bus traffic on
    //    every iteration.  This is the honest PSRAM bus throughput.
    constexpr size_t kBigBlock = 2 * 1024 * 1024;  // 2 MB working set per buffer
    uint8_t* bigA = (uint8_t*)heap_caps_malloc(kBigBlock, MALLOC_CAP_SPIRAM);
    uint8_t* bigB = (uint8_t*)heap_caps_malloc(kBigBlock, MALLOC_CAP_SPIRAM);
    uint8_t* bigSram = (uint8_t*)heap_caps_malloc(kBlock, MALLOC_CAP_INTERNAL);  // 64 KB src
    if (bigA && bigB && bigSram) {
        memset(bigA,    0xC3, kBigBlock);
        memset(bigSram, 0x3C, kBlock);

        auto bigBench = [&](const char* label, uint8_t* dst, uint8_t* src, bool dstWalk, bool srcWalk) {
            const size_t chunks = kBigBlock / kBlock;
            const uint64_t totalBytes = (uint64_t)kBlock * (uint64_t)chunks;
            const uint32_t t0 = micros();
            for (size_t i = 0; i < chunks; ++i) {
                uint8_t* d = dst + (dstWalk ? i * kBlock : 0);
                uint8_t* s = src + (srcWalk ? i * kBlock : 0);
                memcpy(d, s, kBlock);
            }
            const uint32_t dtUs = micros() - t0;
            const uint32_t kbPerSec = (uint32_t)((totalBytes * 1000000ull) / (uint64_t)dtUs / 1024ull);
            const uint32_t mbPerSec_x100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
            Serial.printf("  %-24s %llu B in %lu us => %u KB/s = %u.%02u MB/s\n",
                          label,
                          (unsigned long long)totalBytes,
                          (unsigned long)dtUs,
                          (unsigned)kbPerSec,
                          (unsigned)(mbPerSec_x100 / 100),
                          (unsigned)(mbPerSec_x100 % 100));
        };

        Serial.printf("Cache-busting memcpy (2 MB working set, 64 KB chunks — real PSRAM bus):\n");
        bigBench("SRAM   -> PSRAM (walk)",  bigA,    bigSram, true,  false);
        bigBench("PSRAM  -> SRAM (walk)",   bigSram, bigA,    false, true);
        bigBench("PSRAM  -> PSRAM (walk)",  bigB,    bigA,    true,  true);

        heap_caps_free(bigA);
        heap_caps_free(bigB);
        heap_caps_free(bigSram);
    } else {
        Serial.printf("psram: cache-busting test skipped (alloc failed)\n");
        if (bigA)    heap_caps_free(bigA);
        if (bigB)    heap_caps_free(bigB);
        if (bigSram) heap_caps_free(bigSram);
    }

    Serial.printf("psram: DONE — OPI/80 MHz expected ≈ 40-80 MB/s sustained; QIO/40 MHz ≈ 10-20 MB/s.\n");
}

// `bench_dram <path> [chunkKB]` — POSIX read with a DMA-capable
// INTERNAL-DRAM buffer (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`)
// instead of PSRAM.  ESP32-S3 SDMMC peripheral does DMA into the
// destination buffer; PSRAM-backed buffers go through the cache and
// often require a bounce-buffer copy that halves throughput.  This
// variant runs the same loop as `bench_raw` but with the buffer
// firmly in internal SRAM.  Chunk size is capped at 64 KB (internal
// DRAM headroom is tighter — total internal SRAM on ESP32-S3 is
// 320 KB).
static void cmdBenchDram(const char* path, uint32_t chunkKB) {
    char fsPath[160];
    snprintf(fsPath, sizeof(fsPath), "/sdcard%s%s",
             (path[0] == '/') ? "" : "/", path);

    struct stat st;
    if (::stat(fsPath, &st) != 0) {
        Serial.printf("bench_dram: stat(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }
    const int fd = ::open(fsPath, O_RDONLY);
    if (fd < 0) {
        Serial.printf("bench_dram: open(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }

    if (chunkKB == 0)  chunkKB = 32;
    if (chunkKB > 64)  chunkKB = 64;        // internal-DRAM headroom cap
    const size_t bufLen = chunkKB * 1024u;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(
        bufLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        Serial.printf("bench_dram: internal DMA alloc %u KB failed (free=%u KB)\n",
                      (unsigned)chunkKB,
                      (unsigned)(heap_caps_get_free_size(
                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));
        ::close(fd);
        return;
    }

    Serial.printf("bench_dram: %s size=%lu KB, chunk=%u KB (DMA internal SRAM) — reading…\n",
                  fsPath, (unsigned long)(st.st_size / 1024), (unsigned)chunkKB);

    const uint32_t startMs = millis();
    uint64_t totalRead = 0;
    uint32_t readOps   = 0;
    uint32_t maxOpUs   = 0;
    uint32_t cumOpUs   = 0;
    while (true) {
        const uint32_t opStart = micros();
        const ssize_t n = ::read(fd, buf, bufLen);
        const uint32_t opUs = micros() - opStart;
        if (n <= 0) break;
        totalRead += (uint64_t)n;
        ++readOps;
        cumOpUs += opUs;
        if (opUs > maxOpUs) maxOpUs = opUs;
        if ((size_t)n < bufLen) break;
    }
    const uint32_t elapsedMs = millis() - startMs;

    ::close(fd);
    heap_caps_free(buf);

    const uint32_t totalKB = (uint32_t)(totalRead / 1024);
    const uint32_t kbPerSec = elapsedMs > 0
        ? (uint32_t)((totalRead / 1024ull * 1000ull) / elapsedMs)
        : 0;
    const uint32_t mbPerSecX100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
    const uint32_t avgOpUs = readOps ? (cumOpUs / readOps) : 0;

    Serial.printf(
        "bench_dram: DONE — %llu B (%u KB) in %lu ms => %u KB/s = %u.%02u MB/s\n",
        (unsigned long long)totalRead,
        (unsigned)totalKB,
        (unsigned long)elapsedMs,
        (unsigned)kbPerSec,
        (unsigned)(mbPerSecX100 / 100),
        (unsigned)(mbPerSecX100 % 100));
    Serial.printf(
        "bench_dram: ops=%u  avg-op=%u us  max-op=%u us  chunk=%u KB\n",
        (unsigned)readOps,
        (unsigned)avgOpUs,
        (unsigned)maxOpUs,
        (unsigned)chunkKB);
}

// `bench_raw <path> [chunkKB]` — POSIX open()/read() variant.  Skips
// libc's FILE* buffering (fread copies SD→libc-buf→user-buf, every
// read goes through a 1 KB internal buffer by default).  This call
// path is SD → DMA → user-buf directly, which is what the production
// page_cache_test reader task uses.  If `bench_raw` is materially
// faster than `bench`, fread's libc buffering is a measurable choke.
static void cmdBenchRaw(const char* path, uint32_t chunkKB) {
    char fsPath[160];
    snprintf(fsPath, sizeof(fsPath), "/sdcard%s%s",
             (path[0] == '/') ? "" : "/", path);

    struct stat st;
    if (::stat(fsPath, &st) != 0) {
        Serial.printf("bench_raw: stat(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }
    const int fd = ::open(fsPath, O_RDONLY);
    if (fd < 0) {
        Serial.printf("bench_raw: open(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }

    if (chunkKB == 0)   chunkKB = 32;
    if (chunkKB > 256)  chunkKB = 256;
    const size_t bufLen = chunkKB * 1024u;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(bufLen, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.printf("bench_raw: PSRAM alloc %u KB failed\n", (unsigned)chunkKB);
        ::close(fd);
        return;
    }

    Serial.printf("bench_raw: %s size=%lu KB, chunk=%u KB (POSIX read) — reading…\n",
                  fsPath, (unsigned long)(st.st_size / 1024), (unsigned)chunkKB);

    const uint32_t startMs = millis();
    uint64_t totalRead = 0;
    uint32_t readOps   = 0;
    uint32_t maxOpUs   = 0;
    uint32_t cumOpUs   = 0;
    while (true) {
        const uint32_t opStart = micros();
        const ssize_t n = ::read(fd, buf, bufLen);
        const uint32_t opUs = micros() - opStart;
        if (n <= 0) break;
        totalRead += (uint64_t)n;
        ++readOps;
        cumOpUs += opUs;
        if (opUs > maxOpUs) maxOpUs = opUs;
        if ((size_t)n < bufLen) break;
    }
    const uint32_t elapsedMs = millis() - startMs;

    ::close(fd);
    heap_caps_free(buf);

    const uint32_t totalKB = (uint32_t)(totalRead / 1024);
    const uint32_t kbPerSec = elapsedMs > 0
        ? (uint32_t)((totalRead / 1024ull * 1000ull) / elapsedMs)
        : 0;
    const uint32_t mbPerSecX100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
    const uint32_t avgOpUs = readOps ? (cumOpUs / readOps) : 0;

    Serial.printf(
        "bench_raw: DONE — %llu B (%u KB) in %lu ms => %u KB/s = %u.%02u MB/s\n",
        (unsigned long long)totalRead,
        (unsigned)totalKB,
        (unsigned long)elapsedMs,
        (unsigned)kbPerSec,
        (unsigned)(mbPerSecX100 / 100),
        (unsigned)(mbPerSecX100 % 100));
    Serial.printf(
        "bench_raw: ops=%u  avg-op=%u us  max-op=%u us  chunk=%u KB\n",
        (unsigned)readOps,
        (unsigned)avgOpUs,
        (unsigned)maxOpUs,
        (unsigned)chunkKB);
}

// ─────────────────────────────────────────────────────────────────────
// `bench_mix <N> <path> [chunkKB]` — simulate audio-mixer N-channel
//
// Models the AudioMixer's multi-channel SD access pattern: open the
// file N times (N independent file descriptors), seek each fd to a
// distinct offset spaced 1/N through the file, then loop calling
// `read(chunkKB)` on each fd in round-robin until each stream has
// consumed `kTargetPerStream` bytes.
//
// From the SD controller's perspective, the LBA access pattern is
// identical to N separate WAV files being decoded concurrently:
// sequential within each "stream", random at the file-boundary
// crossings.  This is the worst-case load for SDXC cards optimised
// for single-reader sequential streaming (TLC GC pauses + large-AU
// thrashing surface here, not in plain `bench`).
//
// Buffer lives in DMA-cap internal SRAM (MALLOC_CAP_DMA |
// MALLOC_CAP_INTERNAL) — exact same allocation flags the AudioMixer
// uses for its `_sdReadBuf`.  Default chunk = 32 KB matches
// WAV_SD_READ_BYTES.
//
// Output reports:
//   - Aggregate KB/s + MB/s across all streams
//   - Per-stream KB read
//   - avg-op / max-op latency (worst stream index)
//   - "audio-mixer headroom" factor — how many concurrent 24 kHz /
//     16-bit mono channels this throughput can sustain (each channel
//     consumes ~48 KB/s of source data)
static void cmdBenchMix(uint32_t numStreams, const char* path, uint32_t chunkKB) {
    if (numStreams == 0)  numStreams = 2;
    if (numStreams > 8)   numStreams = 8;
    if (chunkKB == 0)     chunkKB = 32;
    if (chunkKB > 64)     chunkKB = 64;   // internal-DRAM headroom cap

    char fsPath[160];
    snprintf(fsPath, sizeof(fsPath), "/sdcard%s%s",
             (path[0] == '/') ? "" : "/", path);

    struct stat st;
    if (::stat(fsPath, &st) != 0) {
        Serial.printf("bench_mix: stat(%s) failed (errno=%d)\n", fsPath, errno);
        return;
    }
    if (st.st_size < (off_t)(chunkKB * 1024u * 4u)) {
        Serial.printf("bench_mix: file %s too small (%lu KB) for %u-stream test\n",
                      fsPath, (unsigned long)(st.st_size / 1024),
                      (unsigned)numStreams);
        return;
    }

    // Open one fd per stream, seek each to file_size * i / N (aligned to chunk)
    int      fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint64_t start_off[8] = {};
    for (uint32_t i = 0; i < numStreams; ++i) {
        fds[i] = ::open(fsPath, O_RDONLY);
        if (fds[i] < 0) {
            Serial.printf("bench_mix: open stream %u failed (errno=%d)\n",
                          (unsigned)i, errno);
            for (uint32_t j = 0; j < i; ++j) ::close(fds[j]);
            return;
        }
        // Stagger each stream's start position by file_size / N, aligned
        // down to chunkKB boundary so reads stay sector-aligned.
        const uint64_t raw     = ((uint64_t)st.st_size * (uint64_t)i) /
                                 (uint64_t)numStreams;
        const uint64_t aligned = raw - (raw % (uint64_t)(chunkKB * 1024u));
        ::lseek(fds[i], (off_t)aligned, SEEK_SET);
        start_off[i] = aligned;
    }

    const size_t bufLen = chunkKB * 1024u;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(
        bufLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        Serial.printf("bench_mix: DMA-cap alloc %u KB failed (free=%u KB)\n",
                      (unsigned)chunkKB,
                      (unsigned)(heap_caps_get_free_size(
                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));
        for (uint32_t i = 0; i < numStreams; ++i) ::close(fds[i]);
        return;
    }

    Serial.printf("bench_mix: %u streams × %u KB chunks on %s (%lu KB total)\n",
                  (unsigned)numStreams, (unsigned)chunkKB, fsPath,
                  (unsigned long)(st.st_size / 1024));
    Serial.printf("bench_mix: start offsets (KB):");
    for (uint32_t i = 0; i < numStreams; ++i) {
        Serial.printf(" [%u]=%lu", (unsigned)i,
                      (unsigned long)(start_off[i] / 1024));
    }
    Serial.printf("\n");

    // Read budget per stream — capped at 16 MB so a small file doesn't
    // turn the test into a single-pass sequential read.  Loops back to
    // the stream's start position on EOF (mirrors AudioMixer's loop).
    constexpr uint64_t kTargetPerStream = 16ull * 1024ull * 1024ull;
    uint64_t totalRead       = 0;
    uint64_t perStream[8]    = {};
    uint32_t perStreamOps[8] = {};
    uint32_t perStreamMaxUs[8] = {};
    uint32_t totalOps        = 0;
    uint32_t maxOpUs         = 0;
    uint32_t worstStreamIdx  = 0;
    uint64_t cumOpUs         = 0;

    const uint32_t startMs = millis();
    bool done = false;
    while (!done) {
        bool allDone = true;
        for (uint32_t s = 0; s < numStreams; ++s) {
            if (perStream[s] >= kTargetPerStream) continue;
            allDone = false;

            const uint32_t opStart = micros();
            ssize_t n = ::read(fds[s], buf, bufLen);
            const uint32_t opUs = micros() - opStart;

            if (n <= 0) {
                // EOF — rewind this stream to its staggered start and retry.
                ::lseek(fds[s], (off_t)start_off[s], SEEK_SET);
                const uint32_t retryStart = micros();
                n = ::read(fds[s], buf, bufLen);
                const uint32_t retryUs = micros() - retryStart;
                if (n <= 0) { done = true; break; }
                // Account the rewind read's time too.
                cumOpUs += retryUs;
                if (retryUs > maxOpUs) { maxOpUs = retryUs; worstStreamIdx = s; }
                if (retryUs > perStreamMaxUs[s]) perStreamMaxUs[s] = retryUs;
                ++totalOps;
                ++perStreamOps[s];
            }

            totalRead       += (uint64_t)n;
            perStream[s]    += (uint64_t)n;
            cumOpUs         += opUs;
            ++totalOps;
            ++perStreamOps[s];
            if (opUs > maxOpUs)            { maxOpUs = opUs; worstStreamIdx = s; }
            if (opUs > perStreamMaxUs[s])  perStreamMaxUs[s] = opUs;
        }
        if (allDone) done = true;
    }
    const uint32_t elapsedMs = millis() - startMs;

    for (uint32_t i = 0; i < numStreams; ++i) ::close(fds[i]);
    heap_caps_free(buf);

    // ── Aggregate report ─────────────────────────────────────────────
    const uint32_t totalKB     = (uint32_t)(totalRead / 1024);
    const uint32_t kbPerSec    = elapsedMs > 0
        ? (uint32_t)((totalRead / 1024ull * 1000ull) / elapsedMs) : 0;
    const uint32_t mbPerSecX100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
    const uint32_t avgOpUs     = totalOps ? (uint32_t)(cumOpUs / totalOps) : 0;

    Serial.printf(
        "bench_mix: DONE — %llu B (%u KB) in %lu ms => %u KB/s = %u.%02u MB/s\n",
        (unsigned long long)totalRead,
        (unsigned)totalKB,
        (unsigned long)elapsedMs,
        (unsigned)kbPerSec,
        (unsigned)(mbPerSecX100 / 100),
        (unsigned)(mbPerSecX100 % 100));
    Serial.printf(
        "bench_mix: streams=%u  chunk=%u KB  ops=%u  avg-op=%u us  max-op=%u us (stream %u)\n",
        (unsigned)numStreams, (unsigned)chunkKB,
        (unsigned)totalOps, (unsigned)avgOpUs,
        (unsigned)maxOpUs, (unsigned)worstStreamIdx);

    // ── Per-stream breakdown ─────────────────────────────────────────
    for (uint32_t i = 0; i < numStreams; ++i) {
        const uint32_t sKB    = (uint32_t)(perStream[i] / 1024);
        const uint32_t sKBs   = elapsedMs > 0
            ? (uint32_t)((perStream[i] / 1024ull * 1000ull) / elapsedMs) : 0;
        Serial.printf(
            "bench_mix:   stream[%u] %u KB (%u KB/s)  ops=%u  max-op=%u us\n",
            (unsigned)i, (unsigned)sKB, (unsigned)sKBs,
            (unsigned)perStreamOps[i], (unsigned)perStreamMaxUs[i]);
    }

    // ── Audio-mixer headroom ─────────────────────────────────────────
    // Each AudioMixer channel decoding a 24 kHz / 16-bit mono WAV
    // consumes ~48 KB/s of source data (24000 frames × 2 bytes /
    // second).  Per-channel rate × N streams = required sustained
    // bandwidth.  Worst-case ring drain budget at 192 KB/s output is
    // 85 ms — any max-op latency exceeding that is a likely underrun
    // trigger (the ring may drop below the consumer's read cadence).
    constexpr uint32_t kPerChannelKBs = 48;            // 24 kHz / 16-bit mono
    constexpr uint32_t kRingDrainMs   = 85;            // 16 KB / 192 KB/s
    const uint32_t requiredKBs   = numStreams * kPerChannelKBs;
    const uint32_t headroomX100  = requiredKBs > 0
        ? (uint32_t)(((uint64_t)kbPerSec * 100ull) / requiredKBs) : 0;

    Serial.printf(
        "bench_mix: audio-mixer fit (24 kHz/16-bit mono) — need %u KB/s for %u ch,"
        " measured %u KB/s => headroom %u.%02ux\n",
        (unsigned)requiredKBs,
        (unsigned)numStreams,
        (unsigned)kbPerSec,
        (unsigned)(headroomX100 / 100),
        (unsigned)(headroomX100 % 100));

    if (maxOpUs > kRingDrainMs * 1000u) {
        Serial.printf(
            "bench_mix: WARN — max-op %u ms exceeds ring drain budget %u ms"
            " → underrun risk under load (suspect TLC GC pause / large-AU thrash)\n",
            (unsigned)(maxOpUs / 1000), (unsigned)kRingDrainMs);
    }
}

// ─────────────────────────────────────────────────────────────────────
// `bench_mix_files <chunkKB> <path1> <path2> ... <pathN>`
//
// Same access pattern as bench_mix but reading from N DIFFERENT files
// instead of N positions in one file.  Maximises the LBA span between
// stream switches: each round-robin step crosses a fresh AU/erase-block
// boundary and trashes the card's internal prefetch — exactly the
// hostile pattern for SDXC cards optimised for single-reader sequential
// streaming.  Closer to the actual AudioMixer behaviour where each
// channel decodes its own WAV.
//
// Up to 8 paths supported (VFS file-descriptor cap).  Each file is
// opened, read from offset 0, looped back on EOF.  Reports same
// per-stream + headroom stats as bench_mix.
static void cmdBenchMixFiles(uint32_t chunkKB, const char** paths, uint32_t numFiles) {
    if (numFiles == 0)  { Serial.printf("bench_mix_files: need at least one path\n"); return; }
    if (numFiles > 8)   numFiles = 8;
    if (chunkKB == 0)   chunkKB = 32;
    if (chunkKB > 64)   chunkKB = 64;

    char fsPath[8][160];
    int  fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint64_t sizes[8] = {};

    for (uint32_t i = 0; i < numFiles; ++i) {
        snprintf(fsPath[i], sizeof(fsPath[i]), "/sdcard%s%s",
                 (paths[i][0] == '/') ? "" : "/", paths[i]);
        struct stat st;
        if (::stat(fsPath[i], &st) != 0) {
            Serial.printf("bench_mix_files: stat(%s) failed (errno=%d)\n",
                          fsPath[i], errno);
            for (uint32_t j = 0; j < i; ++j) ::close(fds[j]);
            return;
        }
        sizes[i] = (uint64_t)st.st_size;
        if (sizes[i] < (uint64_t)(chunkKB * 1024u * 2u)) {
            Serial.printf("bench_mix_files: %s too small (%lu KB)\n",
                          fsPath[i], (unsigned long)(sizes[i] / 1024));
            for (uint32_t j = 0; j < i; ++j) ::close(fds[j]);
            return;
        }
        fds[i] = ::open(fsPath[i], O_RDONLY);
        if (fds[i] < 0) {
            Serial.printf("bench_mix_files: open(%s) failed (errno=%d)\n",
                          fsPath[i], errno);
            for (uint32_t j = 0; j < i; ++j) ::close(fds[j]);
            return;
        }
    }

    const size_t bufLen = chunkKB * 1024u;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(
        bufLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        Serial.printf("bench_mix_files: DMA-cap alloc %u KB failed\n", (unsigned)chunkKB);
        for (uint32_t i = 0; i < numFiles; ++i) ::close(fds[i]);
        return;
    }

    Serial.printf("bench_mix_files: %u files × %u KB chunks\n",
                  (unsigned)numFiles, (unsigned)chunkKB);
    for (uint32_t i = 0; i < numFiles; ++i) {
        Serial.printf("bench_mix_files:   [%u] %s (%lu KB)\n",
                      (unsigned)i, fsPath[i], (unsigned long)(sizes[i] / 1024));
    }

    constexpr uint64_t kTargetPerStream = 16ull * 1024ull * 1024ull;
    uint64_t totalRead         = 0;
    uint64_t perStream[8]      = {};
    uint32_t perStreamOps[8]   = {};
    uint32_t perStreamMaxUs[8] = {};
    uint32_t totalOps          = 0;
    uint32_t maxOpUs           = 0;
    uint32_t worstStreamIdx    = 0;
    uint64_t cumOpUs           = 0;

    const uint32_t startMs = millis();
    bool done = false;
    while (!done) {
        bool allDone = true;
        for (uint32_t s = 0; s < numFiles; ++s) {
            if (perStream[s] >= kTargetPerStream) continue;
            allDone = false;

            const uint32_t opStart = micros();
            ssize_t n = ::read(fds[s], buf, bufLen);
            const uint32_t opUs = micros() - opStart;

            if (n <= 0) {
                ::lseek(fds[s], 0, SEEK_SET);
                const uint32_t retryStart = micros();
                n = ::read(fds[s], buf, bufLen);
                const uint32_t retryUs = micros() - retryStart;
                if (n <= 0) { done = true; break; }
                cumOpUs += retryUs;
                if (retryUs > maxOpUs) { maxOpUs = retryUs; worstStreamIdx = s; }
                if (retryUs > perStreamMaxUs[s]) perStreamMaxUs[s] = retryUs;
                ++totalOps;
                ++perStreamOps[s];
            }

            totalRead       += (uint64_t)n;
            perStream[s]    += (uint64_t)n;
            cumOpUs         += opUs;
            ++totalOps;
            ++perStreamOps[s];
            if (opUs > maxOpUs)           { maxOpUs = opUs; worstStreamIdx = s; }
            if (opUs > perStreamMaxUs[s]) perStreamMaxUs[s] = opUs;
        }
        if (allDone) done = true;
    }
    const uint32_t elapsedMs = millis() - startMs;

    for (uint32_t i = 0; i < numFiles; ++i) ::close(fds[i]);
    heap_caps_free(buf);

    const uint32_t totalKB      = (uint32_t)(totalRead / 1024);
    const uint32_t kbPerSec     = elapsedMs > 0
        ? (uint32_t)((totalRead / 1024ull * 1000ull) / elapsedMs) : 0;
    const uint32_t mbPerSecX100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
    const uint32_t avgOpUs      = totalOps ? (uint32_t)(cumOpUs / totalOps) : 0;

    Serial.printf(
        "bench_mix_files: DONE — %llu B (%u KB) in %lu ms => %u KB/s = %u.%02u MB/s\n",
        (unsigned long long)totalRead,
        (unsigned)totalKB,
        (unsigned long)elapsedMs,
        (unsigned)kbPerSec,
        (unsigned)(mbPerSecX100 / 100),
        (unsigned)(mbPerSecX100 % 100));
    Serial.printf(
        "bench_mix_files: files=%u  chunk=%u KB  ops=%u  avg-op=%u us  max-op=%u us (stream %u)\n",
        (unsigned)numFiles, (unsigned)chunkKB,
        (unsigned)totalOps, (unsigned)avgOpUs,
        (unsigned)maxOpUs, (unsigned)worstStreamIdx);

    for (uint32_t i = 0; i < numFiles; ++i) {
        const uint32_t sKB  = (uint32_t)(perStream[i] / 1024);
        const uint32_t sKBs = elapsedMs > 0
            ? (uint32_t)((perStream[i] / 1024ull * 1000ull) / elapsedMs) : 0;
        Serial.printf(
            "bench_mix_files:   stream[%u] %u KB (%u KB/s)  ops=%u  max-op=%u us\n",
            (unsigned)i, (unsigned)sKB, (unsigned)sKBs,
            (unsigned)perStreamOps[i], (unsigned)perStreamMaxUs[i]);
    }

    constexpr uint32_t kPerChannelKBs = 48;
    constexpr uint32_t kRingDrainMs   = 85;
    const uint32_t requiredKBs   = numFiles * kPerChannelKBs;
    const uint32_t headroomX100  = requiredKBs > 0
        ? (uint32_t)(((uint64_t)kbPerSec * 100ull) / requiredKBs) : 0;
    Serial.printf(
        "bench_mix_files: audio-mixer fit — need %u KB/s for %u ch, measured %u KB/s"
        " => headroom %u.%02ux\n",
        (unsigned)requiredKBs, (unsigned)numFiles, (unsigned)kbPerSec,
        (unsigned)(headroomX100 / 100), (unsigned)(headroomX100 % 100));

    if (maxOpUs > kRingDrainMs * 1000u) {
        Serial.printf(
            "bench_mix_files: WARN — max-op %u ms exceeds ring drain budget %u ms"
            " → underrun risk under load\n",
            (unsigned)(maxOpUs / 1000), (unsigned)kRingDrainMs);
    }
}

// ─────────────────────────────────────────────────────────────────────
// `bench_during_play <play_path> <N> <bench_path> [chunkKB]`
//
// Investigates the SD-stall-under-audio hypothesis: starts a LOOPING
// audio consumer on `play_path` (drives the codec via I²S DMA + page
// cache reader task, exactly like production playback), waits for
// steady-state, then runs an N-stream SD bench on `bench_path` and
// reports per-read latency.
//
// Compare against plain `bench_mix N bench_path`:
//   - If max-op latency goes up materially in `bench_during_play`, the
//     active I²S / audio path is interfering with SDMMC reads (bus
//     contention, ISR pressure, PSRAM bandwidth, etc.).
//   - If max-op stays similar, the audio path is innocent and the
//     production stall has a different cause (e.g. VFS-FAT bookkeeping
//     when multiple files are open in production).
//
// The consumer's snapshot at the end reports underrun count — if the
// bench reads were slow enough to stall the producer, the consumer
// will have inserted silence; this is the same path that drives the
// `under+` count in production's pace log.
static void cmdBenchDuringPlay(const char* playPath, uint32_t numStreams,
                               const char* benchPath, uint32_t chunkKB) {
    if (numStreams == 0)  numStreams = 2;
    if (numStreams > 8)   numStreams = 8;
    if (chunkKB == 0)     chunkKB = 32;
    if (chunkKB > 64)     chunkKB = 64;

    // ── Start audio playback in slot 0 (looping) ─────────────────────
    stopAllConsumers();
    if (!startConsumer(0, playPath, /*loop=*/true)) {
        Serial.printf("bench_during_play: failed to start consumer on %s\n", playPath);
        return;
    }
    s_consumerCount = 1;
    Serial.printf("bench_during_play: PLAYING %s (loop) — settling 2 s before bench\n",
                  playPath);
    // Let the I²S pipeline reach steady state — codec PLAY locks, page
    // cache pre-fills, ring buffer stabilizes.  Without this the bench
    // catches startup transients (initial page loads, codec wake) and
    // the result is noisy.
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Snapshot consumer stats at start so we can attribute underruns +
    // bytes consumed to the bench window specifically.
    const auto preSnap  = s_consumers[0].snapshot();
    const uint32_t preTotal = preSnap.bytesConsumed;
    const uint32_t preUnder = preSnap.underruns;
    const uint32_t preMaxStall = preSnap.maxStallMs;

    // ── Run the bench (same pattern as bench_mix on one file) ────────
    char fsPath[160];
    snprintf(fsPath, sizeof(fsPath), "/sdcard%s%s",
             (benchPath[0] == '/') ? "" : "/", benchPath);

    struct stat st;
    if (::stat(fsPath, &st) != 0) {
        Serial.printf("bench_during_play: stat(%s) failed (errno=%d)\n", fsPath, errno);
        stopAllConsumers();
        return;
    }
    if (st.st_size < (off_t)(chunkKB * 1024u * 4u)) {
        Serial.printf("bench_during_play: bench file too small\n");
        stopAllConsumers();
        return;
    }

    int      fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint64_t start_off[8] = {};
    for (uint32_t i = 0; i < numStreams; ++i) {
        fds[i] = ::open(fsPath, O_RDONLY);
        if (fds[i] < 0) {
            Serial.printf("bench_during_play: open stream %u failed\n", (unsigned)i);
            for (uint32_t j = 0; j < i; ++j) ::close(fds[j]);
            stopAllConsumers();
            return;
        }
        const uint64_t raw     = ((uint64_t)st.st_size * (uint64_t)i) /
                                 (uint64_t)numStreams;
        const uint64_t aligned = raw - (raw % (uint64_t)(chunkKB * 1024u));
        ::lseek(fds[i], (off_t)aligned, SEEK_SET);
        start_off[i] = aligned;
    }

    const size_t bufLen = chunkKB * 1024u;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(
        bufLen, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        Serial.printf("bench_during_play: DMA-cap alloc %u KB failed\n", (unsigned)chunkKB);
        for (uint32_t i = 0; i < numStreams; ++i) ::close(fds[i]);
        stopAllConsumers();
        return;
    }

    Serial.printf("bench_during_play: %u streams × %u KB on %s WHILE %s plays\n",
                  (unsigned)numStreams, (unsigned)chunkKB, fsPath, playPath);

    // Latency histogram in 5 ms buckets — captures the SHAPE of the
    // distribution rather than just avg + max.  >100 ms goes in the
    // final bucket; max separately tracked.
    constexpr int kBuckets = 21;          // 0..100 ms in 5 ms steps + overflow
    uint32_t hist[kBuckets] = {};

    constexpr uint64_t kTargetPerStream = 8ull * 1024ull * 1024ull;
    uint64_t totalRead = 0;
    uint64_t perStream[8] = {};
    uint32_t totalOps  = 0;
    uint32_t maxOpUs   = 0;
    uint32_t worstStreamIdx = 0;
    uint64_t cumOpUs   = 0;

    const uint32_t startMs = millis();
    bool done = false;
    while (!done) {
        bool allDone = true;
        for (uint32_t s = 0; s < numStreams; ++s) {
            if (perStream[s] >= kTargetPerStream) continue;
            allDone = false;

            const uint32_t opStart = micros();
            ssize_t n = ::read(fds[s], buf, bufLen);
            const uint32_t opUs = micros() - opStart;

            if (n <= 0) {
                ::lseek(fds[s], (off_t)start_off[s], SEEK_SET);
                n = ::read(fds[s], buf, bufLen);
                if (n <= 0) { done = true; break; }
            }

            totalRead    += (uint64_t)n;
            perStream[s] += (uint64_t)n;
            cumOpUs      += opUs;
            ++totalOps;
            if (opUs > maxOpUs) { maxOpUs = opUs; worstStreamIdx = s; }

            // Bucket: 0 = 0-5 ms, 1 = 5-10 ms, ..., 20 = >100 ms
            int b = (int)(opUs / 5000u);
            if (b >= kBuckets) b = kBuckets - 1;
            ++hist[b];
        }
        if (allDone) done = true;
    }
    const uint32_t elapsedMs = millis() - startMs;

    for (uint32_t i = 0; i < numStreams; ++i) ::close(fds[i]);
    heap_caps_free(buf);

    // ── Post-snapshot consumer stats ─────────────────────────────────
    const auto postSnap = s_consumers[0].snapshot();
    const uint32_t deltaTotal = postSnap.bytesConsumed - preTotal;
    const uint32_t deltaUnder = postSnap.underruns     - preUnder;
    const uint32_t postMaxStall = postSnap.maxStallMs;
    (void)preMaxStall; (void)postMaxStall;  // surfaced via consumer log already

    // ── Report ───────────────────────────────────────────────────────
    const uint32_t totalKB     = (uint32_t)(totalRead / 1024);
    const uint32_t kbPerSec    = elapsedMs > 0
        ? (uint32_t)((totalRead / 1024ull * 1000ull) / elapsedMs) : 0;
    const uint32_t mbPerSecX100 = (uint32_t)((kbPerSec * 100ull) / 1024ull);
    const uint32_t avgOpUs     = totalOps ? (uint32_t)(cumOpUs / totalOps) : 0;

    Serial.printf(
        "bench_during_play: DONE — %u KB in %lu ms => %u KB/s = %u.%02u MB/s\n",
        (unsigned)totalKB,
        (unsigned long)elapsedMs,
        (unsigned)kbPerSec,
        (unsigned)(mbPerSecX100 / 100),
        (unsigned)(mbPerSecX100 % 100));
    Serial.printf(
        "bench_during_play: streams=%u  chunk=%u KB  ops=%u  avg=%u us  max=%u us (stream %u)\n",
        (unsigned)numStreams, (unsigned)chunkKB,
        (unsigned)totalOps, (unsigned)avgOpUs,
        (unsigned)maxOpUs, (unsigned)worstStreamIdx);

    Serial.printf("bench_during_play: latency histogram (5 ms buckets):\n");
    for (int b = 0; b < kBuckets; ++b) {
        if (hist[b] == 0) continue;
        const int loMs = b * 5;
        const int hiMs = (b == kBuckets - 1) ? 9999 : (b + 1) * 5;
        Serial.printf("   %3d-%3d ms : %5u\n", loMs, hiMs, (unsigned)hist[b]);
    }

    Serial.printf(
        "bench_during_play: consumer (audio path) — bytes +%u KB, underruns +%u\n",
        (unsigned)(deltaTotal / 1024), (unsigned)deltaUnder);

    stopAllConsumers();
}

// ── Command parser ───────────────────────────────────────────────────

static String s_line;

static void handleCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    // Split on whitespace, up to 12 tokens (bench_mix_files takes
    // <cmd> <chunkKB> + up to 8 paths + 2 spare).
    constexpr int kMaxTok = 12;
    String tok[kMaxTok];
    int n = 0;
    int start = 0;
    while (n < kMaxTok && start <= (int)cmd.length()) {
        int sp = cmd.indexOf(' ', start);
        if (sp < 0) sp = cmd.length();
        tok[n++] = cmd.substring(start, sp);
        start = sp + 1;
    }

    if (tok[0] == "ls") {
        cmdLs(tok[1].length() ? tok[1].c_str() : "/");
    }
    else if (tok[0] == "sd") {
        if (!s_card) { Serial.printf("SD: (not mounted)\n"); }
        else {
            // Detailed card diagnostics — bus width + SD spec + CSD
            // transfer rate tell us whether we're getting full 4-bit
            // HS or a fallback (the most common cause of slow reads).
            Serial.printf("SD: type=%s  max_freq=%u kHz  bus_width=%d-bit\n",
                          s_card->is_mmc ? "MMC" : "SD",
                          (unsigned)s_card->max_freq_khz,
                          (int)s_card->log_bus_width);
            Serial.printf("    scr.bus_width=%u  scr.sd_spec=%u  csd.tr_speed=%u (raw)\n",
                          (unsigned)s_card->scr.bus_width,
                          (unsigned)s_card->scr.sd_spec,
                          (unsigned)s_card->csd.tr_speed);
            Serial.printf("    capacity=%llu MB  sector=%u B  read_block_len=%u\n",
                          (unsigned long long)((uint64_t)s_card->csd.capacity *
                                               (uint64_t)s_card->csd.sector_size /
                                               (1024ull * 1024ull)),
                          (unsigned)s_card->csd.sector_size,
                          (unsigned)s_card->csd.read_block_len);
            Serial.printf("    free PSRAM=%u KB\n",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
    }
    else if (tok[0] == "verbose") {
        const bool v = (tok[1] == "on" || tok[1] == "1");
        PageCache::instance().setVerbose(v);
        Serial.printf("Verbose page-cache logging: %s\n", v ? "ON" : "OFF");
    }
    else if (tok[0] == "bench" && tok[1].length()) {
        stopAllConsumers();
        const uint32_t chunkKB = tok[2].length() ? (uint32_t)tok[2].toInt() : 32u;
        cmdBench(tok[1].c_str(), chunkKB);
    }
    else if (tok[0] == "bench_raw" && tok[1].length()) {
        stopAllConsumers();
        const uint32_t chunkKB = tok[2].length() ? (uint32_t)tok[2].toInt() : 32u;
        cmdBenchRaw(tok[1].c_str(), chunkKB);
    }
    else if (tok[0] == "bench_dram" && tok[1].length()) {
        stopAllConsumers();
        const uint32_t chunkKB = tok[2].length() ? (uint32_t)tok[2].toInt() : 32u;
        cmdBenchDram(tok[1].c_str(), chunkKB);
    }
    else if (tok[0] == "bench_mix" && tok[1].length() && tok[2].length()) {
        // bench_mix <N> <path> [chunkKB]
        stopAllConsumers();
        const uint32_t numStreams = (uint32_t)tok[1].toInt();
        const uint32_t chunkKB = tok[3].length() ? (uint32_t)tok[3].toInt() : 32u;
        cmdBenchMix(numStreams, tok[2].c_str(), chunkKB);
    }
    else if (tok[0] == "bench_mix_files" && tok[1].length() && tok[2].length()) {
        // bench_mix_files <chunkKB> <path1> <path2> ... <pathN>
        stopAllConsumers();
        const uint32_t chunkKB = (uint32_t)tok[1].toInt();
        const char* paths[8];
        uint32_t numFiles = 0;
        for (int i = 2; i < kMaxTok && numFiles < 8 && tok[i].length(); ++i) {
            paths[numFiles++] = tok[i].c_str();
        }
        cmdBenchMixFiles(chunkKB, paths, numFiles);
    }
    else if (tok[0] == "bench_during_play" && tok[1].length() && tok[2].length() && tok[3].length()) {
        // bench_during_play <play_path> <N> <bench_path> [chunkKB]
        const uint32_t numStreams = (uint32_t)tok[2].toInt();
        const uint32_t chunkKB    = tok[4].length() ? (uint32_t)tok[4].toInt() : 32u;
        cmdBenchDuringPlay(tok[1].c_str(), numStreams, tok[3].c_str(), chunkKB);
    }
    else if (tok[0] == "psram") {
        stopAllConsumers();
        cmdPsram();
    }
    else if (tok[0] == "linear" && tok[1].length()) {
        stopAllConsumers();
        if (startConsumer(0, tok[1].c_str(), false)) s_consumerCount = 1;
    }
    else if (tok[0] == "loop" && tok[1].length()) {
        stopAllConsumers();
        if (startConsumer(0, tok[1].c_str(), true)) s_consumerCount = 1;
    }
    else if (tok[0] == "multi" && tok[1].length() && tok[2].length()) {
        stopAllConsumers();
        bool a = startConsumer(0, tok[1].c_str(), true);
        bool b = startConsumer(1, tok[2].c_str(), true);
        s_consumerCount = (int)a + (int)b;
        Serial.printf("multi: %d consumers started\n", s_consumerCount);
    }
    else if (tok[0] == "stop") {
        stopAllConsumers();
        Serial.printf("All consumers stopped.\n");
    }
    else if (tok[0] == "stats") {
        printTelemetry();
    }
    else if (tok[0] == "reset") {
        stopAllConsumers();
        PageCache::instance().resetStats();
        Serial.printf("Stats reset.\n");
    }
    else if (tok[0] == "play" && tok[1].length()) {
        stopAllConsumers();
        AudioPlayer::instance().stop();
        AudioPlayer::instance().play(tok[1].c_str(), /*loop=*/false);
    }
    else if (tok[0] == "ploop" && tok[1].length()) {
        stopAllConsumers();
        AudioPlayer::instance().stop();
        AudioPlayer::instance().play(tok[1].c_str(), /*loop=*/true);
    }
    else if (tok[0] == "vol" && tok[1].length()) {
        AudioCodec::instance().setVolumeDb(tok[1].toFloat());
        Serial.printf("Volume set to %s dB\n", tok[1].c_str());
    }
    else if (tok[0] == "mute") {
        AudioCodec::instance().setMute(tok[1] == "on" || tok[1] == "1");
        Serial.printf("Mute: %s\n", tok[1].c_str());
    }
    else if (tok[0] == "help") {
        Serial.printf("Commands:\n"
                      "  ls [/path]               list directory\n"
                      "  sd                       SD card info\n"
                      "  bench <p> [chunkKB]      RAW SD read benchmark — bypasses page cache + audio pacing\n"
                      "                              tight fread loop, default 32 KB chunks, reports MB/s\n"
                      "  verbose on|off           page-cache LOAD/EVICT lines\n"
                      "  linear <p>               page-cache linear-scan benchmark (audio-paced 192 KB/s)\n"
                      "  loop   <p> <n>           page-cache loop benchmark\n"
                      "  multi  <pA> <pB>         page-cache two-consumer benchmark\n"
                      "  play   <p>               REAL audio playback (once)\n"
                      "  ploop  <p>               REAL audio playback (loop)\n"
                      "  vol    <dB>              codec volume (-100..+24, 0=ref)\n"
                      "  mute   on|off            codec mute\n"
                      "  stop / stats / reset\n");
    }
    else {
        Serial.printf("Unknown command: %s (try 'help')\n", cmd.c_str());
    }
}

static void pollSerial() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (s_line.length()) {
                Serial.printf("> %s\n", s_line.c_str());
                handleCommand(s_line);
                s_line = "";
            }
        } else if (s_line.length() < 512) {
            // 512 chars accommodates bench_mix_files with 8 long paths
            s_line += (char)c;
        }
    }
}

// ── setup / loop ─────────────────────────────────────────────────────

void setup() {
    // setRxBufferSize MUST come BEFORE Serial.begin() on ESP32 —
    // otherwise the buffer stays at the 256 B default and bench_mix_files
    // with 8 long paths gets truncated mid-token.
    Serial.setRxBufferSize(2048);
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n\n=== page_cache_test (ESP32-S3) ===\n");
    Serial.printf("Free PSRAM: %u KB, free heap: %u KB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));

    if (!mountSd()) {
        Serial.printf("[boot] cannot continue without SD — halting\n");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!PageCache::instance().begin(/*core=*/1,
                                      /*priority=*/configMAX_PRIORITIES - 3,
                                      /*stack=*/8192)) {
        Serial.printf("[boot] PageCache start failed — halting\n");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // ── Audio stack: codec → I2S → activate codec ────────────────────
    // Sequence is gated by the codec: TAS5825P needs to be in
    // DEEP_SLEEP (Phase 1), THEN have I²S clocks running, THEN we can
    // call activate() to take it through HIZ → PLAY.
    Wire.begin(Pins::I2C_SDA, Pins::I2C_SCL);
    if (AudioCodec::instance().begin(Wire)) {
        if (AudioI2S::instance().begin(Pins::I2S_BCLK, Pins::I2S_LRCK,
                                        Pins::I2S_DOUT, kSampleRate)) {
            AudioCodec::instance().activate(kSampleRate);
        }
    }

    Serial.printf("[boot] ready.  Type 'help' for commands.\n");
    s_lastTelemMs = millis();
}

void loop() {
    pollSerial();

    static uint32_t lastTick = 0;
    const uint32_t now = millis();
    if (now - lastTick >= 1000) {
        lastTick = now;
        if (s_consumerCount > 0 || AudioPlayer::instance().isPlaying()) {
            printTelemetry();
        }
    }
    delay(10);
}
