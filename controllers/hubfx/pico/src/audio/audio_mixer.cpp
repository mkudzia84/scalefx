/**
 * HubFX Audio Mixer - Implementation
 * 
 * Software audio mixer for Raspberry Pi Pico with I2S output.
 * Runs on Core 1 with dual-core mode for optimal performance.
 */

#include "audio_mixer.h"
#include "audio_config.h"
#include "audio_codec.h"
#include "../debug_config.h"

#if AUDIO_MOCK_I2S
#include "mock_i2s_sink.h"
#else
#include <I2S.h>
#endif

// ============================================================================
//  CONSTANTS
// ============================================================================

constexpr int FADE_DURATION_MS = 50;
constexpr float FADE_STEPS = (FADE_DURATION_MS * AUDIO_SAMPLE_RATE) / (1000.0f * AUDIO_MIX_BUFFER_SIZE);

// ============================================================================
//  STATIC HELPERS
// ============================================================================

#if AUDIO_MOCK_I2S
static MockI2SSink i2sOutput;
#else
static I2S i2sOutput(OUTPUT);
#endif

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

bool AudioMixer::begin(SdFat* sd, uint8_t i2s_data_pin, uint8_t i2s_bclk_pin, uint8_t i2s_lrclk_pin,
                       AudioCodec* codec) {
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
    
    // Initialize I2S output with provided pins
    i2sOutput.setBCLK(i2s_bclk_pin);
    i2sOutput.setDATA(i2s_data_pin);
    i2sOutput.setBitsPerSample(AUDIO_BIT_DEPTH);

    if (!i2sOutput.begin(AUDIO_SAMPLE_RATE)) {
        MIXER_LOG("Failed to initialize I2S");
        return false;
    }

    _i2sRunning = true;
    
    // Initialize audio codec if provided
    if (codec) {
        MIXER_LOG("Using %s codec", codec->getModelName());
        // Codec initialization happens externally before mixer.begin()
    } else {
        MIXER_LOG("No codec provided (I2S only mode)");
    }
    
    // Initialize dual-core mutex
    mutex_init(&_mixerMutex);
    mutex_init(&_cmdMutex);
    _cmdQueueHead = 0;
    _cmdQueueTail = 0;
    _dualCoreMode = true;
    
    _initialized = true;
    
    MIXER_LOG("Initialized");
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
    MIXER_LOG("Shutdown complete");
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
    if (!ch.file.open(_sd, filename, O_RDONLY)) {
        MIXER_LOG("Ch%d: Failed to open: %s", channel, filename);
        return false;
    }

    // Parse WAV header
    if (!parseWavHeader(ch)) {
        MIXER_LOG("Ch%d: Invalid WAV: %s", channel, filename);
        ch.file.close();
        return false;
    }

    // Apply playback options
    // Handle loop count: if options.loop is true and loopCount not set, use infinite
    if (options.loop && options.loopCount == LOOP_INFINITE) {
        ch.loop = true;
        ch.loopCount = LOOP_INFINITE;
        ch.loopCountInitial = LOOP_INFINITE;
    } else if (options.loopCount > 0) {
        ch.loop = true;
        ch.loopCount = options.loopCount;
        ch.loopCountInitial = options.loopCount;
    } else if (options.loopCount == 0 || !options.loop) {
        ch.loop = false;
        ch.loopCount = 0;
        ch.loopCountInitial = 0;
    } else {
        // loopCount == LOOP_INFINITE without loop flag
        ch.loop = true;
        ch.loopCount = LOOP_INFINITE;
        ch.loopCountInitial = LOOP_INFINITE;
    }
    
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

    // Store filename for status display
    strncpy(ch.filename, filename, CHANNEL_FILENAME_MAX - 1);
    ch.filename[CHANNEL_FILENAME_MAX - 1] = '\0';
    
    ch.active = true;
    _channelPlaying[channel] = true;

    // Format loop info for log
    const char* loopStr;
    char loopBuf[16];
    if (ch.loopCount == LOOP_INFINITE) {
        loopStr = "loop";
    } else if (ch.loopCount > 0) {
        snprintf(loopBuf, sizeof(loopBuf), "x%d", ch.loopCount);
        loopStr = loopBuf;
    } else {
        loopStr = "once";
    }
    
    MIXER_LOG("Ch%d: Playing %s (%s, vol=%.2f)", channel, filename, loopStr, ch.volume);
    return true;
}

// ============================================================================
//  CHANNEL INFO GETTERS
// ============================================================================

const char* AudioMixer::getFilename(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return nullptr;
    return _channels[channel].active ? _channels[channel].filename : nullptr;
}

float AudioMixer::getChannelVolume(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0.0f;
    return _channels[channel].volume;
}

bool AudioMixer::isLooping(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channels[channel].loop;
}

int AudioMixer::getLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].loopCount;
}

int AudioMixer::getInitialLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].loopCountInitial;
}

AudioOutput AudioMixer::getOutput(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return AudioOutput::Stereo;
    return _channels[channel].output;
}

uint32_t AudioMixer::getSampleRate(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].sampleRate;
}

uint16_t AudioMixer::getNumChannels(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].numChannels;
}

uint16_t AudioMixer::getBitsPerSample(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].bitsPerSample;
}

uint32_t AudioMixer::getTotalSamples(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].totalSamples;
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
            MIXER_LOG("Ch%d: Stopped", channel);
            break;

        case AudioStopMode::Fade:
            ch.fading = true;
            ch.fadeVolume = 1.0f;
            ch.fadeStep = 1.0f / FADE_STEPS;
            MIXER_LOG("Ch%d: Fading out", channel);
            break;

        case AudioStopMode::LoopEnd:
            ch.loop = false;
            MIXER_LOG("Ch%d: Will stop at loop end", channel);
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
    _channels[channel].loopCount = 0;
}

void AudioMixer::stopLoopingAll() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i].loop = false;
        _channels[i].loopCount = 0;
    }
}

// ============================================================================
//  QUEUE CONTROL
// ============================================================================

bool AudioMixer::queueSound(int channel, const char* filename, const AudioPlaybackOptions& options,
                            QueueLoopBehavior loopBehavior) {
    if (!_initialized) return false;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    if (!filename) return false;
    
    Channel& ch = _channels[channel];
    
    // If channel is not playing, just play directly
    if (!ch.active) {
        return play(channel, filename, options);
    }
    
    // If current track is looping infinitely and no fixed loop count, 
    // we can only queue if we handle it via loopBehavior
    if (ch.loop && ch.loopCount == LOOP_INFINITE) {
        // For infinite loops, we need to handle based on loopBehavior
        if (loopBehavior == QueueLoopBehavior::StopImmediate) {
            // Stop current immediately and play new sound
            stop(channel, AudioStopMode::Immediate);
            return play(channel, filename, options);
        } else {
            // FinishLoop: mark that we want to stop after current loop finishes
            ch.loopCount = 0;  // Will finish after this iteration
            ch.hasQueuedItem = true;
            ch.pendingLoopBehavior = loopBehavior;
        }
    }
    
    return enqueueToChannel(channel, filename, options, loopBehavior);
}

bool AudioMixer::enqueueToChannel(int channel, const char* filename, const AudioPlaybackOptions& options,
                                   QueueLoopBehavior loopBehavior) {
    Channel& ch = _channels[channel];
    
    // Check if queue is full
    int nextHead = (ch.queueHead + 1) % QUEUE_SIZE_PER_CHANNEL;
    if (nextHead == ch.queueTail) {
        MIXER_LOG("Ch%d: Queue full, cannot enqueue %s", channel, filename);
        return false;
    }
    
    // Validate: looping items can only be queued if they have a fixed loop count
    if (options.loop && options.loopCount == LOOP_INFINITE) {
        MIXER_LOG("Ch%d: Cannot queue infinite loop, use fixed loop count", channel);
        return false;
    }
    
    // Add to queue
    QueuedSound& item = ch.queue[ch.queueHead];
    strncpy(item.filename, filename, sizeof(item.filename) - 1);
    item.filename[sizeof(item.filename) - 1] = '\0';
    item.options = options;
    item.loopBehavior = loopBehavior;
    item.valid = true;
    
    ch.queueHead = nextHead;
    ch.hasQueuedItem = true;
    
    MIXER_LOG("Ch%d: Queued %s (%s)", channel, filename,
              loopBehavior == QueueLoopBehavior::StopImmediate ? "stop-immediate" : "finish-loop");
    return true;
}

bool AudioMixer::dequeueFromChannel(int channel, QueuedSound& out) {
    Channel& ch = _channels[channel];
    
    if (ch.queueTail == ch.queueHead) {
        ch.hasQueuedItem = false;
        return false;
    }
    
    out = ch.queue[ch.queueTail];
    ch.queue[ch.queueTail].valid = false;
    ch.queueTail = (ch.queueTail + 1) % QUEUE_SIZE_PER_CHANNEL;
    
    // Update hasQueuedItem flag
    ch.hasQueuedItem = (ch.queueTail != ch.queueHead);
    
    return out.valid;
}

void AudioMixer::clearQueue(int channel) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    
    Channel& ch = _channels[channel];
    ch.queueHead = 0;
    ch.queueTail = 0;
    ch.hasQueuedItem = false;
    
    for (int i = 0; i < QUEUE_SIZE_PER_CHANNEL; i++) {
        ch.queue[i].valid = false;
    }
    
    MIXER_LOG("Ch%d: Queue cleared", channel);
}

void AudioMixer::clearAllQueues() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        clearQueue(i);
    }
}

int AudioMixer::queueLength(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    
    const Channel& ch = _channels[channel];
    int len = ch.queueHead - ch.queueTail;
    if (len < 0) len += QUEUE_SIZE_PER_CHANNEL;
    return len;
}

bool AudioMixer::hasQueuedSounds(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channels[channel].hasQueuedItem;
}

void AudioMixer::checkAndPlayNextQueued(int channel) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    
    Channel& ch = _channels[channel];
    
    QueuedSound nextSound;
    if (dequeueFromChannel(channel, nextSound)) {
        MIXER_LOG("Ch%d: Playing next from queue: %s", channel, nextSound.filename);
        play(channel, nextSound.filename, nextSound.options);
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
            // Check for queued sounds after fade ends
            checkAndPlayNextQueued(i);
        }

        // Handle end of file
        if (ch.samplesRemaining == 0) {
            bool shouldLoop = false;
            
            if (ch.loop) {
                if (ch.loopCount == LOOP_INFINITE) {
                    // Infinite loop - check for queued items
                    if (ch.hasQueuedItem) {
                        // Check the pending loop behavior
                        if (ch.pendingLoopBehavior == QueueLoopBehavior::FinishLoop) {
                            // Current loop iteration is finished, play next
                            shouldLoop = false;
                        } else {
                            // StopImmediate was already handled in queueSound()
                            shouldLoop = true;
                        }
                    } else {
                        shouldLoop = true;
                    }
                } else if (ch.loopCount > 0) {
                    // Finite loops remaining
                    ch.loopCount--;
                    shouldLoop = (ch.loopCount >= 0);
                    if (ch.loopCount == 0) {
                        ch.loop = false;  // No more loops after this
                    }
                }
            }
            
            if (shouldLoop) {
                ch.file.seek(ch.dataStart);
                ch.samplesRemaining = ch.totalSamples;
            } else {
                ch.active = false;
                if (ch.file) ch.file.close();
                _channelPlaying[i] = false;
                // Check for queued sounds after playback ends
                checkAndPlayNextQueued(i);
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
    // Calculate effective volume (channel * master * fade)
    float effectiveVolume = ch.volume * _masterVolume;
    
    // Normal file-based mixing
    int bytesPerSample = ch.numChannels * (ch.bitsPerSample / 8);
    int samplesToRead = min(static_cast<uint32_t>(samples), ch.samplesRemaining);
    int bytesToRead = samplesToRead * bytesPerSample;

    int bytesRead = ch.file.read(_readBuffer, bytesToRead);
    int samplesRead = bytesRead / bytesPerSample;

    // Apply fade if active
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
        case CommandType::QueueSound:
            queueSound(cmd.channelId, cmd.filename, cmd.options, cmd.loopBehavior);
            break;
        case CommandType::ClearQueue:
            if (cmd.channelId < 0) clearAllQueues();
            else clearQueue(cmd.channelId);
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

bool AudioMixer::queueSoundAsync(int channel, const char* filename, const AudioPlaybackOptions& options,
                                  QueueLoopBehavior loopBehavior) {
    if (!_dualCoreMode) return queueSound(channel, filename, options, loopBehavior);

    Command cmd{};
    cmd.type = CommandType::QueueSound;
    cmd.channelId = channel;
    strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
    cmd.options = options;
    cmd.loopBehavior = loopBehavior;
    return queueCommand(cmd);
}

void AudioMixer::clearQueueAsync(int channel) {
    if (!_dualCoreMode) {
        if (channel < 0) clearAllQueues();
        else clearQueue(channel);
        return;
    }

    Command cmd{};
    cmd.type = CommandType::ClearQueue;
    cmd.channelId = channel;
    queueCommand(cmd);
}

// ============================================================================
//  MOCK I2S STATISTICS
// ============================================================================
#if AUDIO_MOCK_I2S
void AudioMixer::printMockStatistics() {
    i2sOutput.printStatistics();
}

void AudioMixer::resetMockStatistics() {
    i2sOutput.resetStatistics();
    MIXER_LOG("Mock I2S statistics reset");
}
#endif
