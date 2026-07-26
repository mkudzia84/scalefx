/*
 * audio_mp3_decoder_pool.cpp — impl.
 */

#include "audio_mp3_decoder_pool.h"

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

#include <cstring>
#include <esp_heap_caps.h>
#include <serial/diag_log.h>

// Pull helix's C API.  Header lives under libhelix-mp3/ in the
// chmorgan/esp-libhelix-mp3 IDF component (pub/ is its include dir).
#include "mp3dec.h"

#define MP3POOL_LOG(fmt, ...)   SFX_LOG_INFO ("[Mp3Pool] " fmt, ##__VA_ARGS__)
#define MP3POOL_WARN(fmt, ...)  SFX_LOG_WARN ("[Mp3Pool] " fmt, ##__VA_ARGS__)
#define MP3POOL_ERROR(fmt, ...) SFX_LOG_ERROR("[Mp3Pool] " fmt, ##__VA_ARGS__)


// ── Lease impl ─────────────────────────────────────────────────────

Mp3DecoderLease::Mp3DecoderLease(Mp3DecoderLease&& o) noexcept
    : _slot(o._slot) {
    o._slot = nullptr;
}

Mp3DecoderLease& Mp3DecoderLease::operator=(Mp3DecoderLease&& o) noexcept {
    if (this != &o) {
        reset();
        _slot   = o._slot;
        o._slot = nullptr;
    }
    return *this;
}

Mp3DecoderLease::~Mp3DecoderLease() { reset(); }

void Mp3DecoderLease::reset() {
    if (!_slot) return;
    Mp3DecoderPool::instance().release(_slot);
    _slot = nullptr;
}

Mp3DecoderHandle Mp3DecoderLease::decoder() const {
    return _slot ? _slot->decoder : nullptr;
}

int16_t* Mp3DecoderLease::pcmBuf() const {
    return _slot ? _slot->pcm : nullptr;
}


// ── Pool impl ──────────────────────────────────────────────────────

Mp3DecoderPool::Mp3DecoderPool() {
    _muSem = xSemaphoreCreateMutex();
    if (!_muSem) {
        MP3POOL_ERROR("mutex create failed");
    }
}

Mp3DecoderPool::~Mp3DecoderPool() {
    for (auto& s : _slots) {
        if (s.decoder) {
            MP3FreeDecoder(static_cast<HMP3Decoder>(s.decoder));
            s.decoder = nullptr;
        }
        if (s.pcm) {
            heap_caps_free(s.pcm);
            s.pcm = nullptr;
        }
    }
    if (_muSem) vSemaphoreDelete(_muSem);
}

Mp3DecoderLease Mp3DecoderPool::acquire() {
    if (!_muSem) return Mp3DecoderLease{};

    xSemaphoreTake(_muSem, portMAX_DELAY);

    // Find a free slot.  Prefer already-allocated slots (just need
    // to reset their state); fall back to lazy-allocating a new one.
    Mp3DecoderSlot* victim = nullptr;
    for (auto& s : _slots) {
        if (s.inUse) continue;
        if (s.decoder) { victim = &s; break; }   // hot reuse
    }
    if (!victim) {
        for (auto& s : _slots) {
            if (!s.inUse && !s.decoder) { victim = &s; break; }
        }
    }
    if (!victim) {
        xSemaphoreGive(_muSem);
        MP3POOL_WARN("pool exhausted — %d slots all in use",
                     (int)AUDIO_MAX_CHANNELS);
        return Mp3DecoderLease{};
    }

    // Lazy-alloc decoder + PCM scratch on first use.
    if (!victim->decoder) {
        victim->decoder = MP3InitDecoder();
        if (!victim->decoder) {
            xSemaphoreGive(_muSem);
            MP3POOL_ERROR("MP3InitDecoder failed (out of internal SRAM?)");
            return Mp3DecoderLease{};
        }
        // PCM scratch lives in PSRAM (perf audit, instructions/34): the
        // decoder writes each output sample exactly once, sequentially, and
        // the drain-copy reads it once — bandwidth-trivial on octal PSRAM.
        // Frees ~4.6 KB of internal SRAM per active MP3 slot (~28 KB at
        // full 6-channel fan-out).  The decoder CONTEXT stays internal
        // (MP3InitDecoder mallocs it; the decode hot path hits it per
        // sample).  Internal fallback keeps PSRAM-less boards working.
        victim->pcm = (int16_t*)heap_caps_malloc(
            kMp3PcmScratchWords * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!victim->pcm)
            victim->pcm = (int16_t*)heap_caps_malloc(
                kMp3PcmScratchWords * sizeof(int16_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!victim->pcm) {
            MP3FreeDecoder(static_cast<HMP3Decoder>(victim->decoder));
            victim->decoder = nullptr;
            xSemaphoreGive(_muSem);
            MP3POOL_ERROR("PCM scratch alloc failed (%u B)",
                          (unsigned)(kMp3PcmScratchWords * sizeof(int16_t)));
            return Mp3DecoderLease{};
        }
        ++_allocated;
        MP3POOL_LOG("allocated slot %d of %d (decoder + %u B PCM scratch)",
                    _allocated, (int)AUDIO_MAX_CHANNELS,
                    (unsigned)(kMp3PcmScratchWords * sizeof(int16_t)));
    } else {
        // Hot reuse — reset decoder state by free+init.  libhelix's
        // HMP3Decoder doesn't expose a reset hook, so we cycle.  This
        // is fast (no heap fragmentation; same-size alloc reuses).
        MP3FreeDecoder(static_cast<HMP3Decoder>(victim->decoder));
        victim->decoder = MP3InitDecoder();
        if (!victim->decoder) {
            xSemaphoreGive(_muSem);
            MP3POOL_ERROR("MP3InitDecoder failed on reuse");
            return Mp3DecoderLease{};
        }
    }
    victim->inUse = true;
    xSemaphoreGive(_muSem);

    return Mp3DecoderLease{victim};
}

void Mp3DecoderPool::release(Mp3DecoderSlot* s) {
    if (!s || !_muSem) return;
    xSemaphoreTake(_muSem, portMAX_DELAY);
    s->inUse = false;
    xSemaphoreGive(_muSem);
}

int Mp3DecoderPool::activeCount() const {
    int n = 0;
    for (const auto& s : _slots) if (s.inUse) ++n;
    return n;
}

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
