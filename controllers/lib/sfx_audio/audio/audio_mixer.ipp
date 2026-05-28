/**
 * Audio Mixer - Implementation
 * 
 * SPSC Ring Buffer Architecture (ESP32-S3):
 *   Core 1 — Producer Task: WAV decode + SD reads + int16 Q15 mixing → ring buffer
 *   Core 1 — Consumer Task: ring buffer → I2S DMA (higher priority)
 *   Core 0: Protocol handling only — uses *Async() API to queue commands
 *
 * Mix kernel runs in int16 / Q15 fixed-point (Phase 5,
 * feature/mixer-int16-kernel — the source PCM is already int16 native,
 * and the per-frame hot loop dominated producer cost at 2+ active
 * channels under the legacy float kernel).  Sources still emit float
 * via IAudioSource::readFrames(); refillDrainBuffer converts to int16
 * once at fill time so the hot path is pure integer.
 * File I/O uses SdCardModule singleton directly.
 *
 * The controller instantiates AudioMixer<ConcreteI2S, ConcreteCodec>.
 * I2S output and codec are accessed as singletons via TI2S::instance()
 * and TCodec::instance(). No platform #ifdef blocks in this file.
 */

#if defined(SFX_HAS_AUDIO)

#include "audio_log.h"

#if SFX_PLATFORM_ESP32
#include <esp_heap_caps.h>     // heap_caps_malloc / MALLOC_CAP_DMA / MALLOC_CAP_INTERNAL
// esp-dsp SIMD kernels (Phase 5 of feature/idf-component-build).
// Header guard via __has_include in case esp-dsp isn't pulled into
// the build (e.g. an older platformio.ini without REQUIRES).
#if defined(__has_include)
#  if __has_include(<dsps_mulc.h>)
#    include <dsps_mulc.h>
#    define SFX_HAS_ESP_DSP 1
#  endif
#endif
#endif
#ifndef SFX_HAS_ESP_DSP
#  define SFX_HAS_ESP_DSP 0
#endif

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

    // ---- Allocate SD read buffer from DMA-cap internal SRAM ----
    //
    // Re-applying the direct-DMA-to-internal-SRAM path (originally built
    // #345 2026-05-27, then rolled back at #353 after 2-track stutter).
    // Per the user's reassessment, the stutter was most likely an SD
    // card issue (random-read penalty / fragmentation / card transient),
    // not the audio code path — so retry the fast path with the same
    // 32 KB block.  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL gives a
    // DMA-coherent allocation; the SDMMC peripheral DMAs straight into
    // it, no PSRAM bounce-buffer copy, lifting raw read throughput from
    // ~1 MB/s → ~14 MB/s.
    //
    // If stutter recurs, the PSRAM fallback (below, fires when DMA-cap
    // alloc fails) is the known-good ~1 MB/s path — flag goes through
    // the WARN log so the regression is visible in the boot trace.
    // Per-channel decode buffer (0.5 s headroom) absorbs the slow path
    // without underrunning, so single-track always plays.
    if (!_sdReadBuf) {
#if SFX_PLATFORM_ESP32
        _sdReadBuf = static_cast<uint8_t*>(
            heap_caps_malloc(WAV_SD_READ_BYTES,
                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (_sdReadBuf) {
            MIXER_LOG("SD scratch: %d KB in DMA-cap SRAM (direct DMA; ~14 MB/s ceiling)",
                      WAV_SD_READ_BYTES / 1024);
        } else {
            MIXER_WARN("SD scratch: DMA-cap SRAM exhausted (%u KB) — falling back to PSRAM (~1 MB/s)",
                       (unsigned)(WAV_SD_READ_BYTES / 1024));
            _sdReadBuf = static_cast<uint8_t*>(sfxPsramMalloc(WAV_SD_READ_BYTES));
        }
#else
        _sdReadBuf = static_cast<uint8_t*>(sfxPsramMalloc(WAV_SD_READ_BYTES));
#endif
        if (!_sdReadBuf) {
            MIXER_ERROR("SD read buffer allocation failed (%d bytes)", WAV_SD_READ_BYTES);
            return false;
        }
    }

    // ---- Allocate per-channel WAV decode buffers + queue from PSRAM ----
    // Per-channel `queue[QUEUE_SIZE_PER_CHANNEL]` lives in PSRAM (re-applied
    // 2026-05-27 after the previous re-apply was rolled back on a stutter
    // suspicion that turned out to be SD-card-side, not the queue).  Each
    // slot ~150 B; 4 × 8 ch = ~5 KB the singleton no longer carries in BSS.
    // Touched only on protocol-rate enqueue / dequeue — PSRAM latency
    // invisible at that cadence.  Failure rollback mirrors wav.bufL/R.
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i] = Channel{};
        _channelPlaying[i] = false;
        _channelRemainingSec[i] = 0.0f;

        // Allocate L+R int16 drain buffers for WAV decode + per-channel queue.
        // int16 (not float — Phase 5 mixer-int16-kernel) halves the PSRAM
        // footprint from 1.5 MB to 768 KB across 8 channels and lets the
        // hot mix loop run pure integer.
        _channels[i].wav.bufL = static_cast<int16_t*>(
            sfxPsramCalloc(WAV_BUF_FRAMES, sizeof(int16_t)));
        _channels[i].wav.bufR = static_cast<int16_t*>(
            sfxPsramCalloc(WAV_BUF_FRAMES, sizeof(int16_t)));
        _channels[i].queue = static_cast<QueuedSound*>(
            sfxPsramCalloc(QUEUE_SIZE_PER_CHANNEL, sizeof(QueuedSound)));

        if (!_channels[i].wav.bufL || !_channels[i].wav.bufR || !_channels[i].queue) {
            MIXER_ERROR("WAV/queue alloc failed for ch %d", i);
            // Clean up already-allocated buffers
            for (int j = 0; j <= i; j++) {
                sfxPsramFree(_channels[j].wav.bufL);
                sfxPsramFree(_channels[j].wav.bufR);
                sfxPsramFree(_channels[j].queue);
                _channels[j].wav.bufL = nullptr;
                _channels[j].wav.bufR = nullptr;
                _channels[j].queue    = nullptr;
            }
            sfxPsramFree(_sdReadBuf);
            _sdReadBuf = nullptr;
            return false;
        }
    }

    // ---- Allocate mixer-wide command queue from PSRAM ----
    // 16 slots × ~150 B = ~2.4 KB.  Mutex-protected control surface
    // (Core 0 enqueues, producer drains); not on the audio frame path.
    if (!_cmdQueue) {
        _cmdQueue = static_cast<Command*>(
            sfxPsramCalloc(AUDIO_CMD_QUEUE_SIZE, sizeof(Command)));
        if (!_cmdQueue) {
            MIXER_ERROR("Command queue alloc failed (%d slots × %u B)",
                        AUDIO_CMD_QUEUE_SIZE, (unsigned)sizeof(Command));
            // Roll back per-channel buffers + SD scratch
            for (int j = 0; j < AUDIO_MAX_CHANNELS; j++) {
                sfxPsramFree(_channels[j].wav.bufL);
                sfxPsramFree(_channels[j].wav.bufR);
                sfxPsramFree(_channels[j].queue);
                _channels[j].wav.bufL = nullptr;
                _channels[j].wav.bufR = nullptr;
                _channels[j].queue    = nullptr;
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

    // Calculate total PSRAM consumed by audio subsystem.
    int wavBufTotal_KB = (AUDIO_MAX_CHANNELS * 2 * WAV_BUF_FRAMES * sizeof(int16_t)) / 1024;
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

    // Stop all playback (each stop() tears down the source via
    // destroyAudioSource, which closes its file or frees preload PSRAM).
    stopAll(AudioStopMode::Immediate);

    // Stop I2S — set flag BEFORE calling end() so Core 1's consume()
    // sees the flag and stops writing to i2sOutput during teardown
    if (_i2sRunning.load(std::memory_order_acquire)) {
        _i2sRunning.store(false, std::memory_order_release);
        SFX_DELAY_MS(2);  // Let Core 1 drain current consumeAndOutput() iteration

        TI2S::instance().end();
    }

    _initialized.store(false, std::memory_order_release);

    // ---- Free PSRAM audio buffers ----
    // Per-channel scratch buffers (the source instances were destroyed
    // in stopAll() above, releasing their files / preload PSRAM) + the
    // per-channel queue (PSRAM-resident, see begin() comment).
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        sfxPsramFree(_channels[i].wav.bufL);
        sfxPsramFree(_channels[i].wav.bufR);
        sfxPsramFree(_channels[i].queue);
        _channels[i].wav.bufL = nullptr;
        _channels[i].wav.bufR = nullptr;
        _channels[i].queue    = nullptr;
    }

    // Free shared SD read buffer (DMA-cap SRAM or PSRAM fallback — sfxPsramFree
    // wraps heap_caps_free which accepts allocations from any capability pool).
    sfxPsramFree(_sdReadBuf);
    _sdReadBuf = nullptr;

    // Free mixer-wide command queue (PSRAM).
    sfxPsramFree(_cmdQueue);
    _cmdQueue = nullptr;

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
    // because the caller (playAsync) marked it TRUE eagerly.
    if (!_initialized) {
        MIXER_ERROR("play() called but mixer not initialized");
        if (channel >= 0 && channel < AUDIO_MAX_CHANNELS) {
            _channelPlaying[channel].store(false, std::memory_order_release);
        }
        return false;
    }
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) {
        MIXER_ERROR("play() invalid channel: %d", channel);
        return false;
    }
    if (!filename) {
        MIXER_ERROR("play() null filename on ch%d", channel);
        _channelPlaying[channel].store(false, std::memory_order_release);
        return false;
    }

    Channel& ch = _channels[channel];
    WavState& ws = ch.wav;

    // Tear down any source already attached.  Source destructor closes
    // its file or frees its preload PSRAM uniformly across kinds.
    destroyAudioSource(ws.source);
    ws.active        = false;
    ws.bufLen        = 0;
    ws.bufPos        = 0;
    ws.resampleFrac  = 0.0f;

    // ── Construct a concrete source ─────────────────────────────────
    // Preload first: short hot samples (gun, alert) get cached in
    // PSRAM so loops never touch SD.  openAndPreload() returns false
    // for oversize files / OOM headroom — caller falls through to
    // streaming.  Stage 4 will add WavPagedSource as a middle tier.
    if (options.preloadIntoMemory) {
        auto* preload = new (ch.sourceStorage) WavPreloadSource();
        if (preload->openAndPreload(filename, _sdReadBuf,
                                    WAV_SD_READ_BYTES,
                                    WAV_MAX_PRELOAD_FRAMES)) {
            ws.source = preload;
        } else {
            preload->~WavPreloadSource();
            // fall through to streaming
        }
    }
    if (!ws.source) {
#if SFX_PLATFORM_ESP32
        // PSRAM-resident source via the AudioAssetCache (May 2026).
        // The cache loads the file in the background; this source
        // reads directly from PSRAM during playback so I²S↔SDMMC
        // contention never affects the audio stream.  No fallback to
        // SD streaming: an asset that doesn't fit (raw WAV > 6 MB
        // budget) is refused at the source level so the operator
        // re-encodes it to MP3 (12× smaller).
        //
        // Dispatch by extension:
        //   *.wav → WavPsramSource (raw PCM, no decode cost)
        //   *.mp3 → Mp3PsramSource (helix decode from PSRAM bytes,
        //                            uses Mp3DecoderPool)
        const size_t nameLen = strlen(filename);
        const bool isMp3 = (nameLen >= 4) &&
            (filename[nameLen-4] == '.') &&
            ((filename[nameLen-3] == 'm') || (filename[nameLen-3] == 'M')) &&
            ((filename[nameLen-2] == 'p') || (filename[nameLen-2] == 'P')) &&
            (filename[nameLen-1] == '3');

        if (isMp3) {
            auto* mp3 = new (ch.sourceStorage) Mp3PsramSource();
            if (mp3->open(filename)) {
                ws.source = mp3;
            } else {
                mp3->~Mp3PsramSource();
                MIXER_ERROR("Ch%d: MP3 open failed: %s", channel, filename);
                _channelPlaying[channel].store(false, std::memory_order_release);
                return false;
            }
        } else {
            auto* psram = new (ch.sourceStorage) WavPsramSource();
            if (psram->open(filename)) {
                ws.source = psram;
            } else {
                psram->~WavPsramSource();
                MIXER_ERROR("Ch%d: cannot play %s (oversized for PSRAM cache "
                            "or load failed) — encode as MP3 to play larger files",
                            channel, filename);
                _channelPlaying[channel].store(false, std::memory_order_release);
                return false;
            }
        }
#endif
    }
    if (!ws.source) {
        _channelPlaying[channel].store(false, std::memory_order_release);
        return false;
    }

    // Cache format for fast status reads (immutable for this source).
    ws.sampleRate_Hz = ws.source->sampleRate_Hz();
    ws.numChannels   = ws.source->numChannels();
    ws.bitsPerSample = ws.source->bitsPerSample();
    ws.totalFrames   = ws.source->totalFrames();

    MIXER_LOG("Ch%d: header OK (%uHz/%uch/%ub, %u frames)", channel,
              (unsigned)ws.sampleRate_Hz, (unsigned)ws.numChannels,
              (unsigned)ws.bitsPerSample, (unsigned)ws.totalFrames);

    // Configure resampler
    ws.resampleRatio = (float)ws.sampleRate_Hz / (float)AUDIO_SAMPLE_RATE;
    ws.resampleFrac  = 0.0f;
    ws.bufLen        = 0;
    ws.bufPos        = 0;

    // Apply loop options — source owns counter decay; we just cache
    // the initial value for getInitialLoopCount() status reads.
    bool srcLoop;
    int  srcLoopCount;
    if (options.loop && options.loopCount == LOOP_INFINITE) {
        srcLoop = true;   srcLoopCount = LOOP_INFINITE;
    } else if (options.loopCount > 0) {
        srcLoop = true;   srcLoopCount = options.loopCount;
    } else if (options.loopCount == 0 || !options.loop) {
        srcLoop = false;  srcLoopCount = 0;
    } else {
        srcLoop = true;   srcLoopCount = LOOP_INFINITE;
    }
    ws.source->setLoop(srcLoop);
    ws.source->setLoopCount(srcLoopCount);
    ws.loopCountInit = srcLoopCount;

    ch.volume = constrain(options.volume, 0.0f, 1.0f);
    ch.outputChannels = options.outputChannels;
    ch.fading = false;
    ch.fadeVolume = 1.0f;
    ch.fadeStep = 0.0f;
    // Optional fade-in
    if (options.fadeInMs > 0) {
        ch.fading     = true;
        ch.fadeVolume = 0.0f;
        ch.fadeStep   = -(1.0f / (((float)options.fadeInMs * AUDIO_SAMPLE_RATE) / 1000.0f));
    }
    // Optional tail fade-out (one-shot only)
    ch.fadeOutTriggerFrames = 0;
    ch.fadeOutStep          = 0.0f;
    if (options.fadeOutMs > 0 && !srcLoop) {
        ch.fadeOutTriggerFrames =
            ((uint32_t)options.fadeOutMs * ws.sampleRate_Hz) / 1000;
        ch.fadeOutStep = 1.0f / (((float)options.fadeOutMs * AUDIO_SAMPLE_RATE) / 1000.0f);
    }
    ch.mute = false;
    ch.pan = 0.0f;
    updatePan(ch);

    // Handle start offset (seek inside the source)
    if (options.startOffsetMs > 0) {
        uint32_t offsetFrames =
            (static_cast<uint32_t>(options.startOffsetMs) * ws.sampleRate_Hz) / 1000;
        if (offsetFrames < ws.totalFrames) {
            ws.source->seekFrame(offsetFrames);
        }
    }

    strncpy(ch.filename, filename, CHANNEL_FILENAME_MAX - 1);
    ch.filename[CHANNEL_FILENAME_MAX - 1] = '\0';

    // Pre-fill drain buffer — loop until either full OR refill makes
    // no progress (EOF for non-looping files SHORTER than buffer).
    while (ws.bufLen < WAV_BUF_FRAMES) {
        int prevLen = ws.bufLen;
        if (!refillDrainBuffer(ch)) break;
        if (ws.bufLen == prevLen) break;
    }
    MIXER_LOG("Ch%d: Pre-filled WAV buffer %d/%d frames (%.0f ms)",
             channel, ws.bufLen, WAV_BUF_FRAMES,
             (float)ws.bufLen / AUDIO_SAMPLE_RATE * 1000.0f);

    ws.active = true;
    _channelPlaying[channel] = true;

    const char* loopStr;
    char loopBuf[16];
    if (srcLoopCount == LOOP_INFINITE) {
        loopStr = "loop";
    } else if (srcLoopCount > 0) {
        snprintf(loopBuf, sizeof(loopBuf), "x%d", srcLoopCount);
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
    auto* src = _channels[channel].wav.source;
    return src ? src->loopCount() != 0 : false;
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    auto* src = _channels[channel].wav.source;
    return src ? src->loopCount() : 0;
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
    stopWithFadeMs(channel, mode, 0);
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopWithFadeMs(int channel, AudioStopMode mode,
                                              uint16_t fadeMs) {
    if (!_initialized) return;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;

    Channel& ch = _channels[channel];
    WavState& ws = ch.wav;
    if (!ws.active) return;

    switch (mode) {
        case AudioStopMode::Immediate:
            // Source destructor closes file / frees preload PSRAM.
            destroyAudioSource(ws.source);
            ws.active = false;
            ws.bufLen = 0;
            ws.bufPos = 0;
            _channelPlaying[channel] = false;
            MIXER_LOG("Ch%d: Stopped", channel);
            break;

        case AudioStopMode::Fade:
            ch.fading = true;
            ch.fadeVolume = 1.0f;
            // Custom fade ramp — fadeMs > 0 means the EngineFx cross-
            // fader (or another caller) supplied an explicit duration.
            // Falls back to the project-default FADE_DURATION_MS (50 ms)
            // for legacy `AudioStopMode::Fade` callers.
            if (fadeMs > 0) {
                ch.fadeStep = 1.0f /
                    (((float)fadeMs * AUDIO_SAMPLE_RATE) / 1000.0f);
                MIXER_LOG("Ch%d: Fading out over %u ms", channel, (unsigned)fadeMs);
            } else {
                ch.fadeStep = FADE_STEP_PER_FRAME;
                MIXER_LOG("Ch%d: Fading out (default %d ms)", channel, FADE_DURATION_MS);
            }
            break;

        case AudioStopMode::LoopEnd:
            if (ws.source) ws.source->setLoop(false);
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
    auto* src = _channels[channel].wav.source;
    if (src) {
        src->setLoop(false);
        src->setLoopCount(0);
    }
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopLoopingAll() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        auto* src = _channels[i].wav.source;
        if (src) {
            src->setLoop(false);
            src->setLoopCount(0);
        }
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
    if (ws.source && ws.source->loopCount() == LOOP_INFINITE) {
        if (loopBehavior == QueueLoopBehavior::StopImmediate) {
            stop(channel, AudioStopMode::Immediate);
            return play(channel, filename, options);
        } else {
            // FinishLoop: set loopCount=0 so the source returns 0 at
            // its current iteration's EOF; produceFrame() tears it
            // down and the queue plays the next item.
            ws.source->setLoopCount(0);
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
    auto* src = _channels[channel].wav.source;
    if (src && src->loopCount() != 0) return -1.0f;
    return _channelRemainingSec[channel];
}

// (parseWavHeader moved to audio_source.cpp as parseWavHeaderLocked —
// shared between WavStreamSource + WavPreloadSource.)

// ============================================================================
//  DRAIN BUFFER REFILL  +  RESAMPLED SAMPLE GET (Producer side)
// ============================================================================
//
// File I/O lives entirely in IAudioSource now (audio_source.h).  These
// methods are pure mixer-side scratch operations on `ws.bufL/R`.
//
// `refillDrainBuffer(ch)` pulls source-rate frames from the channel's
// IAudioSource into the per-channel float buffer.  Loop wrap, PCM
// decode, preload-vs-stream dispatch — all of that is inside the
// source.  This function is purely a "compact + top-up" cursor op.
//
// `getWavSample(ch, L, R)` linearly interpolates the next output-rate
// stereo sample from the drain buffer.  Same algorithm regardless of
// source kind — the source's readFrames() provides the source-rate
// data, this function adapts to the I²S sample rate.

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::refillDrainBuffer(Channel& ch) {
    WavState& ws = ch.wav;
    if (!ws.source) return false;

    // Move remaining frames to front of buffer
    int remaining = ws.bufLen - ws.bufPos;
    if (remaining > 0 && ws.bufPos > 0) {
        memmove(ws.bufL, ws.bufL + ws.bufPos, remaining * sizeof(int16_t));
        memmove(ws.bufR, ws.bufR + ws.bufPos, remaining * sizeof(int16_t));
    }
    ws.bufPos = 0;
    ws.bufLen = remaining;

    int space = WAV_BUF_FRAMES - remaining;
    if (space <= 0) return true;

    // ── Per-call decode-cost cap ────────────────────────────────────
    // Two competing constraints:
    //   1. Refill must add ≥ what produce() drained per cycle or the
    //      drain shrinks until silence is mixed in (per-channel
    //      "starves" in the pace log).
    //   2. Refill blocks the producer task; if it's too long, the ring
    //      drains and underruns (under+ ticks in the pace log).
    // Sweet spot is "refill more than one cycle's worth at a time so
    // refill fires every-other-cycle instead of every cycle" — halves
    // the average refill overhead vs the 4096-cap (cap == consume).
    // With 8192 frames an MP3 refill is ~35-40 ms; the ring's ~85 ms
    // headroom still absorbs that for two active MP3 channels.  PCM
    // (.wav) sources refill instantly regardless.  The drain buffer
    // is 24 K frames so 8 K never overruns.  2026-05-28 trace at
    // @213077ms showed 4096-cap producer falling behind in the
    // engine+gun case; 8192-cap restores the headroom.
    constexpr int kMaxRefillFrames = 8192;
    if (space > kMaxRefillFrames) space = kMaxRefillFrames;

    // Pull from source into a small float scratch in 256-frame batches,
    // converting each batch to int16 Q15 into the drain buffer's tail.
    // The source interface still emits float in [-1, +1]; the int16
    // store is the boundary that makes the hot mix path integer.  Time
    // the WHOLE refill (incl. conversion) so the SD-stall telemetry
    // still reports end-to-end latency.
    //
    // Stack scratch budget: 256 × 2 × 4 B = 2 KB; producer task stack
    // is 8 KB so the headroom is comfortable.  Batch size trades
    // per-call source overhead vs stack pressure — 256 is a few
    // batches per typical refill window.
    constexpr int kRefillBatchFrames = 256;
    float scratchL[kRefillBatchFrames];
    float scratchR[kRefillBatchFrames];

    const uint32_t t0 = micros();
    uint32_t got = 0;
    while ((int)got < space) {
        const int batchCap =
            (space - (int)got < kRefillBatchFrames) ? (space - (int)got)
                                                    : kRefillBatchFrames;
        const uint32_t n = ws.source->readFrames(scratchL, scratchR,
                                                 (uint32_t)batchCap);
        if (n == 0) break;
        int16_t* dstL = ws.bufL + ws.bufLen + (int)got;
        int16_t* dstR = ws.bufR + ws.bufLen + (int)got;
        for (uint32_t k = 0; k < n; ++k) {
            // Clamp + scale.  Source emits [-1, +1]; round-toward-zero
            // truncation is fine here, this conversion is happening at
            // refill cadence (batched), not per output sample.  Use
            // 32767 not 32768 to avoid asymmetric saturation on +1.
            float fL = scratchL[k];
            float fR = scratchR[k];
            if (fL >  1.0f) fL =  1.0f; else if (fL < -1.0f) fL = -1.0f;
            if (fR >  1.0f) fR =  1.0f; else if (fR < -1.0f) fR = -1.0f;
            dstL[k] = (int16_t)(fL * 32767.0f);
            dstR[k] = (int16_t)(fR * 32767.0f);
        }
        got += n;
        if ((int)n < batchCap) break;   // short read → source paused / EOF
    }
    const uint32_t dtUs = micros() - t0;

    // Track max-since-last-log via lock-free CAS (relaxed — the
    // periodic logger races safely; missing one tick is OK).
    uint32_t prevMax = _maxSdReadUs.load(std::memory_order_relaxed);
    while (dtUs > prevMax &&
           !_maxSdReadUs.compare_exchange_weak(prevMax, dtUs,
                                               std::memory_order_relaxed)) {}
    if (dtUs > 5000u)  _slowSdReads.fetch_add(1, std::memory_order_relaxed);
    if (dtUs > 20000u) _verySlowSdReads.fetch_add(1, std::memory_order_relaxed);

    ws.bufLen += (int)got;
    // Return TRUE if we got data OR the source isn't permanently
    // exhausted yet.  This distinguishes "real EOF" (source.isExhausted
    // → destroy channel) from "transient buffering" (paged source
    // waiting for the next page — drain what's already buffered, retry
    // soon).  Before this fix, a single page-transition with the next
    // page not-yet-ready would cause readFrames=0 → refill returns
    // false → channel destroyed mid-track (the engine_start.wav
    // truncation @ ~6.5 s on the build #370 test).
    return ws.bufLen > 0 || !ws.source->isExhausted();
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::getWavSample(Channel& ch, int16_t& outL, int16_t& outR) {
    WavState& ws = ch.wav;
    // Ensure 2 frames are available for linear interpolation; emit
    // silence if not (the produce() loop's per-cycle top-up will refill
    // the drain on the next pass).  The starve counter surfaces drain
    // exhaustion in the per-second pace telemetry.
    if (ws.bufPos + 1 >= ws.bufLen && ws.active) {
        const intptr_t idx = &ch - _channels;
        if (idx >= 0 && idx < AUDIO_MAX_CHANNELS) {
            _channelStarves[idx].fetch_add(1, std::memory_order_relaxed);
        }
        if (ws.source && ws.source->isExhausted()) {
            outL = outR = 0;
            return false;   // mixer destroys channel
        }
        outL = outR = 0;
        return true;        // transient drain; keep channel alive
    }

    const int idx = ws.bufPos;
    // Linear interpolation in Q15 fixed-point:
    //   result16 = a + ((b - a) * fracQ15) >> 15
    // resampleFrac stays float (≤ 1 mul per sample) — keeping it as a
    // float-then-Q15-convert avoids a second integer fixed-point path
    // for the resampler accumulator while the per-sample mix work is
    // already integer.  When most files play at 48 kHz native, frac is
    // always 0 and the interpolation collapses to a load.
    const int32_t fracQ15 = (int32_t)(ws.resampleFrac * 32768.0f);
    const int32_t aL = ws.bufL[idx];
    const int32_t bL = ws.bufL[idx + 1];
    const int32_t aR = ws.bufR[idx];
    const int32_t bR = ws.bufR[idx + 1];
    outL = (int16_t)(aL + (((bL - aL) * fracQ15) >> 15));
    outR = (int16_t)(aR + (((bR - aR) * fracQ15) >> 15));

    // Advance resampler — unchanged, float frac → int advance.
    ws.resampleFrac += ws.resampleRatio;
    int advance = (int)ws.resampleFrac;
    ws.bufPos       += advance;
    ws.resampleFrac -= (float)advance;
    return true;
}


// ============================================================================
//  Q15 MIXING PIPELINE — PRODUCER (Core 1 task on ESP32, Core 0 on Pico)
// ============================================================================
//
// All hot-path math is int16/int32 (Phase 5, feature/mixer-int16-kernel).
// Per-channel sample × volume × pan runs as `(int32)sample × Q15_gain >> 15`
// keeping each per-channel term within int16 range; accumulation across
// channels uses int32 (headroom for 8 ch × ±32767 = ±262144 << INT32_MAX).
// Master volume + output headroom apply once at frame end.  Sources stay
// float at the interface but the drain buffer is int16 (refillDrainBuffer
// quantizes at fill time); this branch's goal is to validate the int16
// hot-loop hypothesis before touching IAudioSource itself.

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::produceFrame() {
    if (ringBuf().isFull()) return false;

    int32_t mixL = 0;
    int32_t mixR = 0;
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;

        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        int16_t trackL16 = 0;
        int16_t trackR16 = 0;

        // Get one resampled int16 stereo frame from the drain buffer.
        if (!getWavSample(ch, trackL16, trackR16)) {
            destroyAudioSource(ws.source);
            ws.active = false;
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            checkAndPlayNextQueued(i);
            continue;
        }

        // Arm the auto tail fade-out once playback-remaining drops below
        // threshold (same logic as the float kernel — float scalar work
        // ≤ 1 op per channel per frame, not worth fixed-point-ifying).
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t framesLeft =
                ws.totalFrames - srcRead + (uint32_t)(ws.bufLen - ws.bufPos);
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Fade ramp update — float arithmetic, one mul/add/cmp per
        // channel per frame.  Cheaper than maintaining a Q15 ramp +
        // converting back.  Combined effectiveVolume converted to Q15
        // ONCE for the per-sample multiply below.
        float effectiveVolume = ch.volume;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                destroyAudioSource(ws.source);
                ws.active = false;
                _channelPlaying[i] = false;
                checkAndPlayNextQueued(i);
                continue;
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                ch.fadeVolume = 1.0f;
                ch.fading = false;
            }
        }

        hasAudio = true;

        // ── Q15 per-channel kernel ───────────────────────────────────
        // sample(int16) × volQ15(int32 in [0, 32768]) >> 15 keeps the
        // value within int16 range when vol ≤ 1.  Pan multiplies fold
        // into the same shape (panL/R also Q15 ≤ 32768).  When pan +
        // routing collapses to "ALL with vol only" the cost is one
        // multiply per channel; with pan it's two.
        const int32_t volQ15 = (int32_t)(effectiveVolume * 32768.0f);
        int32_t L = ((int32_t)trackL16 * volQ15) >> 15;
        int32_t R = ((int32_t)trackR16 * volQ15) >> 15;

        if (ch.outputChannels == AudioChannel::ALL) {
            const int32_t panLQ15 = (int32_t)(ch.panL * 32768.0f);
            const int32_t panRQ15 = (int32_t)(ch.panR * 32768.0f);
            L = (L * panLQ15) >> 15;
            R = (R * panRQ15) >> 15;
        } else {
            if (!(ch.outputChannels & AudioChannel::CH1)) L = 0;
            if (!(ch.outputChannels & AudioChannel::CH2)) R = 0;
        }

        mixL += L;
        mixR += R;

        // Update remaining-time status — once per channel per frame, off
        // the hot multiply path.
        if (ws.sampleRate_Hz > 0) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t framesLeft =
                ws.totalFrames - srcRead + (uint32_t)(ws.bufLen - ws.bufPos);
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }
    }

    // Skip silence — keeps the ring near-empty while idle so a fresh
    // play() reaches output within milliseconds.
    if (!hasAudio) return false;

    // Master volume in Q15.  master=1.0 → mvolQ15=32768 → mix unchanged.
    const int32_t mvolQ15 = (int32_t)(_masterVolume * 32768.0f);
    mixL = (mixL * mvolQ15) >> 15;
    mixR = (mixR * mvolQ15) >> 15;

    // Clamp to full int16 range — matches the legacy float code's
    // saturation at ±1.0 before the output-gain step below.
    if (mixL >  32767) mixL =  32767;
    if (mixL < -32768) mixL = -32768;
    if (mixR >  32767) mixR =  32767;
    if (mixR < -32768) mixR = -32768;

    // Output headroom scaling — preserves the legacy `* MAX_AMPLITUDE`
    // step (24000 / 32768 ≈ 0.732, −2.7 dB) so transients don't slam
    // the DAC.  Final int16 written to the SPSC ring.
    mixL = (mixL * MAX_AMPLITUDE) >> 15;
    mixR = (mixR * MAX_AMPLITUDE) >> 15;

    ringBuf().write((int16_t)mixL, (int16_t)mixR);
    return true;
}

// ============================================================================
//  BLOCK-MODE PRODUCER (Phase 5, feature/idf-component-build, 2026-05-28)
// ============================================================================
//
// produceBlock(N) — N frames per call.  The per-channel scale step
// (sample × volQ15 → scaled int16) runs through esp-dsp's
// `dsps_mulc_s16_ae32` (hand-tuned Xtensa LX7 assembly, ~3-5× faster
// than scalar at typical block sizes).  Accumulation goes into a
// stack-resident int32 mix buffer; the compiler auto-vectorises the
// int16→int32 widen-add loop on LX7.
//
// Block size cap = 256 frames (5.3 ms @ 48 kHz):
//   - Long enough to amortise dsps_mulc_s16_ae32's setup cost.
//   - Short enough that per-block fade granularity stays imperceptible
//     (10 steps in a 50 ms fade; threshold of audibility ≈ 50 steps).
//   - Stack budget: 256 × 2 (L/R) × 4 (int32 mix) + 256 × 2 × 2 (int16
//     track) = 3 KB, well under the 8 KB producer task stack.
//
// Falls back to a scalar Q15 path when esp-dsp isn't in the build
// (SFX_HAS_ESP_DSP = 0) — same arithmetic, just no SIMD speedup.

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::produceBlock(int maxFrames) {
    constexpr int kBlockMax = 256;

    const uint32_t freeSlots = ringBuf().availableWrite();
    int N = maxFrames;
    if (N > (int)freeSlots) N = (int)freeSlots;
    if (N > kBlockMax)      N = kBlockMax;
    if (N <= 0) return 0;

    // Stack scratch.  Zeroing mixL/mixR once costs ~2 KB of memsets per
    // block — negligible vs the per-sample multiply work it saves.
    int32_t mixL[kBlockMax] = {};
    int32_t mixR[kBlockMax] = {};
    int16_t trackL[kBlockMax];
    int16_t trackR[kBlockMax];
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;
        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        // Pull up to N resampled int16 samples from this channel's
        // drain.  EOF mid-block terminates the channel but we still
        // mix what we got into the block (no audible click).
        int got = 0;
        bool sourceDied = false;
        for (int j = 0; j < N; ++j) {
            int16_t l = 0, r = 0;
            if (!getWavSample(ch, l, r)) {
                sourceDied = true;
                break;
            }
            trackL[j] = l;
            trackR[j] = r;
            got = j + 1;
        }
        if (sourceDied && got == 0) {
            destroyAudioSource(ws.source);
            ws.active = false;
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            checkAndPlayNextQueued(i);
            continue;
        }
        if (got == 0) continue;
        hasAudio = true;

        // Arm tail fade-out — same logic as produceFrame, just at
        // block cadence.
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t framesLeft =
                ws.totalFrames - srcRead + (uint32_t)(ws.bufLen - ws.bufPos);
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Per-block fade — one volume value for the whole block.
        // Advance ch.fadeVolume by `got` steps for the next block.
        float effectiveVolume = ch.volume;
        bool  fadeOutComplete = false;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep * (float)got;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                fadeOutComplete = true;   // tear down AFTER mixing this block
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                ch.fadeVolume = 1.0f;
                ch.fading = false;
            }
        }

        // Apply vol + pan as a single Q15 multiply per side.  pan ×
        // vol fits in Q15 because both are in [0, 1].
        int16_t volLQ15, volRQ15;
        bool muteL = false, muteR = false;
        if (ch.outputChannels == AudioChannel::ALL) {
            volLQ15 = (int16_t)(effectiveVolume * ch.panL * 32768.0f);
            volRQ15 = (int16_t)(effectiveVolume * ch.panR * 32768.0f);
        } else {
            const int16_t volQ15 = (int16_t)(effectiveVolume * 32768.0f);
            volLQ15 = (ch.outputChannels & AudioChannel::CH1) ? volQ15 : (muteL = true, int16_t{0});
            volRQ15 = (ch.outputChannels & AudioChannel::CH2) ? volQ15 : (muteR = true, int16_t{0});
        }

        // ── Hot kernel ────────────────────────────────────────────────
#if SFX_HAS_ESP_DSP
        if (muteL) std::memset(trackL, 0, (size_t)got * sizeof(int16_t));
        else       dsps_mulc_s16_ae32(trackL, trackL, got, volLQ15, 1, 1);
        if (muteR) std::memset(trackR, 0, (size_t)got * sizeof(int16_t));
        else       dsps_mulc_s16_ae32(trackR, trackR, got, volRQ15, 1, 1);
#else
        for (int j = 0; j < got; ++j) {
            trackL[j] = (int16_t)(((int32_t)trackL[j] * (int32_t)volLQ15) >> 15);
            trackR[j] = (int16_t)(((int32_t)trackR[j] * (int32_t)volRQ15) >> 15);
        }
#endif

        // int16 → int32 widen + accumulate.  Compiler auto-vectorises
        // this on LX7 — no esp-dsp variant is needed (and one doesn't
        // exist for the s16→s32 mac shape).
        for (int j = 0; j < got; ++j) {
            mixL[j] += (int32_t)trackL[j];
            mixR[j] += (int32_t)trackR[j];
        }

        // Remaining-sec status — once per channel per block, off the
        // per-sample hot path.
        if (ws.sampleRate_Hz > 0) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t framesLeft =
                ws.totalFrames - srcRead + (uint32_t)(ws.bufLen - ws.bufPos);
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }

        // Deferred teardown for completed fade-outs / mid-block EOFs.
        if (fadeOutComplete || sourceDied) {
            destroyAudioSource(ws.source);
            ws.active = false;
            _channelPlaying[i] = false;
            if (sourceDied) _channelRemainingSec[i] = 0.0f;
            checkAndPlayNextQueued(i);
        }
    }

    if (!hasAudio) return 0;

    // Master volume in Q15, clamp to int16, headroom × MAX_AMPLITUDE,
    // write to ring.  Per-sample loop — small enough that LX7 auto-vec
    // handles it; esp-dsp doesn't have a s32→s16 saturating-scale op
    // that fits this shape.
    const int32_t mvolQ15 = (int32_t)(_masterVolume * 32768.0f);
    for (int j = 0; j < N; ++j) {
        int32_t L = (mixL[j] * mvolQ15) >> 15;
        int32_t R = (mixR[j] * mvolQ15) >> 15;
        if (L >  32767) L =  32767; else if (L < -32768) L = -32768;
        if (R >  32767) R =  32767; else if (R < -32768) R = -32768;
        L = (L * (int32_t)MAX_AMPLITUDE) >> 15;
        R = (R * (int32_t)MAX_AMPLITUDE) >> 15;
        ringBuf().write((int16_t)L, (int16_t)R);
    }

    return N;
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::produce(int maxFrames) {
    if (!_initialized) return 0;
    
    // Process any pending commands first
    processCommands();

    // Produce frames into ring buffer FIRST — this is time-critical.
    // The ring is a small core-to-core FIFO (~85 ms) that must stay fed
    // to prevent consumer underruns.  Block-mode (produceBlock) calls
    // esp-dsp's SIMD kernel per channel; produceFrame() stays as the
    // single-frame fallback (kept for tests + Pico builds without
    // esp-dsp).
    int produced = 0;
    while (produced < maxFrames) {
        const int got = produceBlock(maxFrames - produced);
        if (got <= 0) break;
        produced += got;
    }

    // Top-up EVERY active channel whose drain buffer is below 50 %.
    // (Was previously round-robin "one channel per call"; that left
    // 2 + active channels behind the consume rate on MP3 sources —
    // 2026-05-28 trace showed ch1 drain stuck at 6 % with persistent
    // under+ ticks.)  With kMaxRefillFrames=2048 each call costs
    // ~10 ms decode time for MP3; capping ≤3 active sources and ≤4 KB
    // ring drain risk keeps producer-blocking under one ring tick.
    //
    // Skip channels whose source returned 0 bytes the previous attempt
    // and isn't marked exhausted — these are the "phantom-active"
    // failure mode (source forever silent but not finished); we
    // count one starve per call and skip the decode work to avoid
    // burning a refill slot on a dead source.
    for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
        Channel& ch = _channels[i];
        if (!ch.wav.active) continue;
        int available = ch.wav.bufLen - ch.wav.bufPos;
        if (available < WAV_BUF_FRAMES / 2) {
            refillDrainBuffer(ch);
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

    // Track min fill % since last log — catches transient near-empty
    // dips that didn't quite underrun.  Updated lock-free via CAS;
    // relaxed ordering is fine (a sloppy reset/update race only
    // misses one tick on a 1 Hz cadence).
    {
        const uint8_t pct = (uint8_t)((avail * 100u) / RING_FRAMES);
        uint8_t prevMin = _ringMinFillPct.load(std::memory_order_relaxed);
        while (pct < prevMin &&
               !_ringMinFillPct.compare_exchange_weak(prevMin, pct,
                                                     std::memory_order_relaxed)) {}
    }

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
            stopWithFadeMs(cmd.channelId, cmd.stopMode, cmd.fadeMs);
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
void AudioMixer<TI2S, TCodec>::stopAsyncWithFadeMs(int channel, uint16_t fadeMs) {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    Command cmd{};
    cmd.type = CommandType::Stop;
    cmd.channelId = channel;
    cmd.stopMode = AudioStopMode::Fade;
    cmd.fadeMs = fadeMs;
    _channelPlaying[channel].store(false, std::memory_order_release);
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

    uint32_t nextTelemetryMs = millis() + 1000;
    uint32_t prevUnderruns   = mixer._underruns.load(std::memory_order_relaxed);

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

        // ── Pacing telemetry (1 Hz, only when audio is playing) ─────
        const uint32_t now = millis();
        if (now >= nextTelemetryMs) {
            nextTelemetryMs = now + 1000;

            uint8_t activeMask = 0;
            uint8_t activeCount = 0;
            for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
                if (mixer._channels[i].wav.active) {
                    activeMask |= (1u << i);
                    ++activeCount;
                }
            }
            if (activeCount == 0) continue;

            const uint32_t under  = mixer._underruns.load(std::memory_order_relaxed);
            const uint32_t dUnder = under - prevUnderruns;
            prevUnderruns         = under;

            const uint8_t  minFill = mixer._ringMinFillPct.exchange(100, std::memory_order_relaxed);
            const uint32_t maxRdUs = mixer._maxSdReadUs.exchange(0, std::memory_order_relaxed);
            const uint32_t slow5   = mixer._slowSdReads.exchange(0, std::memory_order_relaxed);
            const uint32_t slow20  = mixer._verySlowSdReads.exchange(0, std::memory_order_relaxed);

            // Compose per-channel starve summary (only active channels)
            char starveBuf[96];
            int  off = 0;
            for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
                if (!(activeMask & (1u << i))) continue;
                const uint32_t st = mixer._channelStarves[i].exchange(0, std::memory_order_relaxed);
                const int     fillPct = mixer.getWavBufferFillPercent(i);
                int n = snprintf(starveBuf + off, sizeof(starveBuf) - off,
                                 " ch%d=%u/%d%%", i, (unsigned)st, fillPct);
                if (n < 0 || off + n >= (int)sizeof(starveBuf)) break;
                off += n;
            }

            SFX_LOG_INFO("[pace] active=%u ring-min=%u%% under+%u sd-max=%uus slow5/20=%u/%u starves:%s",
                         (unsigned)activeCount,
                         (unsigned)minFill,
                         (unsigned)dUnder,
                         (unsigned)maxRdUs,
                         (unsigned)slow5, (unsigned)slow20,
                         starveBuf);
        }
    }

    MIXER_LOG("Producer task exiting");
    // Signal stopProducerTask() then suspend.  stopProducerTask deletes
    // us from outside so the TCB + stack are reclaimed synchronously —
    // self-deletion via vTaskDelete(nullptr) defers reap to the idle
    // task, which under upload-induced suspend/resume pressure caused
    // producer-recreate to OOM (verified rollback of build #285).
    mixer._producerExited.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
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

    _producerExited.store(false, std::memory_order_release);
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

    // Wait for the task to fall out of its loop and signal
    // _producerExited.  After the signal it suspends itself; we then
    // vTaskDelete it from outside, which frees the TCB + stack
    // synchronously rather than waiting for the idle task to reap.
    TaskHandle_t handle = _producerTaskHandle;
    _producerTaskHandle = nullptr;
    if (handle) {
        for (int i = 0; i < 40; ++i) {
            if (_producerExited.load(std::memory_order_acquire)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!_producerExited.load(std::memory_order_acquire)) {
            MIXER_WARN("Producer task did not exit cleanly after 200 ms — forcing delete");
        }
        vTaskDelete(handle);
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
