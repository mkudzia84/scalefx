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
#include <esp_heap_caps.h>

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
    // 40 MHz HIGHSPEED — ~2x the SDMMC_FREQ_DEFAULT (20 MHz) data rate.
    // Combined with 4-bit slot width this gives a theoretical 20 MB/s.
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
        Serial.printf("SD: card_type=%s, max_freq=%u kHz, free PSRAM=%u KB\n",
                      s_card ? (s_card->is_mmc ? "MMC" : "SD") : "(none)",
                      s_card ? s_card->max_freq_khz : 0,
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    else if (tok[0] == "verbose") {
        const bool v = (tok[1] == "on" || tok[1] == "1");
        PageCache::instance().setVerbose(v);
        Serial.printf("Verbose page-cache logging: %s\n", v ? "ON" : "OFF");
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
                      "  ls [/path]              list directory\n"
                      "  sd                       SD card info\n"
                      "  verbose on|off           page-cache LOAD/EVICT lines\n"
                      "  linear <p>               page-cache linear-scan benchmark\n"
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
