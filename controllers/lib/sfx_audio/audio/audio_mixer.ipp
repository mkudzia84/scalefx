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
    // Free per-channel WAV decode buffers AND any preload caches.
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        sfxPsramFree(_channels[i].wav.bufL);
        sfxPsramFree(_channels[i].wav.bufR);
        _channels[i].wav.bufL = nullptr;
        _channels[i].wav.bufR = nullptr;
        freePreload(_channels[i].wav);
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
    // Every early-return path below MUST clear `_channelPlaying[channel]`
    // because the caller (playAsync) marked it TRUE eagerly.  Leaking
    // a stale TRUE leaves the channel apparently-playing forever in
    // status/visibility, but with ws.active=false the producer skips
    // it and produces no audio.  (Wedge bug fixed 2026-05-23.)
    if (!_initialized) {
        MIXER_ERROR("play() called but mixer not initialized");
        if (channel >= 0 && channel < AUDIO_MAX_CHANNELS) {
            _channelPlaying[channel].store(false, std::memory_order_release);
        }
        return false;
    }
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) {
        MIXER_ERROR("play() invalid channel: %d", channel);
        return false;   // can't clear out-of-range; playAsync guards too
    }
    if (!filename) {
        MIXER_ERROR("play() null filename on ch%d", channel);
        _channelPlaying[channel].store(false, std::memory_order_release);
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
    // Drop any previous preload — re-trigger of the same sample on the
    // same channel reuses the slot but reallocates so the new options
    // (volume, output mask, ...) take effect cleanly.  No-op if nothing
    // was preloaded.
    freePreload(ws);

    // Open the WAV file via SdCardModule singleton
    // SD mutex protects all file I/O — required when producer runs on
    // Core 1 while storage operations continue on Core 0.
    //
    // EVERY failure path here MUST clear `_channelPlaying[channel]`
    // because `playAsync()` eagerly marked it TRUE at queue time.
    // Without the clear, a wedged channel reports "playing" forever
    // while the producer skips it (ws.active=false) — observed in the
    // GunFx-shot-after-engine-loop wedge on 2026-05-23.
    SdCardModule& sd = SdCardModule::instance();
    if (!sd.isInitialized()) {
        MIXER_ERROR("Ch%d: SD card not initialized", channel);
        _channelPlaying[channel].store(false, std::memory_order_release);
        return false;
    }
    sd.lock();
    uint8_t sdErr = sd.openRead(filename, ws.file);
    if (sdErr != 0) {
        sd.unlock();
        MIXER_ERROR("Ch%d: Failed to open: %s (err=%d)", channel, filename, sdErr);
        _channelPlaying[channel].store(false, std::memory_order_release);
        return false;
    }

    // Parse WAV header (still under SD lock — reads from file)
    if (!parseWavHeader(ws)) {
        ws.file.close();
        sd.unlock();
        MIXER_ERROR("Ch%d: Invalid WAV: %s", channel, filename);
        _channelPlaying[channel].store(false, std::memory_order_release);
        return false;
    }
    sd.unlock();
    MIXER_LOG("Ch%d: header OK (%uHz/%uch/%ub, %u frames)", channel,
              (unsigned)ws.sampleRate_Hz, (unsigned)ws.numChannels,
              (unsigned)ws.bitsPerSample, (unsigned)ws.totalFrames);

    // Configure resampler
    ws.resampleRatio = (float)ws.sampleRate_Hz / (float)AUDIO_SAMPLE_RATE;
    ws.resampleFrac = 0.0f;
    ws.bufLen = 0;
    ws.bufPos = 0;
    ws.preloadPos = 0;

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
    ch.fadeStep = 0.0f;
    // Optional fade-in: ramp the channel 0 → full over `fadeInMs`.  Reuses
    // the fade machinery with a NEGATIVE step (fade-OUT uses a positive
    // step toward 0); the consumer clamps at 1.0 and clears `fading`.
    if (options.fadeInMs > 0) {
        ch.fading     = true;
        ch.fadeVolume = 0.0f;
        ch.fadeStep   = -(1.0f / (((float)options.fadeInMs * AUDIO_SAMPLE_RATE) / 1000.0f));
    }
    // Optional tail fade-out: arm an auto fade-out over the final `fadeOutMs`
    // of a one-shot.  Disabled for loops (a looping track has no "end").  The
    // trigger threshold is in SOURCE frames so it matches the
    // playback-remaining the consumer computes from `framesRead` + buffer.
    ch.fadeOutTriggerFrames = 0;
    ch.fadeOutStep          = 0.0f;
    if (options.fadeOutMs > 0 && !ws.loop) {
        ch.fadeOutTriggerFrames =
            ((uint32_t)options.fadeOutMs * ws.sampleRate_Hz) / 1000;
        ch.fadeOutStep = 1.0f / (((float)options.fadeOutMs * AUDIO_SAMPLE_RATE) / 1000.0f);
    }
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

    // ── Preload path ────────────────────────────────────────────────
    // Caller asked for the whole file to be cached in PSRAM.  On
    // success, file handle is closed inside tryPreload(), the streaming
    // pre-fill loop below becomes a no-op (refillWavBuffer short-circuits
    // on preloadActive), and we skip straight to ws.active=true.
    // On failure (alloc / oversize / partial read) the file is left open
    // so the streaming branch picks up exactly as before.
    if (options.preloadIntoMemory) {
        if (tryPreload(ws, filename)) {
            ws.active = true;
            _channelPlaying[channel] = true;
            float duration_s = (float)ws.totalFrames / (float)ws.sampleRate_Hz;
            MIXER_LOG("Ch%d: Playing %s (preloaded %s, vol=%.2f, %luHz %dbit %s, %.1fs%s)",
                     channel, filename,
                     ws.loopCount == LOOP_INFINITE ? "loop" :
                       (ws.loopCount > 0 ? "x-fixed" : "once"),
                     ch.volume, (unsigned long)ws.sampleRate_Hz,
                     ws.bitsPerSample,
                     ws.numChannels == 2 ? "stereo" : "mono",
                     (double)duration_s,
                     ws.resampleRatio != 1.0f ? " [resample]" : "");
            return true;
        }
        // Fallthrough — tryPreload() left the file open; streaming will
        // pick it up.  startOffset handling for the streaming branch is
        // below; preload always starts at frame 0 (no in-cache offset
        // implemented yet — easy follow-up if needed).
    }

    // Pre-fill WAV buffer — loop until either full OR refill makes no
    // progress (EOF for non-looping files SHORTER than the buffer).
    //
    // BUG fixed 2026-05-23: `refillWavBuffer` returns TRUE at EOF when
    // `bufLen > 0` (signal to producer: "still data to play"), which
    // an unconditional `if (!refillWavBuffer(ws)) break;` pre-fill loop
    // turns into an INFINITE LOOP for any file smaller than
    // WAV_BUF_FRAMES (~500 ms at 48 kHz).  The 200 ms gun-shot WAV at
    // ~9 600 frames triggered this — play() hung forever, channel was
    // left "playing" with bufLen flat-lined, producer skipped it, no
    // audio came out, and the audio mixer looked wedged.  Exit when
    // bufLen stops growing.
    while (ws.bufLen < WAV_BUF_FRAMES) {
        int prevLen = ws.bufLen;
        if (!refillWavBuffer(ws)) break;       // hard EOF/error
        if (ws.bufLen == prevLen) break;       // no progress — file fully consumed
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
            freePreload(ws);                    // release PSRAM cache if any
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
//  PRELOAD INTO MEMORY (AudioPlaybackOptions::preloadIntoMemory)
// ============================================================================
//
// Eliminates per-loop SD reads for short hot samples (sustained-fire gun
// shot, alert tone, burst stinger).  Cap is `WAV_MAX_PRELOAD_FRAMES`
// (96 000 source frames = 2 s @ 48 kHz stereo = 768 KB) — large enough
// to hold a typical 2A42 burst pattern and small enough that even four
// hot samples don't blow the PSRAM budget.
//
// Caller has ALREADY opened the file + parsed the header; we own the
// read-loop and the file handle close on success.  On failure (alloc /
// oversize / partial read) the file is LEFT OPEN — the caller's
// existing streaming-init path picks it up unchanged.

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::freePreload(WavState& ws) {
    if (ws.preloadL) { sfxPsramFree(ws.preloadL); ws.preloadL = nullptr; }
    if (ws.preloadR) { sfxPsramFree(ws.preloadR); ws.preloadR = nullptr; }
    ws.preloadFrames = 0;
    ws.preloadPos    = 0;
    ws.preloadActive = false;
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::tryPreload(WavState& ws, const char* filename) {
    if (ws.totalFrames == 0) return false;
    if (ws.totalFrames > WAV_MAX_PRELOAD_FRAMES) {
        MIXER_WARN("Preload: %s has %u frames > cap %u — falling back to streaming",
                   filename, (unsigned)ws.totalFrames, (unsigned)WAV_MAX_PRELOAD_FRAMES);
        return false;
    }

    // Always start from a clean slate — a prior preload on this channel
    // (re-trigger of the same sample, or switch between samples) must be
    // released BEFORE we allocate, or we'd leak the previous buffers.
    freePreload(ws);

    const uint32_t bytesPerBuf = ws.totalFrames * sizeof(float);
    const uint32_t needed      = bytesPerBuf * 2;  // L + R buffers

    // Live PSRAM budget check (2026-05-23): the hard cap above bounds a
    // single preload, but multiple hot samples (gunA + gunB + alert +
    // engine-stinger) can stack up.  Refuse the alloc if it would leave
    // PSRAM headroom below `kPreloadSafetyMarginBytes` — the SD read
    // buffer, ring, decode buffers and ConfigStore PSRAM allocs all
    // share this pool, and an OOM on those is fatal whereas a streaming
    // fallback for a sample is just a small SD cost.
    constexpr size_t kPreloadSafetyMarginBytes = 1 * 1024 * 1024;   // 1 MB
    const size_t psramFree = sfxPsramFree_bytes();
    if (psramFree > 0 && (size_t)needed + kPreloadSafetyMarginBytes > psramFree) {
        MIXER_WARN("Preload: would leave PSRAM under safety margin "
                   "(need=%u, free=%u, margin=%u) — streaming %s",
                   (unsigned)needed, (unsigned)psramFree,
                   (unsigned)kPreloadSafetyMarginBytes, filename);
        return false;
    }

    ws.preloadL = static_cast<float*>(sfxPsramMalloc(bytesPerBuf));
    ws.preloadR = static_cast<float*>(sfxPsramMalloc(bytesPerBuf));
    if (!ws.preloadL || !ws.preloadR) {
        MIXER_ERROR("Preload: PSRAM alloc failed (%u B each, free=%u) for %s — streaming",
                    (unsigned)bytesPerBuf,
                    (unsigned)sfxPsramFree_bytes(), filename);
        freePreload(ws);
        return false;
    }

    // Read the entire data chunk in WAV_SD_READ_BYTES batches.  The file
    // is already positioned at dataStart by parseWavHeader.  SD mutex
    // wraps the WHOLE read so storage ops on the other core don't seek
    // mid-load (each batch re-locks to give the storage subsystem a
    // chance between reads).
    const int     bytesPerFrame = ws.numChannels * (ws.bitsPerSample / 8);
    SdCardModule& sd            = SdCardModule::instance();
    uint32_t      framesDecoded = 0;

    while (framesDecoded < ws.totalFrames) {
        const uint32_t framesLeft  = ws.totalFrames - framesDecoded;
        const int      batchFrames =
            (framesLeft * (uint32_t)bytesPerFrame > (uint32_t)WAV_SD_READ_BYTES)
                ? (WAV_SD_READ_BYTES / bytesPerFrame)
                : (int)framesLeft;
        const int batchBytes = batchFrames * bytesPerFrame;

        sd.lock();
        int bytesRead = ws.file.read(_sdReadBuf, batchBytes);
        sd.unlock();
        if (bytesRead != batchBytes) {
            MIXER_ERROR("Preload: short read on %s (%d/%d B at frame %u) — streaming",
                        filename, bytesRead, batchBytes, (unsigned)framesDecoded);
            freePreload(ws);
            // Rewind the file handle so the streaming fallback in
            // play() reads from the start of the data chunk instead
            // of the mid-stream position we left it at (2026-05-24:
            // missing rewind meant a partial preload failure left
            // the streaming path reading garbage past the WAV data,
            // which manifested as silent gun audio).
            sd.lock();
            ws.file.seek(ws.dataStart);
            sd.unlock();
            ws.framesRead = 0;
            return false;
        }

        // Convert raw PCM → float, write straight into the preload bufs.
        for (int i = 0; i < batchFrames; ++i) {
            float l, r;
            if (ws.bitsPerSample == 16) {
                const int16_t* buf16 = (const int16_t*)_sdReadBuf;
                if (ws.numChannels == 2) {
                    l = buf16[i * 2]     * (1.0f / 32768.0f);
                    r = buf16[i * 2 + 1] * (1.0f / 32768.0f);
                } else {
                    l = r = buf16[i] * (1.0f / 32768.0f);
                }
            } else {     // 8-bit unsigned
                if (ws.numChannels == 2) {
                    l = (_sdReadBuf[i * 2]     - 128) * (1.0f / 128.0f);
                    r = (_sdReadBuf[i * 2 + 1] - 128) * (1.0f / 128.0f);
                } else {
                    l = r = (_sdReadBuf[i] - 128) * (1.0f / 128.0f);
                }
            }
            ws.preloadL[framesDecoded + i] = l;
            ws.preloadR[framesDecoded + i] = r;
        }
        framesDecoded += batchFrames;
    }

    // File handle no longer needed — close it so the SD subsystem can
    // reclaim the slot.  bufL/bufR (the streaming decode buffers) stay
    // allocated; they're per-channel + never touched on the preload path.
    sd.lock();
    ws.file.close();
    sd.unlock();

    ws.preloadFrames = ws.totalFrames;
    ws.preloadPos    = 0;
    ws.preloadActive = true;
    ws.framesRead    = 0;
    ws.bufLen        = 0;
    ws.bufPos        = 0;

    MIXER_LOG("Preload: cached %s (%u frames, %u KB, %.1f s @ %u Hz)",
              filename, (unsigned)ws.totalFrames,
              (unsigned)(bytesPerBuf * 2 / 1024),
              (double)ws.totalFrames / (double)ws.sampleRate_Hz,
              (unsigned)ws.sampleRate_Hz);
    return true;
}

// ============================================================================
//  WAV BUFFER REFILL (Producer side — Core 1 task on ESP32, Core 0 on Pico)
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::refillWavBuffer(WavState& ws) {
    // Preloaded sample → no SD I/O, no decode buffer, nothing to refill.
    // Returning `true` keeps the streaming-side callers (pre-fill loop in
    // play(), proactive top-up in produce()) happy without any work.
    if (ws.preloadActive) return true;

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

    int bytesPerFrame = ws.numChannels * (ws.bitsPerSample / 8);
    int writeCursor   = remaining;
    bool madeProgress = false;

    // Loop-wrap prefetch (2026-05-23): consume the file tail AND the
    // start of the next iteration in the SAME fill call when looping.
    // Old code returned after reading the tail (e.g. 100 frames) and
    // forced the next call to do the SD seek RIGHT AT the consumer
    // boundary, where SD contention is most visible.  Now the seek
    // happens transparently during a normal top-up — the proactive
    // refill in `produce()` triggers a refill when the buffer drops
    // below 50 %, giving ~250 ms of runway before the wrap reaches
    // the consumer.  No separate prefetch buffer needed; the SPSC
    // ring + WAV decode buffer already handle the hand-off.
    while (space > 0) {
        int framesLeft = (int)(ws.totalFrames - ws.framesRead);
        if (framesLeft <= 0) {
            if (!ws.loop) {
                // EOF on a one-shot — leave ws.active alone (the buffer
                // may still hold frames the consumer will drain) and let
                // produceFrame()'s getWavSample() path handle teardown.
                break;
            }
            // Looping: bump the loop counter (LOOP_INFINITE skips this),
            // seek back to dataStart, and continue the SAME fill pass.
            if (ws.loopCount != LOOP_INFINITE) {
                if (ws.loopCount <= 0) {
                    ws.loop = false;
                    break;
                }
                ws.loopCount--;
            }
            SdCardModule::instance().lock();
            ws.file.seek(ws.dataStart);
            SdCardModule::instance().unlock();
            ws.framesRead = 0;
            framesLeft = (int)ws.totalFrames;
            if (framesLeft <= 0) break;     // pathologically empty file
        }

        int framesToRead = (space < framesLeft) ? space : framesLeft;
        int bytesToRead  = framesToRead * bytesPerFrame;
        if (bytesToRead > WAV_SD_READ_BYTES) {
            bytesToRead  = WAV_SD_READ_BYTES;
            framesToRead = bytesToRead / bytesPerFrame;
        }

        SdCardModule::instance().lock();
        int bytesRead = ws.file.read(_sdReadBuf, bytesToRead);
        SdCardModule::instance().unlock();
        int framesGot = bytesRead / bytesPerFrame;
        if (framesGot <= 0) break;          // read failure — stop the pass
        ws.framesRead += framesGot;

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
            ws.bufL[writeCursor + i] = l;
            ws.bufR[writeCursor + i] = r;
        }
        writeCursor  += framesGot;
        space        -= framesGot;
        madeProgress  = true;
        // Cap per-call SD work: one batch already cost a lock + read; if
        // we filled enough to keep the consumer fed, hand back control
        // and let the next produce() iteration handle the next batch.
        if (bytesRead < bytesToRead) {
            // Short read: probably SD slow-path; defer remaining wrap
            // work to the next call so we don't starve other channels.
            break;
        }
    }

    ws.bufLen = writeCursor;
    if (madeProgress) return ws.bufLen > 0;
    // No progress this call → tell caller "no fresh data" so the
    // pre-fill loop in play() can break out (avoids spin on EOF).
    return ws.bufLen > 0 ? true : false;
}

// ============================================================================
//  WAV SAMPLE RETRIEVAL WITH RESAMPLING (Producer side)
// ============================================================================

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::getWavSample(WavState& ws, float& outL, float& outR) {
    // ── Preloaded sample (AudioPlaybackOptions::preloadIntoMemory) ──
    // The entire file is already in PSRAM as preloadL/R[preloadFrames].
    // For looping, we sample with a modulo wrap on every advance — no
    // SD, no seek, no per-loop hand-off.  For one-shot we return false
    // at the last frame, same teardown path as the streaming branch.
    if (ws.preloadActive) {
        const uint32_t total = ws.preloadFrames;
        if (total < 2) {
            outL = outR = 0.0f;
            return false;
        }
        const uint32_t idx  = ws.preloadPos;
        const float    frac = ws.resampleFrac;

        // Linear interpolation — neighbour wraps to 0 at end-of-buffer
        // when looping so the sample doesn't pop at the wrap boundary.
        // (For one-shot, the second branch below clamps the read but
        // exits at the same frame, so the +1 access on the last frame
        // never goes through this path.)
        uint32_t next = idx + 1;
        if (next >= total) next = ws.loop ? 0u : (total - 1);

        outL = ws.preloadL[idx] + (ws.preloadL[next] - ws.preloadL[idx]) * frac;
        outR = ws.preloadR[idx] + (ws.preloadR[next] - ws.preloadR[idx]) * frac;

        // Advance — modulo-wrap on the position itself (not just the
        // interpolation neighbour) so framesRead surfaced via the
        // remaining-sec status stays accurate.  For one-shot, hitting
        // the last frame returns true ONCE then false on the next call.
        ws.resampleFrac += ws.resampleRatio;
        uint32_t advance = (uint32_t)ws.resampleFrac;
        ws.resampleFrac -= (float)advance;
        uint32_t newPos = idx + advance;
        if (newPos >= total) {
            if (!ws.loop) {
                ws.preloadPos = total;        // mark exhausted for teardown
                return false;                 // signals end-of-playback
            }
            if (ws.loopCount != LOOP_INFINITE) {
                if (ws.loopCount <= 0) {
                    ws.loop       = false;
                    ws.preloadPos = total;
                    return false;
                }
                ws.loopCount--;
            }
            newPos %= total;                  // wrap inside the buffer
        }
        ws.preloadPos = newPos;
        // Keep framesRead in sync so produceFrame's remainingSec math
        // (which only matters for one-shot) reads a sensible value.
        ws.framesRead = newPos;
        return true;
    }

    // ── Streaming path (existing behaviour) ──────────────────────────
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
            // Release preload PSRAM buffers (no-op when streaming).  We
            // could keep them around for a future loop, but the option
            // is per-play() so the next playAsync() will request preload
            // again if it wants the cache.
            freePreload(ws);
            checkAndPlayNextQueued(i);
            continue;
        }

        // Arm the auto tail fade-out once playback-remaining (file frames
        // not yet read + frames still buffered) drops below the threshold.
        // Reuses the fade-OUT machinery (positive step → ramps to 0 and stops
        // the channel).  One-shot: cleared as soon as it fires.
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            uint32_t framesLeft = ws.totalFrames - ws.framesRead + (ws.bufLen - ws.bufPos);
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Apply fade if active.  fadeStep > 0 = fade-OUT (ramp to 0, then
        // stop the channel); fadeStep < 0 = fade-IN (ramp to full, then
        // keep playing).
        float effectiveVolume = ch.volume;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                // Fade-out complete → stop the channel.
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                ws.active = false;
                _channelPlaying[i] = false;
                if (ws.file) {
                    SdCardModule::instance().lock();
                    ws.file.close();
                    SdCardModule::instance().unlock();
                }
                freePreload(ws);                // release PSRAM cache if any
                checkAndPlayNextQueued(i);
                continue;
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                // Fade-in complete → hold at full volume, keep playing.
                ch.fadeVolume = 1.0f;
                ch.fading = false;
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
        // Ring empty — write a batch of silence so BCLK / LRCK keep
        // toggling. Without this the I²S DMA stalls between audio events
        // and downstream codecs (TAS5825M et al.) drop their PLL lock and
        // fall back to HIZ. The blocking i2s_channel_write paces this loop
        // at the audio sample rate, so no extra delay is needed.
        static StereoFrame silenceFrames[256] = {};
        TI2S::instance().writeSamples(silenceFrames, 256);
        _underruns.fetch_add(1, std::memory_order_relaxed);
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
    // Mark the channel busy at queue time so AUDIO_STATUS_RESP reflects
    // the caller's intent immediately — without this there's a ~20 ms
    // race (file open + WAV-header parse + buffer pre-fill) during which
    // the producer hasn't reached the `_channelPlaying = true` line in
    // play() yet.  Every failure path in play() reverts to `false`;
    // stop/fade also clear it through the normal cleanup paths.
    if (channel >= 0 && channel < AUDIO_MAX_CHANNELS) {
        _channelPlaying[channel].store(true, std::memory_order_release);
    }
    // Queue-full failure: revert the eager mark so the channel
    // doesn't stay falsely "playing" while no command is in flight.
    // (Wedge bug fixed 2026-05-23 — was leaking the flag here too.)
    if (!queueCommand(cmd)) {
        if (channel >= 0 && channel < AUDIO_MAX_CHANNELS) {
            _channelPlaying[channel].store(false, std::memory_order_release);
        }
        return false;
    }
    return true;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopAsync(int channel, AudioStopMode mode) {
    Command cmd{};
    cmd.type = (channel < 0) ? CommandType::StopAll : CommandType::Stop;
    cmd.channelId = channel;
    cmd.stopMode = mode;
    // Mirror the eager-state convention from playAsync: clear the
    // channel-playing flag at queue time so AUDIO_STATUS_RESP reflects
    // the caller's intent.  Fade mode still produces audio for a few
    // hundred ms while the level ramps down — we drop the active flag
    // immediately so a fresh play() restart works without state lag.
    if (channel < 0) {
        for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
            _channelPlaying[i].store(false, std::memory_order_release);
        }
    } else if (channel < AUDIO_MAX_CHANNELS) {
        _channelPlaying[channel].store(false, std::memory_order_release);
    }
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

    // Store config for resumeAudio()
    _producerCore = core;
    _producerPriority = priority;
    _producerStackSize = stackSize;

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

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::suspendAudio() {
    MIXER_LOG("Suspending audio (consumer + producer)...");

    // 1. Stop all playback immediately
    stopAll(AudioStopMode::Immediate);

    // 2. Stop producer task (WAV decode + mixing on Core 1)
    stopProducerTask();

    // 3. Suspend consumer task (I2S output on Core 1)
    if (_consumerTaskHandle) {
        vTaskSuspend(_consumerTaskHandle);
    }

    MIXER_LOG("Audio suspended — Core 1 freed");
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::resumeAudio() {
    MIXER_LOG("Resuming audio (consumer + producer)...");

    // 1. Resume consumer task first (higher priority)
    if (_consumerTaskHandle) {
        vTaskResume(_consumerTaskHandle);
    }

    // 2. Restart producer task with stored configuration
    startProducerTask(_producerCore, _producerPriority, _producerStackSize);

    MIXER_LOG("Audio resumed — consumer + producer restarted");
}

#endif // SFX_PLATFORM_ESP32


#endif // SFX_HAS_AUDIO
