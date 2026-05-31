/*
 * audio_source.h — file-I/O abstraction for the audio mixer.
 *
 * Owns the boundary between "what bytes to play" and "how to mix them".
 * Concrete sources (streaming, preload, future paged) handle file open,
 * SD reads, PCM→float decode, loop-wrap, seek.  The mixer holds only
 * the resampler + mixing state and pulls source-rate stereo frames via
 * `readFrames()`.
 *
 * Source kinds (this header)
 * ──────────────────────────
 *   WavStreamSource   — SD-backed, keeps file handle open, refills
 *                       a per-channel float decode buffer on demand.
 *                       The default for medium-size WAVs.
 *   WavPreloadSource  — Whole-file PSRAM cache decoded at open() time;
 *                       file handle closed afterwards.  For short hot
 *                       samples (gun shots, alert beeps).
 *
 *   (Future) WavPagedSource — PageCache-backed for large files; lives
 *   in a separate header to keep this one paging-free for the Stage 2
 *   audio refactor that doesn't change behaviour.
 *
 * Storage
 * ───────
 *   Sources are placement-new'd into a small per-channel byte buffer
 *   (`Channel::sourceStorage`) sized to `kAudioSourceMaxSize`.  No heap
 *   allocations on play().  Destructor runs via `destroyAudioSource(src)`.
 *
 *   The streaming source borrows scratch buffers from the mixer
 *   (per-channel float decode bufL/R + shared SD batch buffer) via
 *   `StreamScratch`.  The preload source allocates its own PSRAM
 *   buffers on construction and frees them on destruction.
 *
 * Thread model
 * ────────────
 *   All methods are called from the producer task (Core 1 on ESP32,
 *   Core 0 on Pico).  SD access is gated by SdCardModule's mutex
 *   inside the source — the mixer never locks SD directly.
 *
 * Loop semantics
 * ──────────────
 *   Source owns loop state (setLoop/setLoopCount).  readFrames() handles
 *   loop wrap internally; callers see one continuous stream until the
 *   source returns 0 (counted-loop exhausted or one-shot EOF).
 */

#ifndef SFX_AUDIO_SOURCE_H
#define SFX_AUDIO_SOURCE_H

#include <cstdint>
#include <cstddef>
#include "platform/sfx_platform.h"

#if defined(SFX_HAS_AUDIO) && SFX_PLATFORM_ESP32

#include "storage/sd_card.h"

// ── IAudioSource — interface every concrete source implements ───────

class IAudioSource {
public:
    static constexpr int LOOP_INFINITE = -1;

    virtual ~IAudioSource() = default;

    // ── Format (immutable after construction) ────────────────────────
    virtual uint32_t sampleRate_Hz() const = 0;
    virtual uint16_t numChannels()   const = 0;
    virtual uint16_t bitsPerSample() const = 0;
    virtual uint32_t totalFrames()   const = 0;

    // ── Position ─────────────────────────────────────────────────────
    /// Source-rate frames consumed since the last loop wrap.  Resets
    /// to 0 when the source seeks back to the start of a looping file.
    virtual uint32_t framesRead()    const = 0;

    // ── Hot path ─────────────────────────────────────────────────────
    /// Decode up to `maxFrames` source-rate stereo frames into outL/outR.
    /// Returns count delivered (0 = end-of-stream).  Handles internal
    /// loop-wrap; the caller never sees a "loop seam" in returned data.
    /// Output is normalized to [-1.0, +1.0].
    virtual uint32_t readFrames(float* outL, float* outR,
                                uint32_t maxFrames) = 0;

    // ── Background work (Stage 4 hook) ───────────────────────────────
    /// Called by the producer task when it has slack.  Streaming and
    /// preload sources are no-ops; the future paged source uses this
    /// to kick off page-cache prefetch.  Returns true if work was done.
    virtual bool pump() { return false; }

    /// True iff the source is permanently done — counted-loop
    /// exhausted, one-shot EOF, or fatal read failure.  Used by the
    /// mixer to distinguish "this source will never deliver more
    /// frames" (destroy channel) from "this source is transiently
    /// buffering" (paged source waiting for the next page — emit
    /// silence this frame, try again next call).  Default true so
    /// sources that don't override get the conservative behaviour
    /// (any readFrames=0 treated as terminal).
    virtual bool isExhausted() const { return true; }

    // ── Loop control ─────────────────────────────────────────────────
    virtual void setLoop(bool loop)        = 0;
    virtual void setLoopCount(int n)       = 0;
    virtual int  loopCount() const         = 0;

    // ── Seek (used for startOffsetMs) ────────────────────────────────
    virtual bool seekFrame(uint32_t frame) = 0;

    // ── Diagnostics ─────────────────────────────────────────────────
    /// Implementations that buffer ahead report fill % (0..100).  The
    /// default 100 means "always ready" — preload source's answer.
    virtual int      bufferFillPercent() const { return 100; }
    virtual uint32_t bufferFrames()      const { return 0; }
};


// ── Shared helpers ──────────────────────────────────────────────────

/// Parse a 44-byte WAV / RIFF / fmt header, then scan for the "data"
/// chunk.  Caller holds the SD lock + has the file positioned at byte 0.
/// On success the file is left at the first data byte.
bool parseWavHeaderLocked(SdFile&    file,
                          uint32_t&  outSampleRate_Hz,
                          uint16_t&  outNumChannels,
                          uint16_t&  outBitsPerSample,
                          uint32_t&  outDataStart,
                          uint32_t&  outTotalFrames);

/// PCM (8-bit unsigned or 16-bit signed) → float decode kernel.
void decodePcmBatch(const uint8_t* src, int frames,
                    uint16_t numChannels, uint16_t bitsPerSample,
                    float* dstL, float* dstR, int dstFrameOffset);


// ── Scratch handed to the streaming source at construction ──────────
//
// Only a small PCM-byte staging buffer for fread().  WavStreamSource
// decodes DIRECTLY into the caller's float buffer — no internal float
// scratch, no aliasing with the mixer's drain buffer.  The original
// "share the mixer's bufL/R as the source's internal scratch" design
// turned out to be wrong: the source's memmove on its own scratch
// trashed the mixer's drained-but-unread frames, AND the subsequent
// memcpy from scratch to dest was overlapping-region undefined
// behaviour.  Fixed by letting the source decode directly to dest.

struct StreamScratch {
    uint8_t* sdReadBuf     = nullptr;     ///< shared PCM batch buffer (PSRAM)
    int      sdReadBufSize = 0;           ///< bytes
};


// ── WavStreamSource — SD-backed streaming ───────────────────────────
//
// No internal float buffer.  Each readFrames() call loops:
//   1. fread up to sdReadBufSize PCM bytes into sdReadBuf
//   2. decodePcmBatch directly into outL[delivered..] / outR[delivered..]
//   3. Repeat until delivered == maxFrames OR short-read OR EOS
// Loop wrap (file seek-to-dataStart + loopCount decrement) happens
// inside the same readFrames() call.

class WavStreamSource final : public IAudioSource {
public:
    /// Construct + open + parse header.  On failure leaves the source
    /// in the "not open" state (readFrames() returns 0 immediately).
    bool open(const char* path, const StreamScratch& scratch);

    ~WavStreamSource() override;

    uint32_t sampleRate_Hz() const override { return _sampleRate_Hz; }
    uint16_t numChannels()   const override { return _numChannels; }
    uint16_t bitsPerSample() const override { return _bitsPerSample; }
    uint32_t totalFrames()   const override { return _totalFrames; }
    uint32_t framesRead()    const override { return _framesRead; }

    uint32_t readFrames(float* outL, float* outR, uint32_t maxFrames) override;

    bool isExhausted() const override { return _exhausted; }

    void setLoop(bool loop) override        { _loop = loop; }
    void setLoopCount(int n) override       { _loopCount = n; _loopCountInit = n; }
    int  loopCount() const override         { return _loopCount; }

    bool seekFrame(uint32_t frame) override;

private:
    void closeFile();

    SdFile        _file;
    StreamScratch _scratch{};
    bool          _open = false;
    // SD mount epoch captured at open(). If a concurrent SD_INIT remounts the
    // card (bumping the epoch), `_file` is invalid; readFrames() detects the
    // mismatch and aborts rather than reading through a stale VFS handle.
    uint32_t      _openEpoch = 0;

    uint32_t _sampleRate_Hz = 0;
    uint16_t _numChannels   = 0;
    uint16_t _bitsPerSample = 0;
    uint32_t _dataStart     = 0;
    uint32_t _totalFrames   = 0;
    uint32_t _framesRead    = 0;

    bool     _loop          = false;
    int      _loopCount     = LOOP_INFINITE;
    int      _loopCountInit = LOOP_INFINITE;
    bool     _exhausted     = false;
};


// ── WavPreloadSource — entire file decoded into PSRAM ───────────────

class WavPreloadSource final : public IAudioSource {
public:
    /// Open + decode the file into PSRAM, then close the SD file
    /// handle.  `sdScratchBuf` is a temporary PCM staging buffer
    /// (NOT retained past return).  `maxPreloadFrames` caps the file
    /// size accepted (0 = no cap).  Returns true on success.
    bool openAndPreload(const char* path,
                        uint8_t* sdScratchBuf,
                        uint32_t sdScratchSize,
                        uint32_t maxPreloadFrames);

    ~WavPreloadSource() override;

    uint32_t sampleRate_Hz() const override { return _sampleRate_Hz; }
    uint16_t numChannels()   const override { return _numChannels; }
    uint16_t bitsPerSample() const override { return _bitsPerSample; }
    uint32_t totalFrames()   const override { return _totalFrames; }
    uint32_t framesRead()    const override { return _framesRead; }

    uint32_t readFrames(float* outL, float* outR, uint32_t maxFrames) override;
    bool     pump() override { return false; }

    bool isExhausted() const override { return _exhausted; }

    void setLoop(bool loop) override        { _loop = loop; }
    void setLoopCount(int n) override       { _loopCount = n; _loopCountInit = n; }
    int  loopCount() const override         { return _loopCount; }

    bool seekFrame(uint32_t frame) override;

private:
    void freeBuffers();

    float*   _bufL          = nullptr;
    float*   _bufR          = nullptr;

    uint32_t _sampleRate_Hz = 0;
    uint16_t _numChannels   = 0;
    uint16_t _bitsPerSample = 0;
    uint32_t _totalFrames   = 0;
    uint32_t _framesRead    = 0;

    bool     _loop          = false;
    int      _loopCount     = LOOP_INFINITE;
    int      _loopCountInit = LOOP_INFINITE;
    bool     _exhausted     = false;
};


// ── In-place storage size + destroy helper ──────────────────────────

// Sized for the largest source instance.  WavStreamSource carries an
// SdFile (fs::File on ESP32 = ~360 bytes including its internal shared
// state).  512 gives headroom for new sources (WavPsramSource +
// AssetHandle ≈ 80 B; future Mp3PsramSource will reuse this storage
// with a per-source PCM scratch ring).
constexpr size_t kAudioSourceMaxSize = 512;
constexpr size_t kAudioSourceAlign   = 8;

static_assert(sizeof(WavStreamSource)  <= kAudioSourceMaxSize,
              "Grow kAudioSourceMaxSize (WavStreamSource)");
static_assert(sizeof(WavPreloadSource) <= kAudioSourceMaxSize,
              "Grow kAudioSourceMaxSize (WavPreloadSource)");
static_assert(alignof(WavStreamSource)  <= kAudioSourceAlign, "alignment");
static_assert(alignof(WavPreloadSource) <= kAudioSourceAlign, "alignment");

// Static asserts for WavPsramSource + Mp3PsramSource live in their
// own .cpp files (forward-declared above) to avoid a circular include
// here (audio_psram_source.h / audio_mp3_source.h both depend on
// IAudioSource declared in this file).

/// Safe destructor that handles a nullptr.  Call this when tearing
/// down a channel — it invokes the source's virtual dtor (closes
/// files, frees preload bufs, etc.).
inline void destroyAudioSource(IAudioSource*& src) {
    if (src) {
        src->~IAudioSource();
        src = nullptr;
    }
}

#endif  // SFX_HAS_AUDIO && SFX_PLATFORM_ESP32
#endif  // SFX_AUDIO_SOURCE_H
