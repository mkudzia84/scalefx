/**
 * Audio Mixer — SPSC Ring Buffer Architecture
 * 
 * Singleton software audio mixer with I2S output.
 * Supports 8 simultaneous channels with WAV playback, per-channel volume,
 * loop/one-shot modes, L/R/stereo routing, and float mixing pipeline.
 * 
 * DUAL-CORE ARCHITECTURE (ESP32-S3):
 *   Core 1 — Producer Task: WAV decode + SD reads + int16 Q15 mixing → SPSC ring buffer
 *   Core 1 — Consumer Task: SPSC ring buffer → I2S DMA (higher priority)
 *   Core 0: Protocol handling only — uses *Async() API to queue commands
 *
 * Mix kernel runs in int16 / Q15 fixed-point (Phase 5,
 * feature/mixer-int16-kernel — the source PCM is already int16 native,
 * and the per-frame hot loop dominates producer cost at 2+ active
 * channels).  Sources still emit float via IAudioSource::readFrames();
 * the per-channel drain buffer is filled int16 once at refill time and
 * the mixer reads int16 from then on.  Linear-interp resampling for
 * non-native sample rates is done in Q15 fixed-point.
 * 
 * Uses SdCardModule singleton for SD card access (producer side only).
 * Audio files are opened directly via SdCardModule::instance().
 * 
 * Usage:
 *   using Mixer = AudioMixer<EspI2SOutput, TAS5825PCodec>;
 *   Mixer::instance().begin(i2s_data, i2s_bclk, i2s_lrclk);
 *   // Core 1 consumer task: Mixer::instance().consume();
 *   // Launch producer on Core 1: Mixer::instance().startProducerTask();
 *   // Core 0 protocol: use Mixer::instance().playAsync() etc.
 */

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include <Arduino.h>
#include <atomic>
#include <concepts>
#include "platform/sfx_platform.h"
#include "storage/sd_card.h"

// Include centralized audio configuration
#include "audio_config.h"
#include "audio_ring_buffer.h"
#include "audio_source.h"
#include "audio_psram_source.h"
#include "audio_mp3_source.h"

// ============================================================================
//  CONSTANTS (see audio_config.h for configuration)
// ============================================================================

// Per-channel decoded-PCM drain ring (int16 frames per channel — Q15
// fixed-point).  Sized for the lock-free SPSC ring buffer pattern
// introduced in `feature/audio-decode-prefetch` (Phase 6): the decoder
// task on Core 0 fills these rings asynchronously; the producer task on
// Core 1 only reads from them.  Cross-core synchronization is via
// atomic readIdx / writeIdx pairs — no mutex on the wire-rate path.
//
// PSRAM budget summary (8 MB available on ESP32-S3-WROOM-1 N8R8):
//   AssetCache  6144 KB ceiling (`kAssetCacheBudgetBytes`; actual ≈ 3 MB
//               in typical configs — MP3 source bytes, not decoded PCM)
//   Drain rings  512 KB (8 ch × 2 L/R × 16384 frames × 2 B; Phase 6,
//               down from 768 KB at Phase 5 to free up 256 KB and align
//               the size to a power of 2 for efficient ring masking)
//   Mixer ring    16 KB  (4096 stereo frames; producer→consumer FIFO)
//   Decoder pool ~60 KB  (Mp3DecoderPool slots, lazy alloc)
//   Misc         ~80 KB  (per-ch queues, command queue, SD scratch
//                         fallback, DiagLog ring)
//   ─────────────────────
//   Ceiling     ~6812 KB → ~1.4 MB headroom at full cache load.
//
// Sizing rationale for the drain ring (Phase 6, 2026-05-28):
//   - Worst-case Core 0 preemption (Arduino loop + protocol burst + a
//     long-running file upload chunk): ~80 ms.  At 48 kHz that's 3840
//     frames drained per channel before the decoder task gets back.
//   - One decode round-trip (~30 ms for an MP3 frame block on Core 0):
//     adds another 1440 frames.
//   - Required ring headroom: ~5300 frames (~110 ms).
//   - Picking 16384 (341 ms) gives a 3× safety margin, is a power of 2
//     for fast (mask-based) ring math, and trims 256 KB from the old
//     24000-frame drain — that 256 KB stays free as PSRAM margin
//     (could go to a larger AssetCache budget if/when needed).
//
// Power-of-2 invariant: code in audio_mixer.ipp assumes
//   `(idx & (WAV_BUF_FRAMES - 1))` is valid mask-modulo, so this
//   constant MUST stay power-of-2 on ESP32.
//
// Pico unchanged: 1024 frames in SRAM heap.
//
// SD read batch (`WAV_SD_READ_BYTES`): 32 KB in DMA-cap internal SRAM
// (re-tried 2026-05-27 after the multi-track stutter was reassessed
// as an SD-card-side issue rather than a cache-coherency problem on
// the audio path).  SDMMC DMAs straight into the buffer — no PSRAM
// bounce — lifting raw SD throughput from ~1 MB/s to ~14 MB/s.  The
// audio_mixer.ipp alloc path falls back to PSRAM with a WARN log if
// the DMA-cap pool is exhausted.
#if SFX_PLATFORM_ESP32
constexpr int WAV_BUF_FRAMES       = 16384;     // decoded int16 frames per track (PSRAM, 341 ms @ 48 kHz, PO2)
constexpr int WAV_SD_READ_BYTES    = 32768;     // bytes per SD card read batch (DMA-cap SRAM)
// Hard cap for `AudioPlaybackOptions::preloadIntoMemory`: 96 000 source
// frames = 2 s @ 48 kHz stereo = 768 KB allocated from PSRAM per
// preloaded channel.  At 8 MB PSRAM and at most ~4 hot looping samples
// (gun A/B + alert + spare) the worst-case is ~3 MB additional — safe
// margin over the AssetCache + drain-ring pool.  Files larger than the
// cap fall back to streaming with a single warn log.
constexpr uint32_t WAV_MAX_PRELOAD_FRAMES = 96000;
#else
constexpr int WAV_BUF_FRAMES       = 1024;      // decoded int16 frames per track
constexpr int WAV_SD_READ_BYTES    = 4096;      // bytes per SD card read batch
// Pico's SRAM heap is far smaller — disable preload entirely (cap=0
// makes every preloadIntoMemory request fall back to streaming).
constexpr uint32_t WAV_MAX_PRELOAD_FRAMES = 0;
#endif

// Max amplitude for final int16 output (headroom for mixing)
constexpr int16_t MAX_AMPLITUDE    = 24000;     // ~73% of 32767

// Command queue for async operations (Core 0 → producer)
constexpr int AUDIO_CMD_QUEUE_SIZE = 16;

// ============================================================================
//  TYPES
// ============================================================================

// Output channel bitmask — each bit enables one physical output channel.
// Extensible: future boards may define CH3=0x04, CH4=0x08, etc.
namespace AudioChannel {
    constexpr uint8_t CH1 = 0x01;          // Output channel 1
    constexpr uint8_t CH2 = 0x02;          // Output channel 2
    constexpr uint8_t ALL = CH1 | CH2;     // All channels (default)
}

enum class AudioStopMode : uint8_t {
    Immediate = 0,
    Fade      = 1,
    LoopEnd   = 2
};



// Behavior when a queued item replaces a looping track
enum class QueueLoopBehavior : uint8_t {
    StopImmediate = 0,    // Stop loop immediately, start queued item
    FinishLoop    = 1     // Let current loop iteration finish, then play queued item
};

// Special value for infinite looping
constexpr int LOOP_INFINITE = -1;

struct AudioPlaybackOptions {
    bool loop              = false;       // Legacy: simple loop on/off
    int loopCount          = LOOP_INFINITE;  // Number of loops: -1=infinite, 0=no loop, N=N loops
    float volume           = 1.0f;
    uint8_t outputChannels = AudioChannel::ALL;
    int startOffsetMs      = 0;
    uint16_t fadeInMs      = 0;           // 0 = start at full volume; >0 = ramp 0→volume over this many ms
    uint16_t fadeOutMs     = 0;           // 0 = play to the last sample; >0 = ramp volume→0 over the final this-many ms (one-shots only; ignored when looping)
    // Preload-into-memory (2026-05-23): request the entire WAV be decoded
    // into a dedicated PSRAM buffer at play() time so subsequent loops
    // don't touch SD.  Caller is asking "this sample is short and will
    // loop hot — cache it".  Gated by `WAV_MAX_PRELOAD_FRAMES` — files
    // larger than the cap silently fall back to streaming (with a warn
    // log so the operator knows why their request was ignored).
    // Intended for sustained-fire gun shots, alert tones, short engine
    // burst stingers; NOT for long ambient loops (engine running,
    // background music) which would blow the cap and stream anyway.
    bool preloadIntoMemory = false;
};

// Queued sound item
struct QueuedSound {
    char filename[128]            = {};
    AudioPlaybackOptions options  = {};
    QueueLoopBehavior loopBehavior = QueueLoopBehavior::StopImmediate;
    bool valid                    = false;
};

// ============================================================================
//  MixerLike concept
// ============================================================================
//
// Compile-time contract every type passed as `TMixer` to consumers
// (EspDualCoreAudio, AudioServicePolicy, EngineFx, GunFx, Alert)
// satisfies.  `AudioMixer<TI2S, TCodec>` is the canonical model — the
// concept is named `MixerLike` to avoid colliding with the class name.
//
// Per Rule 17 / 33: the concept lives next to the type it constrains
// so a maintainer extending the mixer sees the consumer contract in
// the same file.
//
template <typename T>
concept MixerLike = requires(T& m, int ch, const char* path,
                             AudioPlaybackOptions opts,
                             AudioStopMode stopMode) {
    // Singleton accessor — every mixer-like type exposes one.
    { T::instance() } -> std::same_as<T&>;

    // Nested typedefs for codec / I²S backend introspection (used by
    // `EspDualCoreAudio<TMixer>` to recover the codec type via the
    // CodecAdapter trait — see Rule 33).
    typename T::Codec;
    typename T::I2SOutput;

    // Async runtime surface.  Effects (EngineFx, GunFx, Alert) call
    // these from their wire-handler and update() ticks.
    { m.playAsync(ch, path, opts) } -> std::convertible_to<bool>;
    { m.stopAsync(ch, stopMode) };
    { m.isPlaying(ch) }            -> std::convertible_to<bool>;
};

// ============================================================================
//  AUDIO MIXER CLASS (Singleton, templatized on I2S output and codec)
// ============================================================================

/**
 * @tparam TI2S   Concrete I2SOutput singleton (must have static instance())
 * @tparam TCodec Concrete AudioCodec singleton (must have static instance())
 */
template<typename TI2S, typename TCodec>
class AudioMixer {
public:
    /// The I²S output backend / codec types are exposed as nested typedefs
    /// so helpers parameterized on `TMixer` alone (e.g. `EspDualCoreAudio<>`)
    /// can recover them without an extra template parameter.
    using I2SOutput = TI2S;
    using Codec     = TCodec;

    /**
     * Get the singleton instance
     */
    static AudioMixer& instance() {
        static AudioMixer instance;
        return instance;
    }
    
    // Delete copy/move constructors and assignment operators
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) = delete;
    AudioMixer& operator=(AudioMixer&&) = delete;
    
    ~AudioMixer();

    // ---- Initialization (TWO-PHASE for dual-core) ----
    // Phase 1: Call begin() from Core 0 — channels, ring buffer, PSRAM alloc, codec, mutex
    // Phase 2: Call beginI2S() from Core 1 — I2S hardware init via TI2S::instance()
    // TI2S and TCodec are accessed as singletons — no DI needed
    bool begin(uint8_t i2s_data_pin, uint8_t i2s_bclk_pin,
               uint8_t i2s_lrclk_pin);
    bool beginI2S();  // Must be called from Core 1!
    void shutdown();



    // ---- Playback Control ----
    bool play(int channel, const char* filename, const AudioPlaybackOptions& options = {});
    void stop(int channel, AudioStopMode mode = AudioStopMode::Immediate);
    void stopWithFadeMs(int channel, AudioStopMode mode, uint16_t fadeMs);
    void stopAll(AudioStopMode mode = AudioStopMode::Immediate);
    void stopLooping(int channel);
    void stopLoopingAll();

    // ---- Queue Control ----
    bool queueSound(int channel, const char* filename, const AudioPlaybackOptions& options = {},
                    QueueLoopBehavior loopBehavior = QueueLoopBehavior::StopImmediate);
    void clearQueue(int channel);
    void clearAllQueues();
    int queueLength(int channel) const;
    bool hasQueuedSounds(int channel) const;

    // ---- Volume & Routing ----
    void setVolume(int channel, float volume);
    void setMasterVolume(float volume);
    void setOutputChannels(int channel, uint8_t channelMask);
    float volume(int channel) const;
    float masterVolume() const { return _masterVolume; }

    // ---- Status ----
    bool isPlaying(int channel) const;
    bool isAnyPlaying() const;
    bool isInitialized() const { return _initialized; }
    bool isI2SRunning() const  { return _i2sRunning; }
    float remainingSec(int channel) const;
    TCodec& getCodec() { return TCodec::instance(); }
    TI2S& getI2SOutput() { return TI2S::instance(); }

    // ---- Dual-Core Processing ----
    // Producer: WAV decode + SD reads + mixing → ring buffer
    // On ESP32: runs as dedicated FreeRTOS task on Core 1 via startProducerTask()
    // On Pico: call produce() from loop (single-core)
    int produce(int maxFrames = 256);

    /// Block-mode producer (Phase 5 of feature/idf-component-build,
    /// 2026-05-28).  Produces up to `maxFrames` (capped by `kBlockMax`
    /// = 256 and by available ring-buffer space) using esp-dsp's
    /// hand-tuned Xtensa LX7 SIMD for the per-channel scale step.
    /// Replaces the per-frame `produceFrame()` on ESP32; the legacy
    /// per-frame path remains for Pico + tests where esp-dsp isn't
    /// available.
    int produceBlock(int maxFrames = 256);

    /// Float-kernel variant of `produceBlock()` — same block-mode
    /// architecture but the per-channel scale + accumulate runs in
    /// float32 via `dsps_mulc_f32_ae32` + `dsps_add_f32_ae32`.
    /// Comparison experiment with `produceBlock()` (int16 / Q15):
    /// trades 2× memory bandwidth (4 B vs 2 B per sample) + an
    /// int16→float conversion at drain-read time, against simpler
    /// per-sample math (no overflow management, native FPU on the
    /// LX7).  Whichever wins on the same gun-during-engine 2-channel
    /// trace becomes the production kernel.  Selected at compile time
    /// via the `SFX_AUDIO_KERNEL_FLOAT` macro (see produce() body in
    /// audio_mixer.ipp).
    int produceBlockFloat(int maxFrames = 256);
    
    // Consumer (Core 1 task): ring buffer → I2S DMA
    void consume();
    
    // Legacy single-call (for backwards compatibility, calls both)
    void process();

#if SFX_PLATFORM_ESP32
    // ---- Producer Task (ESP32 FreeRTOS) ----
    // Launches a dedicated producer task pinned to the specified core.
    // The task runs produce() in a loop, yielding when the ring is full
    // or no channels are playing. Consumer task should be higher priority.
    bool startProducerTask(int core = 1,
                           int priority = configMAX_PRIORITIES - 2,
                           int stackSize = 8192);
    void stopProducerTask();
    bool isProducerTaskRunning() const {
        return _producerRunning.load(std::memory_order_acquire);
    }

    /// Phase 6 (feature/audio-decode-prefetch): start the decoder
    /// worker task on Core 0.  Idempotent.  Must be called after
    /// begin() so the per-channel rings exist.  Optional — without
    /// it the legacy in-producer refill path (now removed) would
    /// have decoded; with no decoder task the channels stay silent
    /// after play().
    bool startDecoderTask();
    void stopDecoderTask();
    bool isDecoderTaskRunning() const {
        return _decoderRunning.load(std::memory_order_acquire);
    }

    /**
     * @brief Store the consumer task handle for suspend/resume coordination.
     *
     * Call once after creating the Core 1 audio consumer task.
     * Required before calling suspendAudio() / resumeAudio().
     */
    void setConsumerTaskHandle(TaskHandle_t handle) {
        _consumerTaskHandle = handle;
    }

    /**
     * @brief Suspend all audio processing (consumer + producer).
     *
     * Stops all playback, stops the producer task, and suspends the
     * consumer task. Use when Core 1 needs to be freed for competing
     * work (e.g., SD card uploads). Call resumeAudio() to restart.
     *
     * Requires setConsumerTaskHandle() to have been called first.
     */
    void suspendAudio();

    /**
     * @brief Resume audio processing after suspendAudio().
     *
     * Resumes the consumer task and restarts the producer task with
     * the same configuration used in the original startProducerTask() call.
     */
    void resumeAudio();
#endif

    // Stats access
    uint32_t getUnderruns() const { return _underruns.load(std::memory_order_acquire); }
    void resetUnderruns() { _underruns.store(0, std::memory_order_relaxed); }
    uint32_t getConsumeLoops() const { return _consumeLoops.load(std::memory_order_acquire); }
    uint32_t getConsumeFrames() const { return _consumeFrames.load(std::memory_order_acquire); }
    
    // Ring buffer stats
    uint32_t getRingAvailableRead() const;
    uint32_t getRingAvailableWrite() const;
    int getRingFillPercent() const;
    uint32_t getRingCapacity() const { return RING_FRAMES; }

    // Per-channel WAV decode buffer stats
    int getWavBufferFillPercent(int channel) const;
    int getWavBufferFrames(int channel) const;       // valid frames in buffer
    int getWavBufferCapacity() const { return WAV_BUF_FRAMES; }

    // Async commands (thread-safe for dual-core)
    bool playAsync(int channel, const char* filename, const AudioPlaybackOptions& options = {});
    void stopAsync(int channel, AudioStopMode mode = AudioStopMode::Immediate);

    /// Async fade-out with a custom duration.  Ramps the channel's
    /// volume to 0 over `fadeMs` then destroys the source.  Used by
    /// EngineFx's cross-fade transitions where the standard
    /// AudioStopMode::Fade's hardcoded 50 ms is too short.  Effectively
    /// `stopAsync(ch, Fade)` with a configurable ramp.
    void stopAsyncWithFadeMs(int channel, uint16_t fadeMs);
    void setVolumeAsync(int channel, float volume);
    void setMasterVolumeAsync(float volume);
    bool queueSoundAsync(int channel, const char* filename, const AudioPlaybackOptions& options = {},
                         QueueLoopBehavior loopBehavior = QueueLoopBehavior::StopImmediate);
    void clearQueueAsync(int channel);

    // ---- Channel Info Access ----
    const char* getFilename(int channel) const;
    float getChannelVolume(int channel) const;
    bool isLooping(int channel) const;
    int getLoopCount(int channel) const;       // Current remaining loops
    int getInitialLoopCount(int channel) const; // Initial loop count
    uint8_t getOutputChannels(int channel) const;
    uint32_t getSampleRate(int channel) const;
    uint16_t getNumChannels(int channel) const;
    uint16_t getBitsPerSample(int channel) const;
    uint32_t getTotalSamples(int channel) const;

private:
    // ---- Internal Types ----
    static constexpr int CHANNEL_FILENAME_MAX = 64;
    static constexpr int QUEUE_SIZE_PER_CHANNEL = 4;  // Max queued sounds per channel
    
    // Per-channel WAV-side state.
    //
    // Phase 6 (feature/audio-decode-prefetch, 2026-05-28): the drain
    // buffer is a lock-free SPSC ring shared between the decoder task
    // on Core 0 (single writer; calls source->readFrames() + converts
    // float→int16 + writes to bufL/R) and the producer task on Core 1
    // (single reader; getWavSample drains samples for the mix kernel).
    // Cross-core sync via the writeIdx/readIdx atomics with
    // acquire-release ordering — no mutex on the wire-rate path.
    //
    // `WAV_BUF_FRAMES` is power-of-2, so wrap is a single AND with
    // `(WAV_BUF_FRAMES - 1)` — no modulo division on the hot path.
    //
    // Lifecycle:
    //   active=false → ring is dormant; decoder skips it
    //   play()       → sets up source + format fields, RESETS the ring
    //                  indices to 0, marks active=true, notifies decoder
    //   decoder      → fills ring up to ~80% (kept low to avoid OOM if
    //                  many channels active concurrently), loops
    //   stop()/EOF   → clears active; decoder destroys source, leaves
    //                  ring drained for the producer to flush
    struct WavState {
        // Active source — placement-new'd into Channel::sourceStorage
        // at play() time; destroyed via destroyAudioSource() on
        // stop/EOF/fade-end.  Read by both tasks; writes are coordinated
        // through the `active` flag + the decoder being the one that
        // calls destructors.
        IAudioSource* source = nullptr;

        // Cached WAV format (set once at play() — immutable for the
        // lifetime of this source instance, read on every status query).
        uint32_t   sampleRate_Hz  = 0;
        uint16_t   numChannels    = 0;
        uint16_t   bitsPerSample  = 0;
        uint32_t   totalFrames    = 0;

        // Initial loop count snapshot — source's loopCount() decays
        // toward 0 as iterations complete, so we keep the request here.
        int        loopCountInit  = 0;

        // Linear-interpolation resampler — output-rate accumulator.
        // Producer-owned; not touched by the decoder.
        float      resampleRatio  = 1.0f;
        float      resampleFrac   = 0.0f;

        // Per-channel int16 SPSC ring.  PSRAM-allocated at
        // AudioMixer::begin(), capacity = WAV_BUF_FRAMES (power of 2).
        // Decoder task on Core 0 writes (advances writeIdx); producer
        // task on Core 1 reads (advances readIdx).
        int16_t*   bufL           = nullptr;
        int16_t*   bufR           = nullptr;

        // SPSC indices.  Monotonic 32-bit counters; wrap implicit via
        // the size-mask `WAV_BUF_FRAMES - 1`.  Acquire-release pairs
        // guarantee that writes to bufL/R observed by the producer
        // happen-before the matching writeIdx increment it sees.
        //
        // NB: WAV_BUF_FRAMES must stay power-of-2 for these helpers to
        // be valid — static_assert lives in audio_mixer.ipp's begin().
        std::atomic<uint32_t> writeIdx{0};   // decoder writes, producer reads (acquire)
        std::atomic<uint32_t> readIdx{0};    // producer writes, decoder reads (acquire)

        // ── SPSC ring helpers ────────────────────────────────────────
        //
        // Producer-side: `availableRead()` returns the number of
        // freshly-decoded frames waiting to be mixed.  Acquire on
        // writeIdx pairs with the decoder's release after it finishes
        // writing the corresponding bufL/R cells.  readIdx is loaded
        // relaxed because the producer is its sole writer.
        //
        // Decoder-side: `availableWrite()` returns the number of frame
        // slots free to write into.  Mirror logic.

        uint32_t availableRead() const {
            return writeIdx.load(std::memory_order_acquire) -
                   readIdx .load(std::memory_order_relaxed);
        }
        uint32_t availableWrite() const {
            return (uint32_t)WAV_BUF_FRAMES - availableRead();
        }
        /// Producer-side advance — caller has read `n` frames starting
        /// at `readIdx & mask`.
        void commitRead(uint32_t n) {
            readIdx.fetch_add(n, std::memory_order_release);
        }
        /// Decoder-side advance — caller has written `n` frames starting
        /// at `writeIdx & mask`.
        void commitWrite(uint32_t n) {
            writeIdx.fetch_add(n, std::memory_order_release);
        }
        /// Drop everything (play() reset; stop()).  Must NOT be called
        /// while the decoder might still be writing.
        void ringReset() {
            writeIdx.store(0, std::memory_order_relaxed);
            readIdx .store(0, std::memory_order_relaxed);
        }

        // Set true by play() once the source opens; cleared by
        // stop() / decoder-side EOF / fade-end.  Producer skips
        // channels where active=false.  acquire on the producer
        // side pairs with release on play().
        std::atomic<bool> active{false};

        // Hint to the decoder that this channel has just been
        // (re)activated and needs an immediate refill.  Cleared by
        // the decoder once first refill lands.  Cheap signal vs
        // walking every channel's writeIdx==readIdx state.
        std::atomic<bool> needsPrefill{false};

        // Set by the decoder when source->readFrames() returns 0 AND
        // source->isExhausted() is true.  Producer sees this when its
        // own readIdx catches writeIdx — only THEN tears the channel
        // down (so it drains the last samples before silencing).
        std::atomic<bool> sourceExhausted{false};
    };

    struct Channel {
        WavState wav;                          // WAV state with float buffers

        // In-place storage for the active IAudioSource.  At play() time
        // we placement-new a concrete source (WavStreamSource /
        // WavPreloadSource / future WavPagedSource) into this buffer
        // and stash the pointer in `wav.source`.  Sized via
        // `kAudioSourceMaxSize` in audio_source.h — static_asserts
        // there catch growth that overflows.
        alignas(kAudioSourceAlign) uint8_t sourceStorage[kAudioSourceMaxSize];

        float volume          = 1.0f;
        float pan             = 0.0f;          // -1.0 (left) to +1.0 (right)
        float panL            = 0.707f;        // pre-computed left gain
        float panR            = 0.707f;        // pre-computed right gain
        uint8_t outputChannels = AudioChannel::ALL;
        char filename[CHANNEL_FILENAME_MAX] = {};
        bool mute             = false;
        
        // Queue for this channel — pointer to a PSRAM-allocated slot
        // array of capacity QUEUE_SIZE_PER_CHANNEL.  Allocated in
        // AudioMixer::begin(), freed in shutdown().  Each slot is ~150 B;
        // 4 slots × 8 channels = ~5 KB the singleton no longer carries
        // in BSS.  Touched only on playAsync / queueSound /
        // checkAndPlayNextQueued / clearQueue — protocol-rate cadence,
        // PSRAM latency invisible.  The CHANNEL ITSELF stays in DRAM
        // (read every audio frame by produceFrame()); only this queue
        // member is indirected.
        QueuedSound* queue    = nullptr;
        int queueHead         = 0;
        int queueTail         = 0;
        QueueLoopBehavior pendingLoopBehavior = QueueLoopBehavior::StopImmediate;
        bool hasQueuedItem    = false;

        // Fade state
        bool fading           = false;
        float fadeVolume      = 1.0f;
        float fadeStep        = 0.0f;

        // Auto tail fade-out (AudioPlaybackOptions::fadeOutMs).  When the
        // playback-remaining drops below `fadeOutTriggerFrames` source frames,
        // the consumer starts a fade-out at `fadeOutStep` (positive).  Cleared
        // to 0 once armed so it fires exactly once; 0 = disabled.
        uint32_t fadeOutTriggerFrames = 0;
        float    fadeOutStep          = 0.0f;
    };

    enum class CommandType : uint8_t {
        None = 0,
        Play,
        Stop,
        StopAll,
        SetVolume,
        SetMasterVolume,
        SetOutput,
        StopLooping,
        QueueSound,
        ClearQueue
    };

    struct Command {
        CommandType type      = CommandType::None;
        int channelId         = -1;
        char filename[128]    = {};
        AudioPlaybackOptions options;
        AudioStopMode stopMode = AudioStopMode::Immediate;
        QueueLoopBehavior loopBehavior = QueueLoopBehavior::StopImmediate;
        float volume          = 1.0f;
        uint8_t outputChannels = AudioChannel::ALL;
        /// Custom fade-out duration for AudioStopMode::Fade (used by
        /// stopAsyncWithFadeMs).  0 = use the default FADE_DURATION_MS.
        uint16_t fadeMs       = 0;
    };

    // ---- Internal Methods ----
    // Producer side (Core 1 task on ESP32, Core 0 on Pico)
    bool refillDrainBuffer(Channel& ch);        // Pull frames from source into ch.wav.bufL/R (int16 Q15)
    bool getWavSample(Channel& ch, int16_t& outL, int16_t& outR);   // Resampled int16 frame
    bool produceFrame();                         // Mix one frame → ring buffer
    void updatePan(Channel& ch);                 // Recalculate panL/panR

    // Consumer side (Core 1 task)
    void consumeAndOutput();                     // Ring buffer → I2S

#if SFX_PLATFORM_ESP32
    static void producerTaskFunc(void* param);   // FreeRTOS task entry point
    static void decoderTaskFunc(void* param);    // Phase 6 — decode prefetch on Core 0
#endif

    /// Phase 6: kick the decoder task whenever a channel needs immediate
    /// service (play() entry, etc.).  Cheap if the task isn't running
    /// (Pico) — compiles to nothing.
    void notifyDecoder();

    /// Phase 7 polish (2026-05-28): on-demand 32 KB DMA-cap SD scratch.
    /// Allocated by `acquireSdScratch()` at WavPreloadSource open time
    /// (preloadIntoMemory=true path); freed by `releaseSdScratch()` when
    /// that source destructs.  Refcount-protected so concurrent
    /// preloads share one buffer.  Returns nullptr on alloc failure
    /// — caller falls through to the streaming path.
    uint8_t* acquireSdScratch();
    void     releaseSdScratch();
    size_t   sdScratchBytes() const { return WAV_SD_READ_BYTES; }
    
    // Command queue
    bool queueCommand(const Command& cmd);
    void processCommands();
    void executeCommand(const Command& cmd);
    
    // Queue management
    void checkAndPlayNextQueued(int channel);
    bool enqueueToChannel(int channel, const char* filename, const AudioPlaybackOptions& options,
                          QueueLoopBehavior loopBehavior);
    bool dequeueFromChannel(int channel, QueuedSound& out);
    
    // Private constructor for singleton
    AudioMixer() = default;

    // ---- File I/O ----
    // ---- State ----
    Channel _channels[AUDIO_MAX_CHANNELS];
    float _masterVolume       = 1.0f;
    std::atomic<bool> _initialized{false};   // Core 0 writes, Core 1 reads
    std::atomic<bool> _i2sRunning{false};     // Core 1 writes, Core 0 reads
    
    // I2S pins (stored in begin(), used in beginI2S())
    uint8_t _i2sDataPin  = 0;
    uint8_t _i2sBclkPin  = 0;
    uint8_t _i2sLrclkPin = 0;

    // SD scratch buffer for WavPreloadSource's initial in-PSRAM decode.
    // Phase 7 polish (2026-05-28): allocated on demand by
    // acquireSdScratch() instead of unconditionally at begin().  32 KB
    // DMA-cap internal SRAM when available (~14 MB/s SDMMC throughput);
    // falls back to PSRAM (~1 MB/s) when the DMA-cap pool is exhausted.
    // Reference-counted across concurrent WavPreloadSource opens; freed
    // when the last consumer calls releaseSdScratch().
    //
    // Most HubFX sessions never touch this — the AssetCache + Mp3PsramSource
    // path doesn't use it.  GunFx is the current consumer (it sets
    // preloadIntoMemory=true in its play options).
    uint8_t* _sdReadBuf      = nullptr;
    int      _sdScratchUsers = 0;
    SfxMutex _sdScratchMutex;
    bool     _sdScratchMutexInit = false;

    // Command queue mutex (cross-core: Core 0 queues, producer task dequeues)
    SfxMutex _cmdMutex;

    // Command queue (async play/stop/setVolume from Core 0 → producer task).
    // PSRAM-allocated in begin() — capacity = AUDIO_CMD_QUEUE_SIZE; each
    // Command is ~150 B so the table is ~2.4 KB the singleton no longer
    // carries in BSS.  Control-rate (Core 0 protocol packets → mutex →
    // producer drain); PSRAM latency invisible.  Indices stay in DRAM
    // for cross-core atomic semantics (Rule 15).
    Command* _cmdQueue = nullptr;
    std::atomic<int> _cmdQueueHead{0};       // Core 0 writes (mutex), producer reads
    std::atomic<int> _cmdQueueTail{0};       // Producer writes (mutex), Core 0 reads

#if SFX_PLATFORM_ESP32
    // Producer task state
    TaskHandle_t _producerTaskHandle = nullptr;
    std::atomic<bool> _producerRunning{false};  // Producer task reads, Core 0 writes
    /// Set true by the producer task right before it suspends itself
    /// on exit; stopProducerTask() waits for it then vTaskDeletes the
    /// task from outside.  Self-deletion via vTaskDelete(nullptr)
    /// defers TCB reap to the idle task, which under upload-induced
    /// suspend/resume pressure caused producer-recreate to OOM
    /// (rolled-back build #285).
    std::atomic<bool> _producerExited{false};

    // Consumer task handle (set by setConsumerTaskHandle, used by suspend/resume)
    TaskHandle_t _consumerTaskHandle = nullptr;

    // Producer task config (stored by startProducerTask for resumeAudio)
    int _producerCore = 1;
    int _producerPriority = configMAX_PRIORITIES - 2;
    int _producerStackSize = 8192;

    // ── Decoder task (Phase 6, feature/audio-decode-prefetch) ────────
    //
    // Single FreeRTOS task pinned to Core 0 (priority just above the
    // Arduino loop but below the protocol handler).  Wakes on
    // notification (xTaskNotifyGive) from play() / stop() / its own
    // 5 ms tick, walks every active channel, refills any ring whose
    // availableWrite() exceeds a fill threshold.  Producer task on
    // Core 1 NEVER calls decode; it only reads from the per-channel
    // rings.
    TaskHandle_t       _decoderTaskHandle = nullptr;
    std::atomic<bool>  _decoderRunning{false};
    std::atomic<bool>  _decoderExited{false};
    int                _decoderCore       = 0;
    int                _decoderPriority   = 5;        // > Arduino loop (1), < producer (configMAX-2)
    int                _decoderStackSize  = 6144;     // libhelix scratch + per-batch float scratch
    static constexpr uint32_t kDecoderTickMs = 5;
#endif

    // Stats (cross-core diagnostic counters).
    //   _underruns / _consumeLoops / _consumeFrames are KEPT under every
    //   build — they back AUDIO_STATUS_RESP fields read by Studio.
    //   _produceLoops is pace-log-only.
    std::atomic<uint32_t> _underruns{0};      // Core 1 consumer writes, Core 0 reads
    std::atomic<uint32_t> _consumeLoops{0};   // Core 1 consumer writes, Core 0 reads
    std::atomic<uint32_t> _consumeFrames{0};  // Core 1 consumer writes, Core 0 reads

    // Pace-log telemetry (Phase 7 polish, gated by SFX_AUDIO_PACE_TELEMETRY
    // in audio_config.h).  Saves ~50 bytes of DRAM atomics + the entire
    // periodic-logger code block on release builds.  Producer-task logger
    // in producerTaskFunc is wrapped in the same macro.  None of these
    // fields are read by AUDIO_STATUS_RESP — purely informational.
#if SFX_AUDIO_PACE_TELEMETRY
    std::atomic<uint32_t> _produceLoops{0};           // Producer task writes, Core 0 reads
    std::atomic<uint8_t>  _ringMinFillPct{100};       // min observed since last log
    std::atomic<uint32_t> _maxSdReadUs{0};            // max refill latency since last log
    std::atomic<uint32_t> _slowSdReads{0};            // refills > 5 ms since last log
    std::atomic<uint32_t> _verySlowSdReads{0};        // refills > 20 ms since last log
    std::atomic<uint32_t> _channelStarves[AUDIO_MAX_CHANNELS]{};
        // Per-channel drain-empty count — bumped by getWavSample when
        // the SPSC ring's availableRead < 2.  Canonical "decoder
        // falling behind" indicator surfaced in the pace log.
#endif

    // Status (producer task writes, Core 0 reads for status queries)
    std::atomic<bool> _channelPlaying[AUDIO_MAX_CHANNELS]{};
    std::atomic<float> _channelRemainingSec[AUDIO_MAX_CHANNELS]{};
};

// Template implementation (must be visible at point of instantiation)
#include "audio_mixer.ipp"

#endif // AUDIO_MIXER_H
