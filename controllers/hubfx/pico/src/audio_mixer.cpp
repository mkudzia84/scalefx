/**
 * HubFX Audio Mixer - Implementation
 * 
 * Software audio mixer for Raspberry Pi Pico with I2S output.
 * Supports dual-core operation via external combined Core 1 task.
 */

#include "audio_mixer.h"
#include <I2S.h>

// ============================================================================
//  CONSTANTS
// ============================================================================

constexpr int FADE_DURATION_MS = 50;
constexpr float FADE_STEPS = (FADE_DURATION_MS * AUDIO_SAMPLE_RATE) / (1000.0f * AUDIO_MIX_BUFFER_SIZE);

// I2S pin configuration
constexpr int PIN_I2S_DATA  = 0;   // GP0 - I2S DIN
constexpr int PIN_I2S_BCLK  = 1;   // GP1 - I2S BCLK
constexpr int PIN_I2S_LRCLK = 2;   // GP2 - I2S LRCLK/WS

// ============================================================================
//  STATIC HELPERS
// ============================================================================

static I2S i2sOutput(OUTPUT);

static inline int16_t softClip(int32_t sample) {
    // Soft clipping with tanh-like curve for overflow protection
    if (sample > 32767) {
        return static_cast<int16_t>(32767 - (32767 - sample) / 8);
    } else if (sample < -32768) {
        return static_cast<int16_t>(-32768 - (-32768 - sample) / 8);
    }
    return static_cast<int16_t>(sample);
}

// ============================================================================
//  LIFECYCLE
// ============================================================================

AudioMixer::~AudioMixer() {
    shutdown();
}

bool AudioMixer::begin(SdFat* sd) {
    if (_initialized) return true;
    if (!sd) return false;

    _sd = sd;
    _masterVolume = 1.0f;
    _dualCoreMode = false;

    // Initialize all channels
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i] = Channel{};
        _channelPlaying[i] = false;
        _channelRemainingMs[i] = 0;
    }

    // Configure I2S output
    i2sOutput.setBCLK(PIN_I2S_BCLK);
    i2sOutput.setDATA(PIN_I2S_DATA);
    i2sOutput.setBitsPerSample(AUDIO_BIT_DEPTH);

    if (!i2sOutput.begin(AUDIO_SAMPLE_RATE)) {
        Serial.println("[AudioMixer] Failed to initialize I2S");
        return false;
    }

    _i2sRunning = true;
    _initialized = true;
    
    Serial.println("[AudioMixer] Initialized (single-core)");
    return true;
}

bool AudioMixer::beginDualCore(SdFat* sd) {
    if (!begin(sd)) return false;

    _dualCoreMode = true;
    mutex_init(&_mixerMutex);
    mutex_init(&_cmdMutex);
    _cmdQueueHead = 0;
    _cmdQueueTail = 0;

    Serial.println("[AudioMixer] Initialized (dual-core ready)");
    return true;
}

void AudioMixer::shutdown() {
    if (!_initialized) return;

    // Stop all playback
    stopAll(AudioStopMode::Immediate);

    // Close all open files
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (_channels[i].file) {
            _channels[i].file.close();
        }
    }

    // Stop I2S
    if (_i2sRunning) {
        i2sOutput.end();
        _i2sRunning = false;
    }

    _initialized = false;
    Serial.println("[AudioMixer] Shutdown complete");
}

// ============================================================================
//  PLAYBACK CONTROL
// ============================================================================

bool AudioMixer::play(int channel, const char* filename, const AudioPlaybackOptions& options) {
    if (!_initialized) return false;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    if (!filename) return false;

    Channel& ch = _channels[channel];

    // Stop current playback on this channel
    if (ch.active && ch.file) {
        ch.file.close();
    }

    // Open the WAV file
    if (!ch.file.open(filename, O_RDONLY)) {
        Serial.printf("[AudioMixer] Failed to open: %s\n", filename);
        return false;
    }

    // Parse WAV header
    if (!parseWavHeader(ch)) {
        Serial.printf("[AudioMixer] Invalid WAV: %s\n", filename);
        ch.file.close();
        return false;
    }

    // Apply playback options
    ch.loop = options.loop;
    ch.volume = constrain(options.volume, 0.0f, 1.0f);
    ch.output = options.output;
    ch.fading = false;
    ch.fadeVolume = 1.0f;

    // Handle start offset
    if (options.startOffsetMs > 0) {
        uint32_t offsetSamples = (static_cast<uint32_t>(options.startOffsetMs) * ch.sampleRate) / 1000;
        if (offsetSamples < ch.totalSamples) {
            uint32_t offsetBytes = offsetSamples * ch.numChannels * (ch.bitsPerSample / 8);
            ch.file.seek(ch.dataStart + offsetBytes);
            ch.samplesRemaining = ch.totalSamples - offsetSamples;
        }
    }

    ch.active = true;
    _channelPlaying[channel] = true;

    Serial.printf("[AudioMixer] Ch%d: Playing %s (%s, vol=%.2f)\n", 
                  channel, filename, ch.loop ? "loop" : "once", ch.volume);
    return true;
}

void AudioMixer::stop(int channel, AudioStopMode mode) {
    if (!_initialized) return;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;

    Channel& ch = _channels[channel];
    if (!ch.active) return;

    switch (mode) {
        case AudioStopMode::Immediate:
            ch.active = false;
            if (ch.file) ch.file.close();
            _channelPlaying[channel] = false;
            Serial.printf("[AudioMixer] Ch%d: Stopped\n", channel);
            break;

        case AudioStopMode::Fade:
            ch.fading = true;
            ch.fadeVolume = 1.0f;
            ch.fadeStep = 1.0f / FADE_STEPS;
            Serial.printf("[AudioMixer] Ch%d: Fading out\n", channel);
            break;

        case AudioStopMode::LoopEnd:
            ch.loop = false;
            Serial.printf("[AudioMixer] Ch%d: Will stop at loop end\n", channel);
            break;
    }
}

void AudioMixer::stopAll(AudioStopMode mode) {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        stop(i, mode);
    }
}

void AudioMixer::stopLooping(int channel) {
    if (!_initialized) return;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].loop = false;
}

void AudioMixer::stopLoopingAll() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i].loop = false;
    }
}

// ============================================================================
//  VOLUME & ROUTING
// ============================================================================

void AudioMixer::setVolume(int channel, float vol) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].volume = constrain(vol, 0.0f, 1.0f);
}

void AudioMixer::setMasterVolume(float vol) {
    _masterVolume = constrain(vol, 0.0f, 1.0f);
}

void AudioMixer::setOutput(int channel, AudioOutput output) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].output = output;
}

float AudioMixer::volume(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0.0f;
    return _channels[channel].volume;
}

// ============================================================================
//  STATUS
// ============================================================================

bool AudioMixer::isPlaying(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channelPlaying[channel];
}

bool AudioMixer::isAnyPlaying() const {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (_channelPlaying[i]) return true;
    }
    return false;
}

int AudioMixer::remainingMs(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return -1;
    if (!_channelPlaying[channel]) return -1;
    if (_channels[channel].loop) return -1;
    return _channelRemainingMs[channel];
}

// ============================================================================
//  AUDIO PROCESSING
// ============================================================================

void AudioMixer::doMixAndOutput() {
    if (!_initialized || !_i2sRunning) return;

    // Process command queue (dual-core mode)
    if (_dualCoreMode) {
        processCommands();
    }

    // Clear mix buffers
    memset(_mixBufferL, 0, sizeof(_mixBufferL));
    memset(_mixBufferR, 0, sizeof(_mixBufferR));

    // Mix all active channels
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        Channel& ch = _channels[i];
        if (!ch.active) continue;

        mixChannel(ch, _mixBufferL, _mixBufferR, AUDIO_MIX_BUFFER_SIZE);

        // Handle fade completion
        if (ch.fading && ch.fadeVolume <= 0.0f) {
            ch.active = false;
            ch.fading = false;
            if (ch.file) ch.file.close();
            _channelPlaying[i] = false;
        }

        // Handle end of file
        if (ch.samplesRemaining == 0) {
            if (ch.loop) {
                ch.file.seek(ch.dataStart);
                ch.samplesRemaining = ch.totalSamples;
            } else {
                ch.active = false;
                if (ch.file) ch.file.close();
                _channelPlaying[i] = false;
            }
        }

        // Update remaining time status
        if (ch.active && ch.sampleRate > 0) {
            _channelRemainingMs[i] = static_cast<int>((ch.samplesRemaining * 1000UL) / ch.sampleRate);
        }
    }

    // Output mixed audio to I2S
    outputToI2S(AUDIO_MIX_BUFFER_SIZE);
}

void AudioMixer::process() {
    doMixAndOutput();
}

// ============================================================================
//  WAV PARSING
// ============================================================================

bool AudioMixer::parseWavHeader(Channel& ch) {
    uint8_t header[44];

    if (ch.file.read(header, 44) != 44) {
        return false;
    }

    // Validate RIFF header
    if (memcmp(header, "RIFF", 4) != 0) return false;
    if (memcmp(header + 8, "WAVE", 4) != 0) return false;
    if (memcmp(header + 12, "fmt ", 4) != 0) return false;

    // Check PCM format
    uint16_t audioFormat = header[20] | (header[21] << 8);
    if (audioFormat != 1) return false;  // PCM only

    // Extract audio properties
    ch.numChannels = header[22] | (header[23] << 8);
    ch.sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    ch.bitsPerSample = header[34] | (header[35] << 8);

    // Validate supported formats
    if (ch.numChannels < 1 || ch.numChannels > 2) return false;
    if (ch.bitsPerSample != 8 && ch.bitsPerSample != 16) return false;

    // Find data chunk (may not be at fixed offset)
    ch.file.seek(12);
    while (ch.file.available()) {
        uint8_t chunkHeader[8];
        if (ch.file.read(chunkHeader, 8) != 8) return false;

        uint32_t chunkSize = chunkHeader[4] | (chunkHeader[5] << 8) | 
                            (chunkHeader[6] << 16) | (chunkHeader[7] << 24);

        if (memcmp(chunkHeader, "data", 4) == 0) {
            ch.dataStart = ch.file.position();
            uint32_t bytesPerSample = ch.numChannels * (ch.bitsPerSample / 8);
            ch.totalSamples = chunkSize / bytesPerSample;
            ch.samplesRemaining = ch.totalSamples;
            return true;
        }

        ch.file.seek(ch.file.position() + chunkSize);
    }

    return false;
}

// ============================================================================
//  MIXING
// ============================================================================

void AudioMixer::mixChannel(Channel& ch, int32_t* mixL, int32_t* mixR, int samples) {
    int bytesPerSample = ch.numChannels * (ch.bitsPerSample / 8);
    int samplesToRead = min(static_cast<uint32_t>(samples), ch.samplesRemaining);
    int bytesToRead = samplesToRead * bytesPerSample;

    int bytesRead = ch.file.read(_readBuffer, bytesToRead);
    int samplesRead = bytesRead / bytesPerSample;

    // Calculate effective volume (channel * master * fade)
    float effectiveVolume = ch.volume * _masterVolume;
    if (ch.fading) {
        effectiveVolume *= ch.fadeVolume;
        ch.fadeVolume -= ch.fadeStep;
        if (ch.fadeVolume < 0.0f) ch.fadeVolume = 0.0f;
    }

    // Mix samples into buffer
    for (int i = 0; i < samplesRead; i++) {
        int32_t sampleL, sampleR;

        if (ch.bitsPerSample == 16) {
            int16_t* buf16 = reinterpret_cast<int16_t*>(_readBuffer);
            if (ch.numChannels == 2) {
                sampleL = buf16[i * 2];
                sampleR = buf16[i * 2 + 1];
            } else {
                sampleL = sampleR = buf16[i];
            }
        } else {
            // 8-bit: convert from unsigned to signed
            uint8_t* buf8 = reinterpret_cast<uint8_t*>(_readBuffer);
            if (ch.numChannels == 2) {
                sampleL = (static_cast<int>(buf8[i * 2]) - 128) << 8;
                sampleR = (static_cast<int>(buf8[i * 2 + 1]) - 128) << 8;
            } else {
                sampleL = sampleR = (static_cast<int>(buf8[i]) - 128) << 8;
            }
        }

        // Apply volume
        sampleL = static_cast<int32_t>(sampleL * effectiveVolume);
        sampleR = static_cast<int32_t>(sampleR * effectiveVolume);

        // Route to output channels
        switch (ch.output) {
            case AudioOutput::Left:
                mixL[i] += sampleL;
                break;
            case AudioOutput::Right:
                mixR[i] += sampleR;
                break;
            case AudioOutput::Stereo:
            default:
                mixL[i] += sampleL;
                mixR[i] += sampleR;
                break;
        }
    }

    ch.samplesRemaining -= samplesRead;
}

void AudioMixer::outputToI2S(int samples) {
    for (int i = 0; i < samples; i++) {
        int16_t outL = softClip(_mixBufferL[i]);
        int16_t outR = softClip(_mixBufferR[i]);
        i2sOutput.write(outL);
        i2sOutput.write(outR);
    }
}

// ============================================================================
//  DUAL-CORE COMMAND QUEUE
// ============================================================================

bool AudioMixer::queueCommand(const Command& cmd) {
    mutex_enter_blocking(&_cmdMutex);

    int nextHead = (_cmdQueueHead + 1) % AUDIO_CMD_QUEUE_SIZE;
    if (nextHead == _cmdQueueTail) {
        mutex_exit(&_cmdMutex);
        return false;  // Queue full
    }

    _cmdQueue[_cmdQueueHead] = cmd;
    _cmdQueueHead = nextHead;

    mutex_exit(&_cmdMutex);
    return true;
}

void AudioMixer::processCommands() {
    while (_cmdQueueTail != _cmdQueueHead) {
        mutex_enter_blocking(&_cmdMutex);
        Command cmd = _cmdQueue[_cmdQueueTail];
        _cmdQueueTail = (_cmdQueueTail + 1) % AUDIO_CMD_QUEUE_SIZE;
        mutex_exit(&_cmdMutex);

        executeCommand(cmd);
    }
}

void AudioMixer::executeCommand(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::Play:
            play(cmd.channelId, cmd.filename, cmd.options);
            break;
        case CommandType::Stop:
            stop(cmd.channelId, cmd.stopMode);
            break;
        case CommandType::StopAll:
            stopAll(cmd.stopMode);
            break;
        case CommandType::SetVolume:
            setVolume(cmd.channelId, cmd.volume);
            break;
        case CommandType::SetMasterVolume:
            setMasterVolume(cmd.volume);
            break;
        case CommandType::SetOutput:
            setOutput(cmd.channelId, cmd.output);
            break;
        case CommandType::StopLooping:
            if (cmd.channelId < 0) stopLoopingAll();
            else stopLooping(cmd.channelId);
            break;
        default:
            break;
    }
}

// ============================================================================
//  ASYNC API (Thread-safe for dual-core)
// ============================================================================

bool AudioMixer::playAsync(int channel, const char* filename, const AudioPlaybackOptions& options) {
    if (!_dualCoreMode) return play(channel, filename, options);

    Command cmd{};
    cmd.type = CommandType::Play;
    cmd.channelId = channel;
    strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
    cmd.options = options;
    return queueCommand(cmd);
}

void AudioMixer::stopAsync(int channel, AudioStopMode mode) {
    if (!_dualCoreMode) {
        if (channel < 0) stopAll(mode);
        else stop(channel, mode);
        return;
    }

    Command cmd{};
    cmd.type = (channel < 0) ? CommandType::StopAll : CommandType::Stop;
    cmd.channelId = channel;
    cmd.stopMode = mode;
    queueCommand(cmd);
}

void AudioMixer::setVolumeAsync(int channel, float vol) {
    if (!_dualCoreMode) {
        setVolume(channel, vol);
        return;
    }

    Command cmd{};
    cmd.type = CommandType::SetVolume;
    cmd.channelId = channel;
    cmd.volume = vol;
    queueCommand(cmd);
}

void AudioMixer::setMasterVolumeAsync(float vol) {
    if (!_dualCoreMode) {
        setMasterVolume(vol);
        return;
    }

    Command cmd{};
    cmd.type = CommandType::SetMasterVolume;
    cmd.volume = vol;
    queueCommand(cmd);
}
