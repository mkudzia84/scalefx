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
    mount.max_files              = 7;   // reader file-cache holds up to 4 + headroom
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

// ── Command parser ───────────────────────────────────────────────────

static String s_line;

static void handleCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    // Split on whitespace, up to 4 tokens
    String tok[4];
    int n = 0;
    int start = 0;
    while (n < 4 && start <= (int)cmd.length()) {
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
        } else if (s_line.length() < 192) {
            s_line += (char)c;
        }
    }
}

// ── setup / loop ─────────────────────────────────────────────────────

void setup() {
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
