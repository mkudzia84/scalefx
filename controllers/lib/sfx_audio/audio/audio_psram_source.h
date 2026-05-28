/*
 * audio_psram_source.h — PSRAM-resident WAV + MP3 sources.
 *
 *  Both sources hold an `AssetHandle` from the `AudioAssetCache` and
 *  read from the cache's PSRAM buffer directly.  No SD I/O happens
 *  during playback — at worst, the source returns short when the
 *  background loader hasn't caught up yet.
 *
 *  WavPsramSource
 *  ──────────────
 *      Parses the WAV header (in-memory) on open, tracks read offset,
 *      decodes PCM → float into the mixer's output buffers.  Loop wrap
 *      is a single offset reset.  Zero copies beyond the format
 *      decode.
 *
 *  Mp3PsramSource (declared here, defined alongside the MP3 decoder
 *  pool when libhelix-mp3 lands)
 *      Holds the compressed bitstream as PSRAM bytes.  On open, leases
 *      a `HMP3Decoder` from `Mp3DecoderPool`.  readFrames() runs
 *      MP3Decode against a sliding window into the asset bytes,
 *      buffers 1152 stereo frames per decode call into a small per-
 *      source PCM scratch, then drains them.
 */

#ifndef SFX_AUDIO_PSRAM_SOURCE_H
#define SFX_AUDIO_PSRAM_SOURCE_H

#include "audio_source.h"
#include "audio_asset_cache.h"
#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

// ── WavPsramSource ───────────────────────────────────────────────────

class WavPsramSource final : public IAudioSource {
public:
    /// Acquire the asset from the cache and parse its WAV header.
    /// Blocks briefly (≤2 s) on the head bytes (≥44 + first data) so
    /// the format fields are valid before returning.  Subsequent
    /// reads stall transparently while the loader catches up.
    bool open(const char* path);

    ~WavPsramSource() override;

    uint32_t sampleRate_Hz() const override { return _sampleRate_Hz; }
    uint16_t numChannels()   const override { return _numChannels; }
    uint16_t bitsPerSample() const override { return _bitsPerSample; }
    uint32_t totalFrames()   const override { return _totalFrames; }
    uint32_t framesRead()    const override { return _framesRead; }
    bool     isExhausted()   const override { return _exhausted; }

    uint32_t readFrames(float* outL, float* outR, uint32_t maxFrames) override;
    bool     pump() override { return false; }   // loader does its own pacing

    void setLoop(bool loop)        override { _loop = loop; }
    void setLoopCount(int n)       override { _loopCount = n; _loopCountInit = n; }
    int  loopCount() const         override { return _loopCount; }

    bool seekFrame(uint32_t frame) override;

    int      bufferFillPercent() const override;
    uint32_t bufferFrames()      const override;

private:
    AssetHandle _asset;

    // Format
    uint32_t _sampleRate_Hz = 0;
    uint16_t _numChannels   = 0;
    uint16_t _bitsPerSample = 0;
    uint32_t _dataStart     = 0;     ///< byte offset of "data" chunk payload
    uint32_t _dataBytes     = 0;     ///< bytes in the data chunk
    uint32_t _totalFrames   = 0;
    uint8_t  _bytesPerFrame = 0;

    // Position
    uint32_t _readBytes     = 0;     ///< file byte offset (>= _dataStart)
    uint32_t _framesRead    = 0;     ///< source-rate frames since last wrap

    // Loop / lifecycle
    bool     _loop          = false;
    int      _loopCount     = LOOP_INFINITE;
    int      _loopCountInit = LOOP_INFINITE;
    bool     _exhausted     = false;
    bool     _open          = false;
};


// ── Mp3PsramSource (forward declaration; defined when helix lands) ──

class Mp3PsramSource;     // see audio_mp3_source.h once libhelix is in

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
#endif  // SFX_AUDIO_PSRAM_SOURCE_H
