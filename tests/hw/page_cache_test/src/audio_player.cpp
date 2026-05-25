/*
 * audio_player.cpp — implementation.
 *
 * Producer task runs on Core 1 prio MAX-2 (one above the cache
 * reader at MAX-3) so it's always scheduled when there's work
 * AND data ready.  Cache reader runs in slack — exactly the model
 * we want to validate.
 *
 * Algorithm
 * ─────────
 *   while running:
 *     compute pageBase for _cursor
 *     ensure pageRef covers pageBase (acquire if not, slide nextRef in
 *       if it matches, otherwise fresh acquire)
 *     if !pageRef.isReady():
 *       record underrun, write silence (one DMA buffer), continue
 *     else:
 *       compute leftInPage, leftInFile
 *       take = min(leftInPage, leftInFile, kI2SChunkBytes)
 *       i2s_write(page->data + offset, take, blocking)
 *       _cursor += take
 *     if past 50% of page → acquire next page (prefetch)
 *
 * On EOF (cursor >= _dataStart + _dataBytes):
 *   if _loop: reset cursor + bump loopCount; pages may still be cached
 *   else: stop, signal done
 */

#include "audio_player.h"
#include "audio_i2s.h"

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

// I2S DMA frame size matches our chan_cfg.dma_frame_num=512.  We feed
// one DMA-buffer worth per i2s_write (512 frames × 4 bytes = 2 KB).
constexpr int kI2SChunkBytes = 512 * 4;

// Silence buffer for underrun fallback — also doubles as the
// "we know what we're outputting" probe (all zeros = no codec glitch
// from random PCM artifacts).
alignas(4) uint8_t kSilence[kI2SChunkBytes] = {0};

}  // namespace


// ── WAV header parser ─────────────────────────────────────────────────

bool AudioPlayer::parseWavHeader(const uint8_t* hdr, size_t hdrLen) {
    if (hdrLen < 44) return false;
    if (memcmp(hdr, "RIFF", 4) != 0)        return false;
    if (memcmp(hdr + 8,  "WAVE", 4) != 0)   return false;
    if (memcmp(hdr + 12, "fmt ", 4) != 0)   return false;

    const uint16_t audioFormat = hdr[20] | (hdr[21] << 8);
    if (audioFormat != 1) {
        Serial.printf("[Player] non-PCM WAV (audioFormat=%u) — unsupported\n",
                      audioFormat);
        return false;
    }
    _numChannels   = hdr[22] | (hdr[23] << 8);
    _sampleRate    = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    _bitsPerSample = hdr[34] | (hdr[35] << 8);
    if (_numChannels < 1 || _numChannels > 2) return false;
    if (_bitsPerSample != 16) {
        Serial.printf("[Player] only 16-bit PCM supported (got %u) — unsupported\n",
                      _bitsPerSample);
        return false;
    }
    _bytesPerFrame = _numChannels * (_bitsPerSample / 8);

    // Walk chunks looking for "data" — may be at variable offset.
    size_t pos = 12;
    while (pos + 8 <= hdrLen) {
        const uint32_t chunkSize = hdr[pos + 4] | (hdr[pos + 5] << 8) |
                                   (hdr[pos + 6] << 16) | (hdr[pos + 7] << 24);
        if (memcmp(hdr + pos, "data", 4) == 0) {
            _dataStart = pos + 8;
            _dataBytes = chunkSize;
            return true;
        }
        pos += 8 + chunkSize;
    }
    return false;
}


// ── play / stop ───────────────────────────────────────────────────────

bool AudioPlayer::play(const char* path, bool loop) {
    if (_running.load(std::memory_order_acquire)) {
        Serial.printf("[Player] already playing — stop first\n");
        return false;
    }

    // Read just enough of the file to parse the header.  Use POSIX
    // directly here (NOT the page cache) — header is one-shot, and
    // we want to avoid polluting the cache with a 1 KB read.
    char full[160];
    const bool needSlash = (path[0] != '/');
    snprintf(full, sizeof(full), "/sdcard%s%s", needSlash ? "/" : "", path);
    FILE* fp = fopen(full, "rb");
    if (!fp) {
        Serial.printf("[Player] fopen(%s) failed\n", full);
        return false;
    }
    uint8_t hdr[256];
    size_t  n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);
    if (!parseWavHeader(hdr, n)) {
        Serial.printf("[Player] invalid / unsupported WAV: %s\n", path);
        return false;
    }

    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';
    _loop = loop;
    _cursor.store(_dataStart, std::memory_order_relaxed);
    _bytesWritten.store(0, std::memory_order_relaxed);
    _loopCount.store(0, std::memory_order_relaxed);
    _underruns.store(0, std::memory_order_relaxed);
    _maxStallMs.store(0, std::memory_order_relaxed);
    _running.store(true, std::memory_order_release);

    BaseType_t r = xTaskCreatePinnedToCore(&AudioPlayer::taskFunc,
                                            "Player", 8192, this,
                                            configMAX_PRIORITIES - 2,
                                            &_taskHandle, /*core=*/1);
    if (r != pdPASS) {
        _running.store(false, std::memory_order_release);
        Serial.printf("[Player] task create failed\n");
        return false;
    }
    Serial.printf("[Player] playing %s — %u Hz / %uch / %ub, data %u B @ off=%u %s\n",
                  path, (unsigned)_sampleRate, (unsigned)_numChannels,
                  (unsigned)_bitsPerSample, (unsigned)_dataBytes,
                  (unsigned)_dataStart, loop ? "(LOOP)" : "(once)");
    return true;
}

void AudioPlayer::stop() {
    if (!_running.exchange(false, std::memory_order_acq_rel)) return;
    for (int i = 0; i < 50 && _taskHandle; ++i) vTaskDelay(pdMS_TO_TICKS(5));
    if (_taskHandle) { vTaskDelete(_taskHandle); _taskHandle = nullptr; }
    Serial.printf("[Player] stopped\n");
}

AudioPlayer::Stats AudioPlayer::snapshot() {
    Stats s{};
    s.bytesWritten = _bytesWritten.load(std::memory_order_acquire);
    s.fileSize     = _dataBytes;
    s.cursor       = _cursor.load(std::memory_order_acquire);
    s.loopCount    = _loopCount.load(std::memory_order_acquire);
    s.underruns    = _underruns.load(std::memory_order_acquire);
    s.maxStallMs   = _maxStallMs.load(std::memory_order_acquire);
    s.playing      = _running.load(std::memory_order_acquire);
    return s;
}


// ── Producer task ─────────────────────────────────────────────────────

void AudioPlayer::taskFunc(void* arg) {
    static_cast<AudioPlayer*>(arg)->run();
    vTaskDelete(nullptr);
}

void AudioPlayer::run() {
    PageRef curPage;
    PageRef nextPage;
    uint32_t curPageBase  = UINT32_MAX;
    uint32_t nextPageBase = UINT32_MAX;

    // Acquire the first page covering _dataStart.
    {
        const uint32_t base = (_dataStart / kMaxPageBytes) * kMaxPageBytes;
        const uint32_t fileEnd = _dataStart + _dataBytes;
        curPage = PageCache::instance().acquire(_path, base, fileEnd - base);
        curPageBase = base;
    }

    Serial.printf("[Player] task on core %d prio %d\n",
                  xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    while (_running.load(std::memory_order_acquire)) {
        const uint32_t tickStart = millis();
        uint32_t cursor   = _cursor.load(std::memory_order_acquire);
        const uint32_t fileEnd  = _dataStart + _dataBytes;

        // ── End-of-data: wrap or stop ─────────────────────────────
        if (cursor >= fileEnd) {
            if (_loop) {
                _cursor.store(_dataStart, std::memory_order_release);
                _loopCount.fetch_add(1, std::memory_order_acq_rel);
                // Reset page refs to re-acquire page[0] (likely cached).
                curPage.reset();
                nextPage.reset();
                curPageBase = nextPageBase = UINT32_MAX;
                continue;
            }
            // One-shot done.
            _running.store(false, std::memory_order_release);
            break;
        }

        // ── Ensure curPage covers `cursor` ────────────────────────
        const uint32_t pageBase = (cursor / kMaxPageBytes) * kMaxPageBytes;
        if (!curPage.isValid() || curPageBase != pageBase) {
            if (nextPage.isValid() && nextPageBase == pageBase) {
                curPage      = std::move(nextPage);
                curPageBase  = nextPageBase;
                nextPage     = PageRef{};
                nextPageBase = UINT32_MAX;
            } else {
                curPage.reset();
                curPage     = PageCache::instance().acquire(_path, pageBase,
                                                             fileEnd - pageBase);
                curPageBase = pageBase;
            }
        }

        // ── Underrun handling: write silence one DMA buffer ───────
        if (!curPage.isValid() || !curPage.isReady()) {
            if (curPage.isFailed()) {
                Serial.printf("[Player] FATAL page load failed at off=%u — stopping\n",
                              (unsigned)curPageBase);
                _running.store(false, std::memory_order_release);
                break;
            }
            _underruns.fetch_add(1, std::memory_order_acq_rel);
            // Silence keeps the I2S DMA + codec PLL alive.  ~10 ms.
            AudioI2S::instance().writeBytesBlocking(kSilence, kI2SChunkBytes);
            const uint32_t stall = millis() - tickStart;
            uint32_t prev = _maxStallMs.load(std::memory_order_relaxed);
            while (stall > prev &&
                   !_maxStallMs.compare_exchange_weak(prev, stall,
                                                      std::memory_order_relaxed)) {}
            continue;
        }

        // ── Real data: take a chunk from the page ────────────────
        const uint32_t byteInPage    = cursor - curPageBase;
        const uint32_t leftInPage    = curPage.size() - byteInPage;
        const uint32_t leftInFile    = fileEnd - cursor;
        uint32_t take = (leftInPage < (uint32_t)kI2SChunkBytes)
                        ? leftInPage : (uint32_t)kI2SChunkBytes;
        if (take > leftInFile) take = leftInFile;

        // i2s_write blocks until DMA accepts — paces this loop at the
        // codec's sample rate (192 KB/s for 48 kHz stereo16).
        const size_t wrote = AudioI2S::instance().writeBytesBlocking(
            curPage.data() + byteInPage, take);
        _bytesWritten.fetch_add(wrote, std::memory_order_acq_rel);
        _cursor.store(cursor + wrote, std::memory_order_release);

        // ── Prefetch: when curPage is >50% drained, kick the next ──
        const uint32_t consumedInPage = (cursor + wrote) - curPageBase;
        if (consumedInPage > (curPage.size() / 2)) {
            const uint32_t nextBase = curPageBase + curPage.size();
            if (nextBase < fileEnd &&
                (!nextPage.isValid() || nextPageBase != nextBase)) {
                nextPage     = PageCache::instance().acquire(_path, nextBase,
                                                              fileEnd - nextBase);
                nextPageBase = nextBase;
            }
        }
    }

    Serial.printf("[Player] task exit\n");
    curPage.reset();
    nextPage.reset();
    _taskHandle = nullptr;
}
