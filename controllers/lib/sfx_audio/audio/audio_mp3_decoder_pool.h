/*
 * audio_mp3_decoder_pool.h — shared HMP3Decoder pool.
 *
 *  libhelix-mp3 keeps ~9 KB of decoder state per instance.  At
 *  AUDIO_MAX_CHANNELS=6 concurrent MP3 sources, 6 × 9 = 54 KB of
 *  decoder context plus 6 × 4.6 KB of PCM scratch (one decoded frame
 *  = 1152 stereo samples × 2 bytes/sample × 2 channels).  All of it
 *  in internal SRAM (decoder context can't live in PSRAM — the
 *  decoder hot path touches every sample).
 *
 *  Lazy allocation:
 *      Slots are constructed on first `acquire()` and stay alive for
 *      the firmware's lifetime.  Releasing returns the slot to the
 *      pool but does NOT free the decoder — re-acquire is a single
 *      `MP3InitDecoder()`-style reset on the existing context.
 *
 *  Threading:
 *      acquire / release run on the audio producer task (Core 1).
 *      Single mutex; small critical section.
 */

#ifndef SFX_AUDIO_MP3_DECODER_POOL_H
#define SFX_AUDIO_MP3_DECODER_POOL_H

#include <cstdint>
#include <cstddef>
#include "platform/sfx_platform.h"
#include "audio_config.h"

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Helix's HMP3Decoder is a `void*` so we don't need to pull mp3dec.h
// into every consumer.  The implementation file casts when needed.
using Mp3DecoderHandle = void*;

/// One pool slot.  Owns its decoder context + per-source PCM scratch.
struct Mp3DecoderSlot {
    Mp3DecoderHandle  decoder = nullptr;    ///< HMP3Decoder, lazy-alloc
    int16_t*          pcm     = nullptr;    ///< 1152 × 2 = 2304 int16, ~4.6 KB
    bool              inUse   = false;
};

/// Per-source PCM scratch length in stereo samples (the max output
/// of one libhelix-mp3 MP3Decode call for Layer III, 2 channels).
constexpr int kMp3PcmScratchSamples = 1152;

/// Per-source PCM scratch length in int16 words (samples × channels).
constexpr int kMp3PcmScratchWords   = kMp3PcmScratchSamples * 2;


class Mp3DecoderLease {
public:
    Mp3DecoderLease() = default;
    Mp3DecoderLease(const Mp3DecoderLease&)            = delete;
    Mp3DecoderLease& operator=(const Mp3DecoderLease&) = delete;
    Mp3DecoderLease(Mp3DecoderLease&&) noexcept;
    Mp3DecoderLease& operator=(Mp3DecoderLease&&) noexcept;
    ~Mp3DecoderLease();

    bool isValid() const { return _slot != nullptr; }

    /// Raw HMP3Decoder for direct MP3Decode() calls.
    Mp3DecoderHandle decoder() const;

    /// PCM scratch (1152 stereo frames worth, 16-bit interleaved L/R).
    int16_t* pcmBuf() const;

    /// Drop the lease (returns the slot to the pool).
    void reset();

private:
    friend class Mp3DecoderPool;
    explicit Mp3DecoderLease(Mp3DecoderSlot* s) : _slot(s) {}
    Mp3DecoderSlot* _slot = nullptr;
};


class Mp3DecoderPool {
public:
    static Mp3DecoderPool& instance() {
        static Mp3DecoderPool inst;
        return inst;
    }

    Mp3DecoderPool(const Mp3DecoderPool&)            = delete;
    Mp3DecoderPool& operator=(const Mp3DecoderPool&) = delete;

    /// Acquire a slot.  Lazy-allocates the decoder + PCM scratch the
    /// first time a slot is needed.  Returns an invalid lease if the
    /// pool is full (every AUDIO_MAX_CHANNELS slot is in use).  The
    /// returned decoder's internal state is FRESH — caller does not
    /// need to re-init.
    Mp3DecoderLease acquire();

    /// Number of slots currently leased (diagnostic).
    int activeCount() const;

    /// Total slot allocations done so far (diagnostic).
    int allocatedSlots() const { return _allocated; }

private:
    Mp3DecoderPool();
    ~Mp3DecoderPool();
    friend class Mp3DecoderLease;
    void release(Mp3DecoderSlot* s);

    Mp3DecoderSlot    _slots[AUDIO_MAX_CHANNELS];
    int               _allocated = 0;
    SemaphoreHandle_t _muSem     = nullptr;
};

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
#endif  // SFX_AUDIO_MP3_DECODER_POOL_H
