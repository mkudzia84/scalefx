/**
 * Audio Mixer - Implementation
 * 
 * SPSC Ring Buffer Architecture (ESP32-S3):
 *   Core 1 — Producer Task: WAV decode + SD reads + float mixing → ring buffer
 *   Core 1 — Consumer Task: ring buffer → I2S DMA (higher priority)
 *   Core 0: Protocol handling only — uses *Async() API to queue commands
 * 
 * All audio math uses float, leveraging hardware FPU where available.
 * File I/O uses SdCardModule singleton directly.
 *
 * The controller instantiates AudioMixer<ConcreteI2S, ConcreteCodec>.
 * I2S output and codec are accessed as singletons via TI2S::instance()
 * and TCodec::instance(). No platform #ifdef blocks in this file.
 */

#if defined(SFX_HAS_AUDIO)

#include "audio_log.h"

// ============================================================================
//  CONSTANTS
// ============================================================================

constexpr int FADE_DURATION_MS = 50;
constexpr float FADE_STEP_PER_FRAME = 1.0f / ((FADE_DURATION_MS * AUDIO_SAMPLE_RATE) / 1000.0f);
constexpr float TWO_PI_F = 2.0f * 3.14159265358979f;

// ============================================================================
//  STATIC HELPERS
// ============================================================================

// Reference to ring buffer singleton
static inline AudioRingBuffer& ringBuf() {
    return AudioRingBuffer::instance();
}

// ============================================================================
//  LIFECYCLE
// ============================================================================

template<typename TI2S, typename TCodec>
AudioMixer<TI2S, TCodec>::~AudioMixer() {
    shutdown();
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::begin(uint8_t i2s_data_pin, uint8_t i2s_bclk_pin,
                       uint8_t i2s_lrclk_pin) {
    if (_initialized) return true;

    _masterVolume = 1.0f;

    // ---- Allocate ring buffer from PSRAM ----
    if (!ringBuf().init()) {
        MIXER_ERROR("Ring buffer allocation failed (%d KB)", RING_BYTES / 1024);
        return false;
    }

    // ---- Allocate SD read buffer from PSRAM ----
    if (!_sdReadBuf) {
        _sdReadBuf = static_cast<uint8_t*>(sfxPsramMalloc(WAV_SD_READ_BYTES));
        if (!_sdReadBuf) {
            MIXER_ERROR("SD read buffer allocation failed (%d bytes)", WAV_SD_READ_BYTES);
            return false;
        }
    }

    // ---- Allocate per-channel WAV decode buffers from PSRAM ----
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i] = Channel{};
        _channelPlaying[i] = false;
        _channelRemainingSec[i] = 0.0f;

        // Allocate L+R float buffers for WAV decode
        _channels[i].wav.bufL = static_cast<float*>(
            sfxPsramCalloc(WAV_BUF_FRAMES, sizeof(float)));
        _channels[i].wav.bufR = static_cast<float*>(
            sfxPsramCalloc(WAV_BUF_FRAMES, sizeof(float)));

        if (!_channels[i].wav.bufL || !_channels[i].wav.bufR) {
            MIXER_ERROR("WAV buffer alloc failed for ch %d", i);
            // Clean up already-allocated buffers
            for (int j = 0; j <= i; j++) {
                sfxPsramFree(_channels[j].wav.bufL);
                sfxPsramFree(_channels[j].wav.bufR);
                _channels[j].wav.bufL = nullptr;
                _channels[j].wav.bufR = nullptr;
            }
            sfxPsramFree(_sdReadBuf);
            _sdReadBuf = nullptr;
            return false;
        }
    }

    // Reset ring buffer
    ringBuf().reset();
    _underruns = 0;
    
    // Store I2S pins for beginI2S() (called from Core 1)
    _i2sDataPin  = i2s_data_pin;
    _i2sBclkPin  = i2s_bclk_pin;
    _i2sLrclkPin = i2s_lrclk_pin;
    
    
    // Initialize command queue mutex
    sfxMutexInit(_cmdMutex);
    _cmdQueueHead = 0;
    _cmdQueueTail = 0;

    // Calculate total PSRAM consumed by audio subsystem
    int wavBufTotal_KB = (AUDIO_MAX_CHANNELS * 2 * WAV_BUF_FRAMES * sizeof(float)) / 1024;
    int sdBuf_KB = WAV_SD_READ_BYTES / 1024;
    int ringBuf_KB = RING_BYTES / 1024;
    int totalPsram_KB = wavBufTotal_KB + sdBuf_KB + ringBuf_KB;
    
    _initialized.store(true, std::memory_order_release);  // visible to Core 1
    
    MIXER_LOG("Phase 1 complete: %d ch, ring=%dKB, wav=%dKB, sd=%dKB (total PSRAM=%dKB), codec=%s",
             AUDIO_MAX_CHANNELS, ringBuf_KB, wavBufTotal_KB, sdBuf_KB, totalPsram_KB,
             TCodec::instance().getModelName());
    return true;
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::beginI2S() {
    // MUST be called from Core 1 — I2S library expects init and write on same core
    if (_i2sRunning) return true;
    if (!_initialized) {
        MIXER_ERROR("beginI2S() called before begin() - init order wrong");
        return false;
    }
    
    MIXER_LOG("Phase 2: configuring I2S on Core 1 (data=GP%d, bclk=GP%d, lrclk=GP%d)",
             _i2sDataPin, _i2sBclkPin, _i2sLrclkPin);

    I2SPinConfig pins{_i2sDataPin, _i2sBclkPin, _i2sLrclkPin};
    if (!TI2S::instance().begin(pins, AUDIO_SAMPLE_RATE, AUDIO_BIT_DEPTH)) {
        MIXER_ERROR("I2S backend init failed (%s)", TI2S::instance().backendName());
        return false;
    }

    // No pre-fill needed — consumer writes silence to I2S when ring is empty
    // (tx_desc_auto_clear = true in I2S config). Keeping the ring empty at
    // startup ensures low latency when the first play command arrives.

    _i2sRunning.store(true, std::memory_order_release);  // visible to Core 0
    
    MIXER_LOG("Phase 2 complete: I2S %uHz/%dbit running on Core 1 (%s, data=GP%d, bclk=GP%d)",
             AUDIO_SAMPLE_RATE, AUDIO_BIT_DEPTH, TI2S::instance().backendName(),
             _i2sDataPin, _i2sBclkPin);
    return true;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::shutdown() {
    if (!_initialized) return;

#if SFX_PLATFORM_ESP32
    // Stop producer task first (it accesses channels and SD)
    stopProducerTask();
#endif

    // Stop all playback
    stopAll(AudioStopMode::Immediate);

    // Close all open files
    SdCardModule& sd = SdCardModule::instance();
    sd.lock();
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (_channels[i].wav.file) {
            _channels[i].wav.file.close();
        }
    }
    sd.unlock();

    // Stop I2S — set flag BEFORE calling end() so Core 1's consume()
    // sees the flag and stops writing to i2sOutput during teardown
    if (_i2sRunning.load(std::memory_order_acquire)) {
        _i2sRunning.store(false, std::memory_order_release);
        SFX_DELAY_MS(2);  // Let Core 1 drain current consumeAndOutput() iteration

        TI2S::instance().end();
    }

    _initialized.store(false, std::memory_order_release);

    // ---- Free PSRAM audio buffers ----
    // Free per-channel WAV decode buffers
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        sfxPsramFree(_channels[i].wav.bufL);
        sfxPsramFree(_channels[i].wav.bufR);
        _channels[i].wav.bufL = nullptr;
        _channels[i].wav.bufR = nullptr;
    }

    // Free shared SD read buffer
    sfxPsramFree(_sdReadBuf);
    _sdReadBuf = nullptr;

    // Free ring buffer
    ringBuf().shutdown();

    MIXER_LOG("Shutdown complete (PSRAM freed)");
}

// ============================================================================
//  PAN CALCULATION
// ============================================================================

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::updatePan(Channel& ch) {
    // Constant-power panning: preserves perceived loudness at center
    //   pan=-1 → L=1.0, R=0.0 | pan=0 → L≈0.707, R≈0.707 | pan=+1 → L=0.0, R=1.0
    float angle = (ch.pan + 1.0f) * 3.14159265f * 0.25f;  // maps [-1,+1] → [0, π/2]
    ch.panL = cosf(angle);
    ch.panR = sinf(angle);
}

// ============================================================================
//  PLAYBACK CONTROL
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::play(int channel, const char* filename, const AudioPlaybackOptions& options) {
    if (!_initialized) {
        MIXER_ERROR("play() called but mixer not initialized");
        return false;
    }
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) {
        MIXER_ERROR("play() invalid channel: %d", channel);
        return false;
    }
    if (!filename) {
        MIXER_ERROR("play() null filename on ch%d", channel);
        return false;
    }

    Channel& ch = _channels[channel];
    WavState& ws = ch.wav;

    // Stop current playback on this channel
    if (ws.active && ws.file) {
        ws.file.close();
    }
    ws.active = false;
    ws.bufLen = 0;
    ws.bufPos = 0;
    ws.framesRead = 0;
    ws.resampleFrac = 0.0f;

    // Open the WAV file via SdCardModule singleton
    // SD mutex protects all file I/O — required when producer runs on
    // Core 1 while storage operations continue on Core 0.
    SdCardModule& sd = SdCardModule::instance();
    if (!sd.isInitialized()) {
        MIXER_ERROR("Ch%d: SD card not initialized", channel);
        return false;
    }
    sd.lock();
    uint8_t sdErr = sd.openRead(filename, ws.file);
    if (sdErr != 0) {
        sd.unlock();
        MIXER_ERROR("Ch%d: Failed to open: %s (err=%d)", channel, filename, sdErr);
        return false;
    }

    // Parse WAV header (still under SD lock — reads from file)
    if (!parseWavHeader(ws)) {
        ws.file.close();
        sd.unlock();
        MIXER_ERROR("Ch%d: Invalid WAV: %s", channel, filename);
        return false;
    }
    sd.unlock();

    // Configure resampler
    ws.resampleRatio = (float)ws.sampleRate_Hz / (float)AUDIO_SAMPLE_RATE;
    ws.resampleFrac = 0.0f;
    ws.bufLen = 0;
    ws.bufPos = 0;

    // Apply playback options
    if (options.loop && options.loopCount == LOOP_INFINITE) {
        ws.loop = true;
        ws.loopCount = LOOP_INFINITE;
        ws.loopCountInit = LOOP_INFINITE;
    } else if (options.loopCount > 0) {
        ws.loop = true;
        ws.loopCount = options.loopCount;
        ws.loopCountInit = options.loopCount;
    } else if (options.loopCount == 0 || !options.loop) {
        ws.loop = false;
        ws.loopCount = 0;
        ws.loopCountInit = 0;
    } else {
        ws.loop = true;
        ws.loopCount = LOOP_INFINITE;
        ws.loopCountInit = LOOP_INFINITE;
    }
    
    ch.volume = constrain(options.volume, 0.0f, 1.0f);
    ch.outputChannels = options.outputChannels;
    ch.fading = false;
    ch.fadeVolume = 1.0f;
    ch.mute = false;
    ch.pan = 0.0f;
    updatePan(ch);

    // Handle start offset
    if (options.startOffsetMs > 0) {
        uint32_t offsetFrames = (static_cast<uint32_t>(options.startOffsetMs) * ws.sampleRate_Hz) / 1000;
        if (offsetFrames < ws.totalFrames) {
            uint32_t bytesPerFrame = ws.numChannels * (ws.bitsPerSample / 8);
            ws.file.seek(ws.dataStart + offsetFrames * bytesPerFrame);
            ws.framesRead = offsetFrames;
        }
    }

    // Store filename for status display
    strncpy(ch.filename, filename, CHANNEL_FILENAME_MAX - 1);
    ch.filename[CHANNEL_FILENAME_MAX - 1] = '\0';

    // Pre-fill WAV buffer — loop until full (multiple SD reads) so the
    // channel can sustain playback for ~0.5 s even if the SD stalls.
    while (ws.bufLen < WAV_BUF_FRAMES) {
        if (!refillWavBuffer(ws)) break;  // EOF or error
    }
    MIXER_LOG("Ch%d: Pre-filled WAV buffer %d/%d frames (%.0f ms)",
             channel, ws.bufLen, WAV_BUF_FRAMES,
             (float)ws.bufLen / AUDIO_SAMPLE_RATE * 1000.0f);
    
    ws.active = true;
    _channelPlaying[channel] = true;

    // Format loop info for log
    const char* loopStr;
    char loopBuf[16];
    if (ws.loopCount == LOOP_INFINITE) {
        loopStr = "loop";
    } else if (ws.loopCount > 0) {
        snprintf(loopBuf, sizeof(loopBuf), "x%d", ws.loopCount);
        loopStr = loopBuf;
    } else {
        loopStr = "once";
    }
    
    float duration_s = (float)ws.totalFrames / (float)ws.sampleRate_Hz;
    MIXER_LOG("Ch%d: Playing %s (%s, vol=%.2f, %luHz %dbit %s, %.1fs%s)",
             channel, filename, loopStr, ch.volume,
             (unsigned long)ws.sampleRate_Hz, ws.bitsPerSample,
             ws.numChannels == 2 ? "stereo" : "mono",
             (double)duration_s,
             ws.resampleRatio != 1.0f ? " [resample]" : "");
    return true;
}

// ============================================================================
//  CHANNEL INFO GETTERS
// ============================================================================

template<typename TI2S, typename TCodec>
const char* AudioMixer<TI2S, TCodec>::getFilename(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return nullptr;
    return _channels[channel].wav.active ? _channels[channel].filename : nullptr;
}

template<typename TI2S, typename TCodec>
float AudioMixer<TI2S, TCodec>::getChannelVolume(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0.0f;
    return _channels[channel].volume;
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::isLooping(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channels[channel].wav.loop;
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.loopCount;
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getInitialLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.loopCountInit;
}

template<typename TI2S, typename TCodec>
uint8_t AudioMixer<TI2S, TCodec>::getOutputChannels(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return AudioChannel::ALL;
    return _channels[channel].outputChannels;
}

template<typename TI2S, typename TCodec>
uint32_t AudioMixer<TI2S, TCodec>::getSampleRate(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.sampleRate_Hz;
}

template<typename TI2S, typename TCodec>
uint16_t AudioMixer<TI2S, TCodec>::getNumChannels(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.numChannels;
}

template<typename TI2S, typename TCodec>
uint16_t AudioMixer<TI2S, TCodec>::getBitsPerSample(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.bitsPerSample;
}

template<typename TI2S, typename TCodec>
uint32_t AudioMixer<TI2S, TCodec>::getTotalSamples(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.totalFrames;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stop(int channel, AudioStopMode mode) {
    if (!_initialized) return;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;

    Channel& ch = _channels[channel];
    WavState& ws = ch.wav;
    if (!ws.active) return;

    switch (mode) {
        case AudioStopMode::Immediate:
            ws.active = false;
            ws.bufLen = 0;
            ws.bufPos = 0;
            if (ws.file) {
                SdCardModule::instance().lock();
                ws.file.close();
                SdCardModule::instance().unlock();
            }
            _channelPlaying[channel] = false;
            MIXER_LOG("Ch%d: Stopped", channel);
            break;

        case AudioStopMode::Fade:
            ch.fading = true;
            ch.fadeVolume = 1.0f;
            ch.fadeStep = FADE_STEP_PER_FRAME;
            MIXER_LOG("Ch%d: Fading out", channel);
            break;

        case AudioStopMode::LoopEnd:
            ws.loop = false;
            MIXER_LOG("Ch%d: Will stop at loop end", channel);
            break;
    }
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopAll(AudioStopMode mode) {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        stop(i, mode);
    }
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopLooping(int channel) {
    if (!_initialized) return;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].wav.loop = false;
    _channels[channel].wav.loopCount = 0;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopLoopingAll() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i].wav.loop = false;
        _channels[i].wav.loopCount = 0;
    }
}

// ============================================================================
//  QUEUE CONTROL
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::queueSound(int channel, const char* filename, const AudioPlaybackOptions& options,
                            QueueLoopBehavior loopBehavior) {
    if (!_initialized) return false;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    if (!filename) return false;
    
    Channel& ch = _channels[channel];
    WavState& ws = ch.wav;
    
    // If channel is not playing, just play directly
    if (!ws.active) {
        return play(channel, filename, options);
    }
    
    // If current track is looping infinitely, handle via loopBehavior
    if (ws.loop && ws.loopCount == LOOP_INFINITE) {
        if (loopBehavior == QueueLoopBehavior::StopImmediate) {
            // Stop current immediately and play new sound
            stop(channel, AudioStopMode::Immediate);
            return play(channel, filename, options);
        } else {
            // FinishLoop: mark that we want to stop after current loop finishes
            ws.loopCount = 0;  // Will finish after this iteration
            ch.hasQueuedItem = true;
            ch.pendingLoopBehavior = loopBehavior;
        }
    }
    
    return enqueueToChannel(channel, filename, options, loopBehavior);
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::enqueueToChannel(int channel, const char* filename, const AudioPlaybackOptions& options,
                                   QueueLoopBehavior loopBehavior) {
    Channel& ch = _channels[channel];
    
    // Check if queue is full
    int nextHead = (ch.queueHead + 1) % QUEUE_SIZE_PER_CHANNEL;
    if (nextHead == ch.queueTail) {
        MIXER_WARN("Ch%d: Queue full, cannot enqueue %s", channel, filename);
        return false;
    }
    
    // Validate: looping items can only be queued if they have a fixed loop count
    if (options.loop && options.loopCount == LOOP_INFINITE) {
        MIXER_WARN("Ch%d: Cannot queue infinite loop, use fixed loop count", channel);
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

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::dequeueFromChannel(int channel, QueuedSound& out) {
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

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::clearQueue(int channel) {
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

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::clearAllQueues() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        clearQueue(i);
    }
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::queueLength(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    
    const Channel& ch = _channels[channel];
    int len = ch.queueHead - ch.queueTail;
    if (len < 0) len += QUEUE_SIZE_PER_CHANNEL;
    return len;
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::hasQueuedSounds(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channels[channel].hasQueuedItem;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::checkAndPlayNextQueued(int channel) {
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

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::setVolume(int channel, float vol) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].volume = constrain(vol, 0.0f, 1.0f);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::setMasterVolume(float vol) {
    _masterVolume = constrain(vol, 0.0f, 1.0f);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::setOutputChannels(int channel, uint8_t channelMask) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].outputChannels = channelMask;
}

template<typename TI2S, typename TCodec>
float AudioMixer<TI2S, TCodec>::volume(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0.0f;
    return _channels[channel].volume;
}

// ============================================================================
//  STATUS
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::isPlaying(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channelPlaying[channel];
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::isAnyPlaying() const {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (_channelPlaying[i]) return true;
    }
    return false;
}

template<typename TI2S, typename TCodec>
float AudioMixer<TI2S, TCodec>::remainingSec(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return -1.0f;
    if (!_channelPlaying[channel]) return -1.0f;
    if (_channels[channel].wav.loop) return -1.0f;
    return _channelRemainingSec[channel];
}

// ============================================================================
//  WAV PARSING
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::parseWavHeader(WavState& ws) {
    uint8_t header[44];

    if (ws.file.read(header, 44) != 44) {
        return false;
    }

    // Validate RIFF header
    if (memcmp(header, "RIFF", 4) != 0) return false;
    if (memcmp(header + 8, "WAVE", 4) != 0) return false;
    if (memcmp(header + 12, "fmt ", 4) != 0) return false;

    // Check PCM format (audioFormat == 1)
    uint16_t audioFormat = header[20] | (header[21] << 8);
    if (audioFormat != 1) return false;

    // Extract audio properties
    ws.numChannels   = header[22] | (header[23] << 8);
    ws.sampleRate_Hz = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    ws.bitsPerSample = header[34] | (header[35] << 8);

    // Validate supported formats
    if (ws.numChannels < 1 || ws.numChannels > 2) return false;
    if (ws.bitsPerSample != 8 && ws.bitsPerSample != 16) return false;

    // Find data chunk (may not be at fixed offset)
    ws.file.seek(12);
    while (ws.file.available()) {
        uint8_t chunkHeader[8];
        if (ws.file.read(chunkHeader, 8) != 8) return false;

        uint32_t chunkSize = chunkHeader[4] | (chunkHeader[5] << 8) | 
                            (chunkHeader[6] << 16) | (chunkHeader[7] << 24);

        if (memcmp(chunkHeader, "data", 4) == 0) {
            ws.dataStart = ws.file.position();
            uint32_t bytesPerFrame = ws.numChannels * (ws.bitsPerSample / 8);
            ws.totalFrames = chunkSize / bytesPerFrame;
            ws.framesRead  = 0;
            return true;
        }

        ws.file.seek(ws.file.position() + chunkSize);
    }

    return false;
}

// ============================================================================
//  WAV BUFFER REFILL (Producer side — Core 1 task on ESP32, Core 0 on Pico)
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::refillWavBuffer(WavState& ws) {
    // Move remaining frames to front of buffer
    int remaining = ws.bufLen - ws.bufPos;
    if (remaining > 0 && ws.bufPos > 0) {
        memmove(ws.bufL, ws.bufL + ws.bufPos, remaining * sizeof(float));
        memmove(ws.bufR, ws.bufR + ws.bufPos, remaining * sizeof(float));
    }
    ws.bufPos = 0;
    ws.bufLen = remaining;

    int space = WAV_BUF_FRAMES - remaining;
    if (space <= 0) return true;

    // How many frames left in file?
    int framesLeft = (int)(ws.totalFrames - ws.framesRead);
    if (framesLeft <= 0) {
        if (ws.loop) {
            // Handle loop count
            if (ws.loopCount != LOOP_INFINITE) {
                if (ws.loopCount <= 0) {
                    ws.loop = false;
                    // Don't set ws.active = false here — the WAV decode
                    // buffer may still have frames to play.  produceFrame()
                    // will keep mixing until getWavSample() returns false
                    // (buffer empty), which triggers proper cleanup.
                    return ws.bufLen > 0;
                }
                ws.loopCount--;
            }
            // SD lock for file seek (cross-core safety with storage ops)
            SdCardModule::instance().lock();
            ws.file.seek(ws.dataStart);
            SdCardModule::instance().unlock();
            ws.framesRead = 0;
            framesLeft = (int)ws.totalFrames;
        } else {
            // Don't set ws.active = false here — the WAV decode
            // buffer may still have frames to play.  produceFrame()
            // will keep mixing until getWavSample() returns false
            // (buffer empty), which triggers proper cleanup and
            // sets _channelPlaying = false.
            return ws.bufLen > 0;
        }
    }

    int framesToRead  = min(space, framesLeft);
    int bytesPerFrame = ws.numChannels * (ws.bitsPerSample / 8);
    int bytesToRead   = framesToRead * bytesPerFrame;
    if (bytesToRead > WAV_SD_READ_BYTES) {
        bytesToRead  = WAV_SD_READ_BYTES;
        framesToRead = bytesToRead / bytesPerFrame;
    }

    // SD lock for file read (cross-core safety with storage ops)
    SdCardModule::instance().lock();
    int bytesRead = ws.file.read(_sdReadBuf, bytesToRead);
    SdCardModule::instance().unlock();
    int framesGot = bytesRead / bytesPerFrame;
    ws.framesRead += framesGot;

    // Convert raw PCM to float [-1.0, +1.0]
    for (int i = 0; i < framesGot; i++) {
        float l, r;
        if (ws.bitsPerSample == 16) {
            const int16_t* buf16 = (const int16_t*)_sdReadBuf;
            if (ws.numChannels == 2) {
                l = buf16[i * 2]     * (1.0f / 32768.0f);
                r = buf16[i * 2 + 1] * (1.0f / 32768.0f);
            } else {
                l = r = buf16[i] * (1.0f / 32768.0f);
            }
        } else {  // 8-bit unsigned
            if (ws.numChannels == 2) {
                l = (_sdReadBuf[i * 2]     - 128) * (1.0f / 128.0f);
                r = (_sdReadBuf[i * 2 + 1] - 128) * (1.0f / 128.0f);
            } else {
                l = r = (_sdReadBuf[i] - 128) * (1.0f / 128.0f);
            }
        }
        ws.bufL[remaining + i] = l;
        ws.bufR[remaining + i] = r;
    }
    ws.bufLen = remaining + framesGot;
    return ws.bufLen > 0;
}

// ============================================================================
//  WAV SAMPLE RETRIEVAL WITH RESAMPLING (Producer side)
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::getWavSample(WavState& ws, float& outL, float& outR) {
    // Ensure at least 2 frames available for linear interpolation
    while (ws.bufPos + 1 >= ws.bufLen) {
        if (!refillWavBuffer(ws)) {
            outL = outR = 0.0f;
            return false;
        }
        // Safety: if refill returned true but still not enough data for
        // interpolation, no more progress is possible (e.g., end of file
        // with exactly 1 frame remaining). Treat as end of track to avoid
        // an infinite loop that blocks the entire main loop.
        if (ws.bufPos + 1 >= ws.bufLen) {
            outL = outR = 0.0f;
            return false;
        }
    }

    int   idx  = ws.bufPos;
    float frac = ws.resampleFrac;

    // Linear interpolation between adjacent source frames
    outL = ws.bufL[idx] + (ws.bufL[idx + 1] - ws.bufL[idx]) * frac;
    outR = ws.bufR[idx] + (ws.bufR[idx + 1] - ws.bufR[idx]) * frac;

    // Advance resampler: step through source at ratio = srcRate / outputRate
    ws.resampleFrac += ws.resampleRatio;
    int advance = (int)ws.resampleFrac;
    ws.bufPos       += advance;
    ws.resampleFrac -= (float)advance;

    return true;
}

// ============================================================================
//  FLOAT MIXING PIPELINE — PRODUCER (Core 1 task on ESP32, Core 0 on Pico)
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::produceFrame() {
    // Fill ring to capacity.  The ring is a small FIFO between cores (~85 ms),
    // so filling it fully adds negligible latency.  SD resilience comes from
    // the large per-channel WAV decode buffers (0.5 s each).
    if (ringBuf().isFull()) return false;

    float mixL = 0.0f;
    float mixR = 0.0f;
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;
        
        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        float trackL = 0.0f;
        float trackR = 0.0f;

        // Get one resampled stereo frame from WAV
        if (!getWavSample(ws, trackL, trackR)) {
            // End of playback — close file and clean up
            ws.active = false;
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            if (ws.file) {
                SdCardModule::instance().lock();
                ws.file.close();
                SdCardModule::instance().unlock();
            }
            checkAndPlayNextQueued(i);
            continue;
        }

        // Apply fade if active
        float effectiveVolume = ch.volume;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep;
            if (ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                ws.active = false;
                _channelPlaying[i] = false;
                if (ws.file) {
                    SdCardModule::instance().lock();
                    ws.file.close();
                    SdCardModule::instance().unlock();
                }
                checkAndPlayNextQueued(i);
                continue;
            }
        }

        hasAudio = true;

        // Apply track volume
        trackL *= effectiveVolume;
        trackR *= effectiveVolume;

        // Apply output routing (bitmask: CH1=0x01, CH2=0x02)
        if (ch.outputChannels == AudioChannel::ALL) {
            // Both channels: apply constant-power pan
            trackL *= ch.panL;
            trackR *= ch.panR;
        } else {
            // Selective: zero disabled channels
            if (!(ch.outputChannels & AudioChannel::CH1)) trackL = 0.0f;
            if (!(ch.outputChannels & AudioChannel::CH2)) trackR = 0.0f;
        }

        // Accumulate into stereo mix
        mixL += trackL;
        mixR += trackR;

        // Update remaining time status (float seconds — avoids integer overflow for long files)
        if (ws.sampleRate_Hz > 0) {
            uint32_t framesLeft = ws.totalFrames - ws.framesRead + (ws.bufLen - ws.bufPos);
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }
    }

    // Don't buffer silence — the consumer writes silence to I2S directly
    // when the ring is empty. Skipping silence keeps the ring near-empty
    // while idle, so new audio reaches the output within milliseconds.
    if (!hasAudio) return false;

    // Apply master volume
    mixL *= _masterVolume;
    mixR *= _masterVolume;

    // Clamp to [-1.0, +1.0]
    if (mixL >  1.0f) mixL =  1.0f;
    if (mixL < -1.0f) mixL = -1.0f;
    if (mixR >  1.0f) mixR =  1.0f;
    if (mixR < -1.0f) mixR = -1.0f;

    // Convert to int16 for ring buffer
    int16_t sampleL = (int16_t)(mixL * MAX_AMPLITUDE);
    int16_t sampleR = (int16_t)(mixR * MAX_AMPLITUDE);

    ringBuf().write(sampleL, sampleR);
    return true;
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::produce(int maxFrames) {
    if (!_initialized) return 0;
    
    // Process any pending commands first
    processCommands();

    // Produce frames into ring buffer FIRST — this is time-critical.
    // The ring is a small core-to-core FIFO (~85 ms) that must stay fed
    // to prevent consumer underruns.
    int produced = 0;
    for (int i = 0; i < maxFrames; i++) {
        if (!produceFrame()) break;
        produced++;
    }

    // Proactively top-up ONE channel's WAV decode buffer per call
    // (round-robin).  This spreads SD I/O across loop iterations and
    // avoids the expensive memmove + SD read blocking produce().
    // Only triggers when buffer drops below 50% — at that point there
    // is still 250 ms of audio remaining, plenty of runway.
    {
        static int refillIdx = 0;
        for (int attempt = 0; attempt < AUDIO_MAX_CHANNELS; attempt++) {
            int ch = refillIdx % AUDIO_MAX_CHANNELS;
            refillIdx++;
            WavState& ws = _channels[ch].wav;
            if (!ws.active) continue;
            int available = ws.bufLen - ws.bufPos;
            if (available < WAV_BUF_FRAMES / 2) {
                refillWavBuffer(ws);   // one SD batch (~4096 frames)
            }
            break;  // one channel per call
        }
    }

    return produced;
}

// ============================================================================
//  I2S OUTPUT — CONSUMER (Core 1)
// ============================================================================

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::consumeAndOutput() {
    uint32_t avail = ringBuf().availableRead();
    
    static bool firstLog = true;
    if (firstLog) {
        MIXER_LOG("Consumer first call: avail=%u init=%d i2s=%d",
                  avail, (bool)_initialized, (bool)_i2sRunning);
        firstLog = false;
    }
    
    if (avail > 0) {
        // Read frames from ring buffer and write to I2S backend.
        // i2s_write(portMAX_DELAY) blocks when DMA buffers are full,
        // naturally pacing the consumer at the audio sample rate.
        uint32_t count = (avail > 512) ? 512 : avail;
        StereoFrame frames[512];
        for (uint32_t i = 0; i < count; i++) {
            frames[i] = ringBuf().read();
        }
        TI2S::instance().writeSamples(frames, count);
        _consumeFrames.fetch_add(count, std::memory_order_relaxed);
    } else {
        // Ring empty — yield and wait for producer to refill.
        // auto_clear_after_cb=true in the I2S channel config ensures
        // DMA buffers are zeroed after transmission, so no manual
        // silence flush is needed (which would inject audible gaps
        // if the ring briefly empties during active playback).
        _underruns.fetch_add(1, std::memory_order_relaxed);
        SFX_DELAY_MS(1);
    }
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::consume() {
    _consumeLoops.fetch_add(1, std::memory_order_relaxed);
    if (!_initialized || !_i2sRunning) return;
    consumeAndOutput();
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::process() {
    // Legacy single-call: does both produce and consume
    // For optimal performance, call produce() and consume() separately
    // from their respective cores
    produce(256);
    consume();
}

// ============================================================================
//  RING BUFFER STATS
// ============================================================================

template<typename TI2S, typename TCodec>
uint32_t AudioMixer<TI2S, TCodec>::getRingAvailableRead() const {
    return ringBuf().availableRead();
}

template<typename TI2S, typename TCodec>
uint32_t AudioMixer<TI2S, TCodec>::getRingAvailableWrite() const {
    return ringBuf().availableWrite();
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getRingFillPercent() const {
    return ringBuf().fillPercent();
}

// ============================================================================
//  WAV DECODE BUFFER STATS
// ============================================================================

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getWavBufferFillPercent(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    const WavState& ws = _channels[channel].wav;
    if (!ws.active || WAV_BUF_FRAMES == 0) return 0;
    int available = ws.bufLen - ws.bufPos;
    return (available * 100) / WAV_BUF_FRAMES;
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getWavBufferFrames(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    const WavState& ws = _channels[channel].wav;
    if (!ws.active) return 0;
    return ws.bufLen - ws.bufPos;
}

// ============================================================================
//  DUAL-CORE COMMAND QUEUE
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::queueCommand(const Command& cmd) {
    MIXER_LOG(">>> queueCommand: type=%d ch=%d file=%s",
             (int)cmd.type, cmd.channelId, cmd.filename[0] ? cmd.filename : "(none)");
    
    sfxMutexLock(_cmdMutex);

    int nextHead = (_cmdQueueHead.load(std::memory_order_relaxed) + 1) % AUDIO_CMD_QUEUE_SIZE;
    if (nextHead == _cmdQueueTail.load(std::memory_order_relaxed)) {
        sfxMutexUnlock(_cmdMutex);
        MIXER_WARN("Command queue full, dropping command");
        return false;
    }

    _cmdQueue[_cmdQueueHead.load(std::memory_order_relaxed)] = cmd;
    _cmdQueueHead.store(nextHead, std::memory_order_release);

    sfxMutexUnlock(_cmdMutex);
    MIXER_LOG("<<< Cmd queued: type=%d ch=%d (head=%d tail=%d)",
             (int)cmd.type, cmd.channelId, _cmdQueueHead.load(std::memory_order_relaxed), _cmdQueueTail.load(std::memory_order_relaxed));
    return true;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::processCommands() {
    int processed = 0;
    while (_cmdQueueTail.load(std::memory_order_acquire) != _cmdQueueHead.load(std::memory_order_acquire)) {
        sfxMutexLock(_cmdMutex);
        Command cmd = _cmdQueue[_cmdQueueTail.load(std::memory_order_relaxed)];
        _cmdQueueTail.store((_cmdQueueTail.load(std::memory_order_relaxed) + 1) % AUDIO_CMD_QUEUE_SIZE, std::memory_order_release);
        sfxMutexUnlock(_cmdMutex);

        MIXER_LOG("processCommands: dequeued type=%d ch=%d (tail now=%d)",
                 (int)cmd.type, cmd.channelId, _cmdQueueTail.load(std::memory_order_relaxed));
        executeCommand(cmd);
        processed++;
    }
    if (processed > 0) {
        MIXER_LOG("processCommands: processed %d commands", processed);
    }
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::executeCommand(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::Play: {
            MIXER_LOG("executeCommand: PLAY ch%d: %s", cmd.channelId, cmd.filename);
            bool result = play(cmd.channelId, cmd.filename, cmd.options);
            MIXER_LOG("executeCommand: play() returned %s", result ? "true" : "false");
            break;
        }
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
            setOutputChannels(cmd.channelId, cmd.outputChannels);
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
//  ASYNC API (Safe to call from any core — uses command queue)
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::playAsync(int channel, const char* filename, const AudioPlaybackOptions& options) {
    Command cmd{};
    cmd.type = CommandType::Play;
    cmd.channelId = channel;
    strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
    cmd.options = options;
    return queueCommand(cmd);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopAsync(int channel, AudioStopMode mode) {
    Command cmd{};
    cmd.type = (channel < 0) ? CommandType::StopAll : CommandType::Stop;
    cmd.channelId = channel;
    cmd.stopMode = mode;
    queueCommand(cmd);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::setVolumeAsync(int channel, float vol) {
    Command cmd{};
    cmd.type = CommandType::SetVolume;
    cmd.channelId = channel;
    cmd.volume = vol;
    queueCommand(cmd);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::setMasterVolumeAsync(float vol) {
    Command cmd{};
    cmd.type = CommandType::SetMasterVolume;
    cmd.volume = vol;
    queueCommand(cmd);
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::queueSoundAsync(int channel, const char* filename, const AudioPlaybackOptions& options,
                                  QueueLoopBehavior loopBehavior) {
    Command cmd{};
    cmd.type = CommandType::QueueSound;
    cmd.channelId = channel;
    strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
    cmd.options = options;
    cmd.loopBehavior = loopBehavior;
    return queueCommand(cmd);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::clearQueueAsync(int channel) {
    Command cmd{};
    cmd.type = CommandType::ClearQueue;
    cmd.channelId = channel;
    queueCommand(cmd);
}

// ============================================================================
//  PRODUCER TASK (ESP32 FreeRTOS — runs on Core 1 alongside consumer)
// ============================================================================

#if SFX_PLATFORM_ESP32

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::producerTaskFunc(void* param) {
    auto& mixer = AudioMixer::instance();
    MIXER_LOG("Producer task started on core %d (priority %d)",
              xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    while (mixer._producerRunning.load(std::memory_order_acquire)) {
        int produced = mixer.produce(RING_FRAMES);

        if (produced == 0) {
            // Ring full or no channels playing — yield CPU.
            // 2ms sleep lets the consumer drain ~96 frames of DMA space
            // before the producer wakes to refill.  When idle (no audio),
            // this prevents the task from busy-spinning.
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        // If produced > 0, loop immediately.  The FreeRTOS scheduler will
        // preempt us when the higher-priority consumer task unblocks from
        // i2s_channel_write().
    }

    MIXER_LOG("Producer task exiting");
    vTaskDelete(nullptr);
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::startProducerTask(int core, int priority, int stackSize) {
    if (_producerRunning.load(std::memory_order_acquire)) return true;
    if (!_initialized.load(std::memory_order_acquire)) {
        MIXER_ERROR("startProducerTask() called before begin()");
        return false;
    }

    _producerRunning.store(true, std::memory_order_release);

    BaseType_t result = xTaskCreatePinnedToCore(
        producerTaskFunc,
        "AudioProducer",
        stackSize,
        nullptr,
        priority,
        &_producerTaskHandle,
        core
    );

    if (result != pdPASS) {
        _producerRunning.store(false, std::memory_order_release);
        MIXER_ERROR("Failed to create producer task (err=%d)", result);
        return false;
    }

    MIXER_LOG("Producer task created: core=%d priority=%d stack=%d", core, priority, stackSize);
    return true;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopProducerTask() {
    if (!_producerRunning.load(std::memory_order_acquire)) return;

    MIXER_LOG("Stopping producer task...");
    _producerRunning.store(false, std::memory_order_release);

    // Wait for task to finish its current iteration and self-delete
    if (_producerTaskHandle) {
        vTaskDelay(pdMS_TO_TICKS(50));
        _producerTaskHandle = nullptr;
    }

    MIXER_LOG("Producer task stopped");
}

#endif // SFX_PLATFORM_ESP32


#endif // SFX_HAS_AUDIO
