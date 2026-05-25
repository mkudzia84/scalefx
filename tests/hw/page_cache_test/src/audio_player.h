/*
 * audio_player.h — single-channel paged WAV player.
 *
 * Open a WAV file, parse its RIFF/WAVE/fmt/data header, then run a
 * producer task on Core 1 (prio MAX-2) that:
 *
 *   1. Drains PCM bytes from the PageCache page covering the current
 *      file offset
 *   2. Pushes them straight into I2S via blocking write
 *
 *   I2S TX blocking naturally paces the producer at the codec's
 *   sample rate (192 KB/s for 48 kHz stereo16) — no SPSC ring
 *   needed for a single channel.  The page cache + reader task
 *   provides the SD-stall resilience.
 *
 * Looping
 * ───────
 *   `play(path, loop=true)` wraps back to byte 0 on EOF.  Loop
 *   wrap counts toward `loopCount`.
 *
 * Audio-side telemetry
 * ────────────────────
 *   Every 1 s the producer task prints a line:
 *     [Player] state=PLAYING pos=23456789/17496208 underruns=0 bytes_written=...
 *
 *   "underrun" here means: producer called PageCache::acquire, the
 *   page wasn't Ready, so the player wrote silence to I2S for that
 *   iteration.  Distinct from the cache-side underrun count (which
 *   counts every tick where ANY page wasn't ready).
 */

#ifndef AUDIO_PLAYER_TEST_H
#define AUDIO_PLAYER_TEST_H

#include "page_cache.h"
#include <atomic>
#include <cstdint>
#include <Arduino.h>

class AudioPlayer {
public:
    static AudioPlayer& instance() {
        static AudioPlayer inst;
        return inst;
    }

    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /// Start playback of `path` from the SD card.  Returns true if the
    /// file opened + WAV header parsed successfully.  Caller must have
    /// already started the I2S TX and codec (AudioI2S + AudioCodec).
    bool play(const char* path, bool loop);

    /// Stop playback.  Idempotent.  Producer task exits and frees the
    /// active PageRefs.
    void stop();

    /// True while the producer task is actively pushing samples.
    bool isPlaying() const { return _running.load(std::memory_order_acquire); }

    /// Diagnostics snapshot — read from any task / core.
    struct Stats {
        uint32_t bytesWritten;
        uint32_t fileSize;
        uint32_t cursor;
        uint32_t loopCount;
        uint32_t underruns;     ///< I2S silence inserts on missing page
        uint32_t maxStallMs;    ///< worst single-iteration stall
        bool     playing;
    };
    Stats snapshot();

private:
    AudioPlayer() = default;
    static void  taskFunc(void* arg);
    void         run();
    bool         parseWavHeader(const uint8_t* hdr, size_t hdrLen);

    char     _path[96]       = {};
    bool     _loop           = false;

    // WAV format (parsed from header)
    uint32_t _sampleRate     = 0;
    uint16_t _numChannels    = 0;
    uint16_t _bitsPerSample  = 0;
    uint32_t _dataStart      = 0;     ///< byte offset of first PCM sample
    uint32_t _dataBytes      = 0;     ///< total PCM byte length
    uint16_t _bytesPerFrame  = 0;

    // Runtime state
    std::atomic<bool>     _running     {false};
    std::atomic<uint32_t> _cursor      {0};
    std::atomic<uint32_t> _bytesWritten{0};
    std::atomic<uint32_t> _loopCount   {0};
    std::atomic<uint32_t> _underruns   {0};
    std::atomic<uint32_t> _maxStallMs  {0};

    TaskHandle_t _taskHandle = nullptr;
};

#endif  // AUDIO_PLAYER_TEST_H
