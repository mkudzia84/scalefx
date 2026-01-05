/**
 * HubFX Audio Mixer
 * 
 * Singleton software audio mixer for Raspberry Pi Pico with I2S output.
 * Supports 8 simultaneous channels with WAV playback, per-channel volume,
 * loop/one-shot modes, L/R/stereo routing, and soft-clipped mixing.
 * 
 * Uses SdCardModule singleton for thread-safe SD card access.
 * 
 * Usage:
 *   AudioMixer& mixer = AudioMixer::instance();
 *   mixer.begin(i2s_data, i2s_bclk, i2s_lrclk, codec);
 */

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include <Arduino.h>
#include <SdFat.h>
#include <pico/mutex.h>

class AudioCodec;  // Forward declaration

// Use File32 type (SdFat32) for SD card file operations
using SdCardFile = File32;

// Include centralized audio configuration (relative path from src/audio/)
#include "audio_config.h"

// ============================================================================
//  CONSTANTS (see audio_config.h for configuration)
// ============================================================================

// These values come from audio_config.h - do not redefine here
// To change settings, edit audio_config.h or use -D flags in platformio.ini

// Ensure audio_config.h values are used (removed duplicate definitions)
// All configurable parameters now in audio_config.h

constexpr int AUDIO_CMD_QUEUE_SIZE     = 16;

// ============================================================================
//  TYPES
// ============================================================================

enum class AudioOutput : uint8_t {
    Stereo = 0,
    Left   = 1,
    Right  = 2
};

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
    AudioOutput output     = AudioOutput::Stereo;
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
//  AUDIO MIXER CLASS (Singleton)
// ============================================================================

class AudioMixer {
public:
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

    // ---- Initialization ----
    // Uses SdCardModule singleton internally for SD access
    bool begin(uint8_t i2s_data_pin, uint8_t i2s_bclk_pin, uint8_t i2s_lrclk_pin,
               AudioCodec* codec = nullptr);
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
    void setOutput(int channel, AudioOutput output);
    float volume(int channel) const;
    float masterVolume() const { return _masterVolume; }

    // ---- Status ----
    bool isPlaying(int channel) const;
    bool isAnyPlaying() const;
    int remainingMs(int channel) const;
    AudioCodec* getCodec() const { return _codec; }
    
#if AUDIO_MOCK_I2S
    // Mock I2S statistics access
    void printMockStatistics();
    void resetMockStatistics();
#endif

    // ---- Processing ----
    void process();

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
    AudioOutput getOutput(int channel) const;
    uint32_t getSampleRate(int channel) const;
    uint16_t getNumChannels(int channel) const;
    uint16_t getBitsPerSample(int channel) const;
    uint32_t getTotalSamples(int channel) const;

private:
    // ---- Internal Types ----
    static constexpr int CHANNEL_FILENAME_MAX = 64;
    static constexpr int QUEUE_SIZE_PER_CHANNEL = 4;  // Max queued sounds per channel
    
    struct Channel {
        SdCardFile file;
        bool active           = false;
        bool loop             = false;
        int loopCount         = 0;        // Remaining loops: -1=infinite, 0=no more loops
        int loopCountInitial  = 0;        // Initial loop count for status
        float volume          = 1.0f;
        AudioOutput output    = AudioOutput::Stereo;
        char filename[CHANNEL_FILENAME_MAX] = {};
        
        // Queue for this channel
        QueuedSound queue[QUEUE_SIZE_PER_CHANNEL];
        int queueHead         = 0;
        int queueTail         = 0;
        QueueLoopBehavior pendingLoopBehavior = QueueLoopBehavior::StopImmediate;
        bool hasQueuedItem    = false;    // Flag: queue has waiting item

        // WAV info
        uint32_t sampleRate     = 0;
        uint16_t numChannels    = 0;
        uint16_t bitsPerSample  = 0;
        uint32_t dataStart      = 0;
        uint32_t totalSamples   = 0;
        uint32_t samplesRemaining = 0;

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
        AudioOutput output    = AudioOutput::Stereo;
    };

    // ---- Internal Methods ----
    void doMixAndOutput();  // Core mixing logic (called by processCore0/processCore1)
    bool parseWavHeader(Channel& ch);
    void mixChannel(Channel& ch, int32_t* mixL, int32_t* mixR, int samples);
    void outputToI2S(int samples);
    bool queueCommand(const Command& cmd);
    void processCommands();
    void executeCommand(const Command& cmd);
    void checkAndPlayNextQueued(int channel);  // Check queue after track ends
    bool enqueueToChannel(int channel, const char* filename, const AudioPlaybackOptions& options,
                          QueueLoopBehavior loopBehavior);
    bool dequeueFromChannel(int channel, QueuedSound& out);
    
    // Private constructor for singleton
    AudioMixer() = default;

    // ---- State ----
    Channel _channels[AUDIO_MAX_CHANNELS];
    AudioCodec* _codec        = nullptr;
    float _masterVolume       = 1.0f;
    bool _initialized         = false;
    bool _i2sRunning          = false;

    // Mixing buffers
    int32_t _mixBufferL[AUDIO_MIX_BUFFER_SIZE] = {};
    int32_t _mixBufferR[AUDIO_MIX_BUFFER_SIZE] = {};
    int16_t _readBuffer[AUDIO_MIX_BUFFER_SIZE * 2] = {};

    // Dual-core support
    bool _dualCoreMode                = false;
    mutex_t _mixerMutex;
    mutex_t _cmdMutex;

    // Command queue (Core 0 -> Core 1)
    Command _cmdQueue[AUDIO_CMD_QUEUE_SIZE];
    volatile int _cmdQueueHead        = 0;
    volatile int _cmdQueueTail        = 0;

    // Status (Core 1 -> Core 0)
    volatile bool _channelPlaying[AUDIO_MAX_CHANNELS] = {};
    volatile int _channelRemainingMs[AUDIO_MAX_CHANNELS] = {};
};

#endif // AUDIO_MIXER_H
