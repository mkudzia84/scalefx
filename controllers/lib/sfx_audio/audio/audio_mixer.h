/**
 * Audio Mixer — SPSC Ring Buffer Architecture
 * 
 * Singleton software audio mixer with I2S output.
 * Supports 8 simultaneous channels with WAV playback, per-channel volume,
 * loop/one-shot modes, L/R/stereo routing, and float mixing pipeline.
 * 
 * DUAL-CORE ARCHITECTURE (ESP32-S3):
 *   Core 1 — Producer Task: WAV decode + SD reads + float mixing → SPSC ring buffer
 *   Core 1 — Consumer Task: SPSC ring buffer → I2S DMA (higher priority)
 *   Core 0: Protocol handling only — uses *Async() API to queue commands
 * 
 * All audio math uses float, leveraging hardware FPU where available
 * (Cortex-M33 on RP2350, Xtensa DSP on ESP32-S3).
 * WAV samples are converted to float for mixing; int16 conversion happens
 * only at the final output stage. WAV files at non-native sample rates
 * are resampled via linear interpolation.
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

// ============================================================================
//  CONSTANTS (see audio_config.h for configuration)
// ============================================================================

// WAV pre-buffer sizes (float frames per channel)
// These buffers provide the primary SD-stall resilience — each channel can
// sustain playback for WAV_BUF_FRAMES / AUDIO_SAMPLE_RATE seconds without
// any SD reads.  The mixed-audio ring buffer is a small core-to-core FIFO.
//
// ESP32-S3: 24000 frames/ch = 0.5 s at 48 kHz.  8 ch × 2 (L+R) × 24000
//   × 4 bytes = 1.5 MB total in PSRAM (8 MB available).
//   SD read batch = 16 KB (4 sectors) reduces SD access frequency.
// Pico: Smaller buffers in SRAM heap.
#if SFX_PLATFORM_ESP32
constexpr int WAV_BUF_FRAMES       = 24000;     // decoded float frames per track (PSRAM, 0.5 s @ 48 kHz)
constexpr int WAV_SD_READ_BYTES    = 16384;     // bytes per SD card read batch (PSRAM)
#else
constexpr int WAV_BUF_FRAMES       = 1024;      // decoded float frames per track
constexpr int WAV_SD_READ_BYTES    = 4096;      // bytes per SD card read batch
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
    
    // WAV state for float buffer + resampler
    struct WavState {
        SdFile file;
        bool       active         = false;
        bool       loop           = true;
        int        loopCount      = -1;        // -1=infinite, 0=no loop, N=N loops
        int        loopCountInit  = -1;        // Initial loop count for status

        // WAV format (from header)
        uint32_t   sampleRate_Hz  = 0;
        uint16_t   numChannels    = 0;
        uint16_t   bitsPerSample  = 0;
        uint32_t   dataStart      = 0;         // byte offset of PCM data
        uint32_t   totalFrames    = 0;         // total sample frames
        uint32_t   framesRead     = 0;         // frames consumed from file

        // Decoded float buffer for mixing [-1.0, +1.0]
        // Dynamically allocated from PSRAM (ESP32-S3) or heap (Pico)
        // in AudioMixer::begin(). nullptr until allocated.
        float*     bufL         = nullptr;
        float*     bufR         = nullptr;
        int        bufLen         = 0;         // valid frames in buffer
        int        bufPos         = 0;         // next frame to consume

        // Linear interpolation resampler
        float      resampleRatio  = 1.0f;      // srcRate / outputRate
        float      resampleFrac   = 0.0f;      // fractional sample position
    };

    struct Channel {
        WavState wav;                          // WAV state with float buffers
        float volume          = 1.0f;
        float pan             = 0.0f;          // -1.0 (left) to +1.0 (right)
        float panL            = 0.707f;        // pre-computed left gain
        float panR            = 0.707f;        // pre-computed right gain
        uint8_t outputChannels = AudioChannel::ALL;
        char filename[CHANNEL_FILENAME_MAX] = {};
        bool mute             = false;
        
        // Queue for this channel
        QueuedSound queue[QUEUE_SIZE_PER_CHANNEL];
        int queueHead         = 0;
        int queueTail         = 0;
        QueueLoopBehavior pendingLoopBehavior = QueueLoopBehavior::StopImmediate;
        bool hasQueuedItem    = false;

        // Fade state
        bool fading           = false;
        float fadeVolume      = 1.0f;
        float fadeStep        = 0.0f;
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
    };

    // ---- Internal Methods ----
    // Producer side (Core 1 task on ESP32, Core 0 on Pico)
    bool refillWavBuffer(WavState& ws);        // Batch SD read into float buffer
    bool getWavSample(WavState& ws, float& outL, float& outR);  // Resampled frame
    bool produceFrame();                        // Mix one frame → ring buffer
    void updatePan(Channel& ch);                // Recalculate panL/panR
    
    // Consumer side (Core 1 task)
    void consumeAndOutput();                    // Ring buffer → I2S

#if SFX_PLATFORM_ESP32
    static void producerTaskFunc(void* param);  // FreeRTOS task entry point
#endif
    
    // WAV parsing
    bool parseWavHeader(WavState& ws);
    
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

    // SD read buffer (shared temp for all channels on producer side)
    // Dynamically allocated from PSRAM in begin(), freed in shutdown().
    uint8_t* _sdReadBuf = nullptr;

    // Command queue mutex (cross-core: Core 0 queues, producer task dequeues)
    SfxMutex _cmdMutex;

    // Command queue (async play/stop/setVolume from Core 0 → producer task)
    Command _cmdQueue[AUDIO_CMD_QUEUE_SIZE];
    std::atomic<int> _cmdQueueHead{0};       // Core 0 writes (mutex), producer reads
    std::atomic<int> _cmdQueueTail{0};       // Producer writes (mutex), Core 0 reads

#if SFX_PLATFORM_ESP32
    // Producer task state
    TaskHandle_t _producerTaskHandle = nullptr;
    std::atomic<bool> _producerRunning{false};  // Producer task reads, Core 0 writes

    // Consumer task handle (set by setConsumerTaskHandle, used by suspend/resume)
    TaskHandle_t _consumerTaskHandle = nullptr;

    // Producer task config (stored by startProducerTask for resumeAudio)
    int _producerCore = 1;
    int _producerPriority = configMAX_PRIORITIES - 2;
    int _producerStackSize = 8192;
#endif

    // Stats (cross-core diagnostic counters)
    std::atomic<uint32_t> _underruns{0};     // Core 1 consumer writes, Core 0 reads
    std::atomic<uint32_t> _produceLoops{0};  // Producer task writes, Core 0 reads
    std::atomic<uint32_t> _consumeLoops{0};  // Core 1 consumer writes, Core 0 reads
    std::atomic<uint32_t> _consumeFrames{0}; // Core 1 consumer writes, Core 0 reads

    // Status (producer task writes, Core 0 reads for status queries)
    std::atomic<bool> _channelPlaying[AUDIO_MAX_CHANNELS]{};
    std::atomic<float> _channelRemainingSec[AUDIO_MAX_CHANNELS]{};
};

// Template implementation (must be visible at point of instantiation)
#include "audio_mixer.ipp"

#endif // AUDIO_MIXER_H
