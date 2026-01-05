/**
 * HubFX Audio Mixer
 * 
 * Software audio mixer for Raspberry Pi Pico with I2S output.
 * Supports 8 simultaneous channels with WAV playback, per-channel volume,
 * loop/one-shot modes, L/R/stereo routing, and soft-clipped mixing.
 */

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include <Arduino.h>
#include <SdFat.h>
#include <pico/mutex.h>

class AudioCodec;  // Forward declaration

// Use explicit FsFile type (SdFat) to avoid conflicts with LittleFS File
using SdCardFile = FsFile;

// Include centralized audio configuration (relative path from src/audio/)
#include "audio_config.h"

// Legacy compatibility - these are now defined in audio_config.h
// Kept here to avoid breaking existing code that includes only this header

// Legacy compatibility - these are now defined in audio_config.h
// Kept here to avoid breaking existing code that includes only this header

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

struct AudioPlaybackOptions {
    bool loop              = false;
    float volume           = 1.0f;
    AudioOutput output     = AudioOutput::Stereo;
    int startOffsetMs      = 0;
};

// ============================================================================
//  AUDIO MIXER CLASS
// ============================================================================

class AudioMixer {
public:
    AudioMixer() = default;
    ~AudioMixer();

    // Non-copyable
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // ---- Initialization ----
    bool begin(SdFat* sd, uint8_t i2s_data_pin, uint8_t i2s_bclk_pin, uint8_t i2s_lrclk_pin,
               AudioCodec* codec = nullptr);
    void shutdown();

    // ---- Playback Control ----
    bool play(int channel, const char* filename, const AudioPlaybackOptions& options = {});
    void stop(int channel, AudioStopMode mode = AudioStopMode::Immediate);
    void stopAll(AudioStopMode mode = AudioStopMode::Immediate);
    void stopLooping(int channel);
    void stopLoopingAll();

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

    // ---- Processing ----
    void process();

    // Async commands (thread-safe for dual-core)
    bool playAsync(int channel, const char* filename, const AudioPlaybackOptions& options = {});
    void stopAsync(int channel, AudioStopMode mode = AudioStopMode::Immediate);
    void setVolumeAsync(int channel, float volume);
    void setMasterVolumeAsync(float volume);

#if AUDIO_TEST_MODE
    // ---- Test Mode ----
    void activateTestChannel(int channel, float volume = 1.0f);
    void deactivateTestChannel(int channel);
    uint32_t getI2SWriteCount() const { return _i2sWriteCount; }
    void printMixerStatus() const;
#endif

private:
    // ---- Internal Types ----
    struct Channel {
        SdCardFile file;
        bool active           = false;
        bool loop             = false;
        float volume          = 1.0f;
        AudioOutput output    = AudioOutput::Stereo;

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
        StopLooping
    };

    struct Command {
        CommandType type      = CommandType::None;
        int channelId         = -1;
        char filename[128]    = {};
        AudioPlaybackOptions options;
        AudioStopMode stopMode = AudioStopMode::Immediate;
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

    // ---- State ----
    SdFat* _sd                = nullptr;
    Channel _channels[AUDIO_MAX_CHANNELS];
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
    
    #if AUDIO_TEST_MODE
    // Diagnostic counters
    uint32_t _i2sWriteCount = 0;
    #endif
};

#endif // AUDIO_MIXER_H
