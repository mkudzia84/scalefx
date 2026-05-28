/*
 * audio_mp3_source.h — MP3-from-PSRAM IAudioSource.
 *
 *  Holds an AssetHandle pointing at the MP3 bitstream resident in
 *  PSRAM via AudioAssetCache, plus a leased HMP3Decoder from
 *  Mp3DecoderPool.  readFrames() pulls bytes from the asset, runs
 *  MP3Decode to produce 1152 stereo PCM frames at a time into a
 *  per-source scratch buffer, then converts int16 → float into the
 *  mixer's drain buffer.
 *
 *  Loop wrap: rewind the bitstream cursor to the first MP3 sync,
 *  reset the decoder (the pool's `acquire` after `release` gives a
 *  fresh context).  Cheap — same as gun-loop on the WAV path.
 *
 *  Threading: every method runs on the audio producer task.
 */

#ifndef SFX_AUDIO_MP3_SOURCE_H
#define SFX_AUDIO_MP3_SOURCE_H

#include "audio_source.h"
#include "audio_asset_cache.h"
#include "audio_mp3_decoder_pool.h"
#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_AUDIO)

class Mp3PsramSource final : public IAudioSource {
public:
    /// Acquire the MP3 asset from the cache, lease a decoder, find
    /// the first sync word, decode the first frame to extract format
    /// (sampleRate, channels).  Blocks briefly (≤ 2 s) on enough head
    /// bytes for the first decode.  Returns false on cache rejection,
    /// pool exhaustion, or invalid bitstream.
    bool open(const char* path);

    ~Mp3PsramSource() override;

    uint32_t sampleRate_Hz() const override { return _sampleRate_Hz; }
    uint16_t numChannels()   const override { return _numChannels; }
    uint16_t bitsPerSample() const override { return 16; }   // helix always 16-bit
    uint32_t totalFrames()   const override { return _totalFrames; }
    uint32_t framesRead()    const override { return _framesRead; }
    bool     isExhausted()   const override { return _exhausted; }

    uint32_t readFrames(float* outL, float* outR, uint32_t maxFrames) override;
    bool     pump() override { return false; }

    void setLoop(bool loop)        override { _loop = loop; }
    void setLoopCount(int n)       override { _loopCount = n; _loopCountInit = n; }
    int  loopCount() const         override { return _loopCount; }

    bool seekFrame(uint32_t frame) override;

    int      bufferFillPercent() const override;
    uint32_t bufferFrames()      const override;

private:
    /// Find the next MP3 sync word from `_bitPos`; on success
    /// advances `_bitPos` to the sync.  Returns false if not found
    /// (loader hasn't caught up or end of stream).
    bool findNextSync();

    /// Decode the next MP3 frame into `_pcmBuf`, populate `_pcmLen`
    /// (in stereo frames) + `_pcmPos = 0`.  Returns true on success.
    /// On error, sets `_exhausted` and returns false.
    bool decodeNextFrame();

    /// Re-init decoder + rewind bitstream for loop wrap.
    void rewindForLoop();

    // Asset + decoder
    AssetHandle      _asset;
    Mp3DecoderLease  _lease;

    // Bitstream cursor
    uint32_t _bitPos       = 0;     ///< byte offset into _asset.data()
    uint32_t _bitPosLoopBase = 0;   ///< first sync — loop wrap target

    // Per-frame PCM scratch (1152 stereo samples).  Pointer lives
    // inside the decoder lease (pool allocates).
    int16_t* _pcmBuf       = nullptr;
    int      _pcmLen       = 0;     ///< stereo frames currently in scratch
    int      _pcmPos       = 0;     ///< next frame to drain

    // Format (latched at open() from first decoded frame)
    uint32_t _sampleRate_Hz = 0;
    uint16_t _numChannels   = 0;
    uint32_t _totalFrames   = 0;   ///< estimated; MP3 has no exact frame count without scanning
    uint32_t _framesRead    = 0;

    // Loop / lifecycle
    bool     _loop          = false;
    int      _loopCount     = LOOP_INFINITE;
    int      _loopCountInit = LOOP_INFINITE;
    bool     _exhausted     = false;
    bool     _open          = false;
};

#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_AUDIO
#endif  // SFX_AUDIO_MP3_SOURCE_H
