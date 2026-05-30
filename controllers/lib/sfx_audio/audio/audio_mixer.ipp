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
#  if __has_include(<dsps_mulc.h>) && __has_include(<dsps_add.h>)
#    include <dsps_mulc.h>
#    include <dsps_add.h>
#    define SFX_HAS_ESP_DSP 1
#  endif
#endif
#endif
#ifndef SFX_HAS_ESP_DSP
#  define SFX_HAS_ESP_DSP 0
#endif

// ── Kernel selection (Phase 5b experiment, 2026-05-28) ──────────────
// SFX_AUDIO_KERNEL_FLOAT=1  → produce() routes to produceBlockFloat()
//                            (float32 + dsps_*_f32_ae32 SIMD).
// SFX_AUDIO_KERNEL_FLOAT=0  → produce() routes to produceBlock()
//                            (int16 / Q15 + dsps_mulc_s16_ae32 SIMD).
// Toggle to A/B the two kernels on the same firmware structure.
#ifndef SFX_AUDIO_KERNEL_FLOAT
#  define SFX_AUDIO_KERNEL_FLOAT 0
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
    // Phase 7 wave 1 lazy `_sdReadBuf` — restored 2026-05-28 after the
    // 512 KB upload crash was diagnosed and proven UNRELATED to this
    // path (commit 12d8c69).  The 32 KB DMA-cap scratch is now
    // allocated on the FIRST `play(preloadIntoMemory=true)` via
    // `acquireSdScratch()`, refcounted across concurrent preloads, and
    // freed back to the DMA-cap pool when the refcount drops to zero.
    // No allocation happens here in begin().  Reclaims 32 KB DMA-cap
    // SRAM for I²S DMA / SDMMC ISR whenever no preload is in flight.
    // Only effects with `preloadIntoMemory=true` exercise this path
    // (GunFx today); engine / alert / lightfx all stream from
    // AudioAssetCache + Mp3PsramSource, never touching this buffer.

    // ---- Allocate per-channel WAV decode buffers + queue from PSRAM ----
    // Per-channel `queue[QUEUE_SIZE_PER_CHANNEL]` lives in PSRAM (re-applied
    // 2026-05-27 after the previous re-apply was rolled back on a stutter
    // suspicion that turned out to be SD-card-side, not the queue).  Each
    // slot ~150 B; 4 × 8 ch = ~5 KB the singleton no longer carries in BSS.
    // Touched only on protocol-rate enqueue / dequeue — PSRAM latency
    // invisible at that cadence.  Failure rollback mirrors wav.bufL/R.
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        // Reset per-channel state field-by-field (can't do
        // `_channels[i] = Channel{}` because WavState now holds
        // atomics — std::atomic isn't move-assignable).
        Channel& chReset = _channels[i];
        chReset.wav.source = nullptr;
        chReset.wav.sampleRate_Hz = 0;
        chReset.wav.numChannels = 0;
        chReset.wav.bitsPerSample = 0;
        chReset.wav.totalFrames = 0;
        chReset.wav.loopCountInit = 0;
        chReset.wav.resampleRatio = 1.0f;
        chReset.wav.resampleFrac  = 0.0f;
        chReset.wav.ringReset();
        chReset.wav.active.store(false, std::memory_order_relaxed);
        chReset.wav.needsPrefill.store(false, std::memory_order_relaxed);
        chReset.wav.sourceExhausted.store(false, std::memory_order_relaxed);
        chReset.volume = 1.0f;
        chReset.pan    = 0.0f;
        chReset.panL   = 0.707f;
        chReset.panR   = 0.707f;
        chReset.outputChannels = AudioChannel::ALL;
        chReset.filename[0] = '\0';
        chReset.mute = false;
        chReset.fading = false;
        chReset.fadeVolume = 1.0f;
        chReset.fadeStep   = 0.0f;
        chReset.fadeOutTriggerFrames = 0;
        chReset.fadeOutStep = 0.0f;
        chReset.queueHead = 0;
        chReset.queueTail = 0;
        chReset.pendingLoopBehavior = QueueLoopBehavior::StopImmediate;
        chReset.hasQueuedItem = false;

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
            // _sdReadBuf is lazy now — nothing allocated here to free.
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
            // Roll back per-channel buffers.  _sdReadBuf is lazy now —
            // nothing allocated here to free.
            for (int j = 0; j < AUDIO_MAX_CHANNELS; j++) {
                sfxPsramFree(_channels[j].wav.bufL);
                sfxPsramFree(_channels[j].wav.bufR);
                sfxPsramFree(_channels[j].queue);
                _channels[j].wav.bufL = nullptr;
                _channels[j].wav.bufR = nullptr;
                _channels[j].queue    = nullptr;
            }
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
    // Phase 6: stop decoder task before producer so no new frames land
    // mid-shutdown.  Producer next, then I²S last.
    stopDecoderTask();
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

    // Free shared SD read buffer (DMA-cap SRAM or PSRAM fallback —
    // sfxPsramFree wraps heap_caps_free which accepts allocations from
    // any capability pool).
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
    // (Phase 6: setting active=false BEFORE destroying the source
    // makes any decoder-task observation of `active` race-safe — the
    // decoder skips the channel on its next pass and won't try to
    // call into a destroyed source.)
    ws.active.store(false, std::memory_order_release);
    destroyAudioSource(ws.source);
    ws.ringReset();
    ws.sourceExhausted.store(false, std::memory_order_relaxed);
    ws.needsPrefill   .store(false, std::memory_order_relaxed);
    ws.resampleFrac   = 0.0f;

    // ── Construct a concrete source ─────────────────────────────────
    // Preload first: short hot samples (gun, alert) get cached in
    // PSRAM so loops never touch SD.  openAndPreload() returns false
    // for oversize files / OOM headroom — caller falls through to
    // streaming.  Stage 4 will add WavPagedSource as a middle tier.
    if (options.preloadIntoMemory) {
        // Phase 7 wave 1 lazy SD scratch — acquire 32 KB DMA-cap buffer
        // for this preload, release it as soon as the read completes
        // (data is in PSRAM by then, scratch goes back to DMA-cap pool).
        // Refcount inside acquire/release keeps concurrent preloads
        // sharing one allocation.  Null return = DMA-cap exhausted and
        // PSRAM fallback also failed — fall through to PSRAM-source.
        if (uint8_t* scratch = acquireSdScratch()) {
            auto* preload = new (ch.sourceStorage) WavPreloadSource();
            if (preload->openAndPreload(filename, scratch,
                                        WAV_SD_READ_BYTES,
                                        WAV_MAX_PRELOAD_FRAMES)) {
                ws.source = preload;
            } else {
                preload->~WavPreloadSource();
                // fall through to PSRAM-source path
            }
            releaseSdScratch();
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
    // Ring already reset above.

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
            SFX_LOG_INFO("[mixer] ch%u seek %dms -> frame %lu/%lu (%.1fs file)",
                         channel, options.startOffsetMs, (unsigned long)offsetFrames,
                         (unsigned long)ws.totalFrames,
                         ws.sampleRate_Hz ? (float)ws.totalFrames / ws.sampleRate_Hz : 0.0f);
        } else {
            // Offset past end-of-file — the seek is SKIPPED and playback starts
            // at 0.  A large engine startingOffset on a short start sound lands
            // here (looks like "the offset does nothing").
            SFX_LOG_WARN("[mixer] ch%u startOffset %dms EXCEEDS file length "
                         "(%.1fs, %lu frames) — playing from 0",
                         channel, options.startOffsetMs,
                         ws.sampleRate_Hz ? (float)ws.totalFrames / ws.sampleRate_Hz : 0.0f,
                         (unsigned long)ws.totalFrames);
        }
    }

    strncpy(ch.filename, filename, CHANNEL_FILENAME_MAX - 1);
    ch.filename[CHANNEL_FILENAME_MAX - 1] = '\0';

    // Phase 6 (feature/audio-decode-prefetch): NO synchronous pre-fill.
    // The decoder task on Core 0 will fill the ring as soon as it sees
    // `needsPrefill=true` (typically within ~5 ms — its tick period).
    // The producer task on Core 1 emits silence for the brief window
    // until the first frames land; the alternative — blocking the
    // producer for ~70 ms inside this play() call — was the root cause
    // of the play-start under+ spike observed in build 472.  Imperceptible
    // ~5 ms silence at sound-effect start vs guaranteed underrun on the
    // currently-mixing channels is the right trade.
    ws.needsPrefill.store(true,  std::memory_order_relaxed);
    ws.active      .store(true,  std::memory_order_release);
    _channelPlaying[channel] = true;
    notifyDecoder();

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
    if (!ws.active.load(std::memory_order_acquire)) return;

    switch (mode) {
        case AudioStopMode::Immediate:
            // Clear active first (release) so the decoder task sees
            // the dead channel on its next pass and won't try to
            // refill mid-destroy.
            ws.active.store(false, std::memory_order_release);
            destroyAudioSource(ws.source);
            ws.ringReset();
            ws.sourceExhausted.store(false, std::memory_order_relaxed);
            ws.needsPrefill   .store(false, std::memory_order_relaxed);
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

    // ── SPSC ring write side (Phase 6) ─────────────────────────────
    // Called from the decoder task on Core 0.  Producer's readIdx
    // load with acquire (in availableWrite) pairs with our release
    // commitWrite at the end.  Writes go into the ring at
    //   (writeIdx & mask)
    // and may have to be split into two contiguous spans when the
    // ring boundary falls inside the write region — fully handled
    // inside the inner loop.
    constexpr uint32_t kRingMask = (uint32_t)WAV_BUF_FRAMES - 1;
    static_assert((WAV_BUF_FRAMES & (WAV_BUF_FRAMES - 1)) == 0,
                  "WAV_BUF_FRAMES must be power of 2 for ring masking");

    uint32_t space = ws.availableWrite();
    if (space == 0) return true;

    // ── Per-call decode-cost cap ────────────────────────────────────
    // Phase 6 the cap matters less because refill runs on Core 0 (no
    // producer blocking), but bounding the loop keeps decoder-task
    // wakeups responsive enough for Arduino loop + protocol to share
    // Core 0 nicely.  4096 frames ≈ 85 ms of audio per refill — long
    // enough to amortize libhelix per-frame overhead, short enough
    // that the decoder task yields back to Core 0 promptly.
    constexpr uint32_t kMaxRefillFrames = 4096;
    if (space > kMaxRefillFrames) space = kMaxRefillFrames;

    constexpr int kRefillBatchFrames = 256;
    float scratchL[kRefillBatchFrames];
    float scratchR[kRefillBatchFrames];

#if SFX_AUDIO_PACE_TELEMETRY
    const uint32_t t0 = micros();
#endif
    const uint32_t startIdx = ws.writeIdx.load(std::memory_order_relaxed);
    uint32_t got = 0;
    while (got < space) {
        const uint32_t batchCap =
            (space - got < (uint32_t)kRefillBatchFrames) ? (space - got)
                                                         : (uint32_t)kRefillBatchFrames;
        const uint32_t n = ws.source->readFrames(scratchL, scratchR, batchCap);
        if (n == 0) break;

        // Scatter into the ring, handling the wrap point.  At most one
        // wrap per batch (batchCap ≤ ring size).
        for (uint32_t k = 0; k < n; ++k) {
            float fL = scratchL[k];
            float fR = scratchR[k];
            if (fL >  1.0f) fL =  1.0f; else if (fL < -1.0f) fL = -1.0f;
            if (fR >  1.0f) fR =  1.0f; else if (fR < -1.0f) fR = -1.0f;
            const uint32_t pos = (startIdx + got + k) & kRingMask;
            ws.bufL[pos] = (int16_t)(fL * 32767.0f);
            ws.bufR[pos] = (int16_t)(fR * 32767.0f);
        }
        got += n;
        if (n < batchCap) break;   // short read → source paused / EOF
    }
#if SFX_AUDIO_PACE_TELEMETRY
    const uint32_t dtUs = micros() - t0;
    // Track max-since-last-log via lock-free CAS (relaxed — the
    // periodic logger races safely; missing one tick is OK).
    uint32_t prevMax = _maxSdReadUs.load(std::memory_order_relaxed);
    while (dtUs > prevMax &&
           !_maxSdReadUs.compare_exchange_weak(prevMax, dtUs,
                                               std::memory_order_relaxed)) {}
    if (dtUs > 5000u)  _slowSdReads.fetch_add(1, std::memory_order_relaxed);
    if (dtUs > 20000u) _verySlowSdReads.fetch_add(1, std::memory_order_relaxed);
#endif

    // Publish written frames to the producer with release.  After this
    // commit the producer's availableRead() will see the new samples,
    // and its acquire load of writeIdx pairs with this release.
    if (got > 0) ws.commitWrite(got);

    // Stamp source-exhausted state for the producer to observe AFTER
    // draining the last samples (it checks sourceExhausted only when
    // availableRead() == 0).  needsPrefill cleared so the decoder
    // task doesn't re-poll this channel until something kicks it.
    ws.needsPrefill.store(false, std::memory_order_relaxed);
    const bool exhausted = ws.source && ws.source->isExhausted();
    if (exhausted) ws.sourceExhausted.store(true, std::memory_order_release);

    // Return TRUE if we got data OR the source is still transiently
    // buffering (paged source waiting for the next page).  FALSE only
    // for permanent exhaustion.
    return got > 0 || !exhausted;
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::getWavSample(Channel& ch, int16_t& outL, int16_t& outR) {
    WavState& ws = ch.wav;
    constexpr uint32_t kRingMask = (uint32_t)WAV_BUF_FRAMES - 1;

    // SPSC read side (Phase 6).  availableRead() does an acquire load
    // of writeIdx that pairs with the decoder task's release on
    // commitWrite; readIdx is loaded relaxed because the producer is
    // its sole writer.
    const uint32_t avail = ws.availableRead();
    const bool     act   = ws.active.load(std::memory_order_acquire);
    if (avail < 2 && act) {
        // Drain ran below the linear-interp threshold (need 2 samples
        // for one interpolated output).  Could be a transient (decoder
        // hasn't filled yet) or a real source EOF.
#if SFX_AUDIO_PACE_TELEMETRY
        const intptr_t cidx = &ch - _channels;
        if (cidx >= 0 && cidx < AUDIO_MAX_CHANNELS) {
            _channelStarves[cidx].fetch_add(1, std::memory_order_relaxed);
        }
#endif
        // If the decoder marked the source done, tear down NOW even if
        // there's 1 sample remaining — we can't interp from a single
        // sample (no neighbor) and waiting for avail==0 stalls forever
        // when resampleRatio < 1 (24 kHz→48 kHz upsamples advance 0
        // frames every other output sample, so the avail-1 clamp on
        // commitRead pins advance=0 once avail==1).  Verified 2026-05-28
        // against the Phase 6 trace: ch0 init.mp3 (24 kHz mono) stuck
        // active with empty drain for 1+ s after natural EOF.
        if (ws.sourceExhausted.load(std::memory_order_acquire)) {
            if (avail > 0) ws.commitRead(avail);  // drain the residue
            outL = outR = 0;
            return false;
        }
        outL = outR = 0;
        return true;        // transient drain; keep channel alive
    }

    const uint32_t rIdx     = ws.readIdx.load(std::memory_order_relaxed);
    const uint32_t pos0     = rIdx       & kRingMask;
    const uint32_t pos1     = (rIdx + 1) & kRingMask;
    const int32_t  fracQ15  = (int32_t)(ws.resampleFrac * 32768.0f);
    const int32_t  aL = ws.bufL[pos0];
    const int32_t  bL = ws.bufL[pos1];
    const int32_t  aR = ws.bufR[pos0];
    const int32_t  bR = ws.bufR[pos1];
    outL = (int16_t)(aL + (((bL - aL) * fracQ15) >> 15));
    outR = (int16_t)(aR + (((bR - aR) * fracQ15) >> 15));

    // Advance resampler — float frac → int advance, then publish the
    // commit to the decoder.  We never advance past `avail - 1` (the
    // interp needs sample [rIdx+1] to be valid too); clamping here
    // prevents a brief over-read at the drain boundary.
    ws.resampleFrac += ws.resampleRatio;
    uint32_t advance = (uint32_t)ws.resampleFrac;
    if (advance > avail - 1) advance = avail - 1;
    if (advance > 0) ws.commitRead(advance);
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
            ws.active.store(false, std::memory_order_release);
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
                ws.totalFrames - srcRead + ws.availableRead();
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
                ws.active.store(false, std::memory_order_release);
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
                ws.totalFrames - srcRead + ws.availableRead();
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
            ws.active.store(false, std::memory_order_release);
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
                ws.totalFrames - srcRead + ws.availableRead();
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
                ws.totalFrames - srcRead + ws.availableRead();
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }

        // Deferred teardown for completed fade-outs / mid-block EOFs.
        if (fadeOutComplete || sourceDied) {
            destroyAudioSource(ws.source);
            ws.active.store(false, std::memory_order_release);
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

// ============================================================================
//  FLOAT-KERNEL BLOCK PRODUCER (Phase 5b experiment, 2026-05-28)
// ============================================================================
//
// Parallel to `produceBlock()`.  Same block-mode architecture
// (kBlockMax = 256 frames, per-block fade granularity, deferred
// teardown), but the per-channel scale + accumulate run in float32
// via esp-dsp's `dsps_mulc_f32_ae32` + `dsps_add_f32_ae32`.
//
// Trade-offs vs the int16 / Q15 kernel:
//   + Simpler arithmetic — no Q15 conversion, no overflow management,
//     no int32 widening accumulate.  Master volume + clamp + headroom
//     are one float multiply + clamp each.
//   + Pan + volume combine cleanly as a single float multiplier.
//   − 2× memory bandwidth on the per-block scratch (4 B vs 2 B per
//     sample → 8 KB per block-side scratch vs 4 KB).  Tight on the
//     LX7 L1 cache.
//   − int16→float conversion at the drain-read boundary (the drain
//     buffer is still int16 so we don't need to also flip its
//     storage).  One multiply + load per sample.
//   − Float SIMD lanes are 4-wide vs int16's 8-wide on the Xtensa
//     ext, so each SIMD instruction processes half as many lanes
//     per cycle (compensated by simpler kernel body).
//
// The outcome of `produceBlockFloat` vs `produceBlock` on the same
// gun-during-engine 2-channel trace decides the production kernel.

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::produceBlockFloat(int maxFrames) {
#if !SFX_HAS_ESP_DSP
    // No esp-dsp = no float SIMD; fall through to the int16 kernel.
    return produceBlock(maxFrames);
#else
    constexpr int kBlockMax = 256;

    const uint32_t freeSlots = ringBuf().availableWrite();
    int N = maxFrames;
    if (N > (int)freeSlots) N = (int)freeSlots;
    if (N > kBlockMax)      N = kBlockMax;
    if (N <= 0) return 0;

    // Float scratch — 4 KB per side (vs 1 KB for int16).  Total
    // stack: 4 × (mixL/mixR/trackL/trackR) × 4 B × 256 = 4 KB.
    // Fits the 8 KB producer task stack with comfortable headroom.
    float mixL[kBlockMax] = {};
    float mixR[kBlockMax] = {};
    float trackL[kBlockMax];
    float trackR[kBlockMax];
    bool hasAudio = false;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;
        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        // Pull up to N samples, convert int16 → float [-1, +1] inline.
        // Conversion: one int→float + one fmul per sample.  At 256
        // samples × 2 ch × ~6 active channels worst case, that's
        // ~3 k float conversions per block — small fraction of the
        // mix kernel itself, but a real cost we're trading for
        // simpler kernel math.
        constexpr float kInt16ToFloat = 1.0f / 32768.0f;
        int got = 0;
        bool sourceDied = false;
        for (int j = 0; j < N; ++j) {
            int16_t l = 0, r = 0;
            if (!getWavSample(ch, l, r)) {
                sourceDied = true;
                break;
            }
            trackL[j] = (float)l * kInt16ToFloat;
            trackR[j] = (float)r * kInt16ToFloat;
            got = j + 1;
        }
        if (sourceDied && got == 0) {
            destroyAudioSource(ws.source);
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            _channelRemainingSec[i] = 0.0f;
            checkAndPlayNextQueued(i);
            continue;
        }
        if (got == 0) continue;
        hasAudio = true;

        // Arm tail fade-out (same logic as produceBlock).
        if (ch.fadeOutTriggerFrames > 0 && !ch.fading) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t framesLeft =
                ws.totalFrames - srcRead + ws.availableRead();
            if (framesLeft <= ch.fadeOutTriggerFrames) {
                ch.fading               = true;
                ch.fadeVolume           = 1.0f;
                ch.fadeStep             = ch.fadeOutStep;
                ch.fadeOutTriggerFrames = 0;
            }
        }

        // Per-block fade.
        float effectiveVolume = ch.volume;
        bool  fadeOutComplete = false;
        if (ch.fading) {
            effectiveVolume *= ch.fadeVolume;
            ch.fadeVolume -= ch.fadeStep * (float)got;
            if (ch.fadeStep > 0.0f && ch.fadeVolume <= 0.0f) {
                ch.fadeVolume = 0.0f;
                ch.fading = false;
                fadeOutComplete = true;
            } else if (ch.fadeStep < 0.0f && ch.fadeVolume >= 1.0f) {
                ch.fadeVolume = 1.0f;
                ch.fading = false;
            }
        }

        // Combine vol + pan into a single float multiplier per side.
        float volL, volR;
        if (ch.outputChannels == AudioChannel::ALL) {
            volL = effectiveVolume * ch.panL;
            volR = effectiveVolume * ch.panR;
        } else {
            volL = (ch.outputChannels & AudioChannel::CH1) ? effectiveVolume : 0.0f;
            volR = (ch.outputChannels & AudioChannel::CH2) ? effectiveVolume : 0.0f;
        }

        // ── Float SIMD hot kernel ──────────────────────────────────────
        // dsps_mulc_f32_ae32: track[i] = track[i] * volX
        // dsps_add_f32_ae32:  mix[i]   = track[i] + mix[i]
        dsps_mulc_f32_ae32(trackL, trackL, got, volL, 1, 1);
        dsps_mulc_f32_ae32(trackR, trackR, got, volR, 1, 1);
        dsps_add_f32_ae32 (trackL, mixL,   mixL, got, 1, 1, 1);
        dsps_add_f32_ae32 (trackR, mixR,   mixR, got, 1, 1, 1);

        // Remaining-sec status.
        if (ws.sampleRate_Hz > 0) {
            const uint32_t srcRead = ws.source ? ws.source->framesRead() : 0;
            const uint32_t framesLeft =
                ws.totalFrames - srcRead + ws.availableRead();
            _channelRemainingSec[i] = (float)framesLeft / (float)ws.sampleRate_Hz;
        }

        if (fadeOutComplete || sourceDied) {
            destroyAudioSource(ws.source);
            ws.active.store(false, std::memory_order_release);
            _channelPlaying[i] = false;
            if (sourceDied) _channelRemainingSec[i] = 0.0f;
            checkAndPlayNextQueued(i);
        }
    }

    if (!hasAudio) return 0;

    // Master volume + clamp + headroom + write to ring — float scalar
    // loop.  Could use dsps_mulc_f32_ae32 for the master-vol step but
    // the inline loop is simple enough that LX7 auto-vec handles it
    // and we'd need a separate pass for the clamp + int conversion.
    const float masterAndHeadroom = _masterVolume * (float)MAX_AMPLITUDE;
    for (int j = 0; j < N; ++j) {
        float L = mixL[j] * _masterVolume;
        float R = mixR[j] * _masterVolume;
        if (L >  1.0f) L =  1.0f; else if (L < -1.0f) L = -1.0f;
        if (R >  1.0f) R =  1.0f; else if (R < -1.0f) R = -1.0f;
        ringBuf().write((int16_t)(L * (float)MAX_AMPLITUDE),
                        (int16_t)(R * (float)MAX_AMPLITUDE));
    }
    (void)masterAndHeadroom;  // reserved for the fused dsps variant

    return N;
#endif  // SFX_HAS_ESP_DSP
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::produce(int maxFrames) {
    if (!_initialized) return 0;
    
    // Process any pending commands first
    processCommands();

    // Produce frames into ring buffer FIRST — this is time-critical.
    // The ring is a small core-to-core FIFO (~85 ms) that must stay fed
    // to prevent consumer underruns.  Block-mode (produceBlock /
    // produceBlockFloat) calls esp-dsp's SIMD kernel per channel;
    // produceFrame() stays as the single-frame fallback (kept for
    // tests + Pico builds without esp-dsp).
    //
    // SFX_AUDIO_KERNEL_FLOAT selects the A/B test variant — see the
    // macro definition at the top of this file for the trade-off
    // analysis.
    int produced = 0;
    while (produced < maxFrames) {
#if SFX_AUDIO_KERNEL_FLOAT
        const int got = produceBlockFloat(maxFrames - produced);
#else
        const int got = produceBlock(maxFrames - produced);
#endif
        if (got <= 0) break;
        produced += got;
    }

    // Phase 6 (feature/audio-decode-prefetch): the per-cycle refill
    // loop is GONE.  refillDrainBuffer is owned by the decoder task
    // on Core 0.  The producer here only mixes from the ring.
    // If any channel runs low on data, the decoder task will see it
    // on its next tick (5 ms typical) and refill — without ever
    // blocking THIS task.

    return produced;
}

// ============================================================================
//  I2S OUTPUT — CONSUMER (Core 1)
// ============================================================================

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::consumeAndOutput() {
    uint32_t avail = ringBuf().availableRead();

#if SFX_AUDIO_PACE_TELEMETRY
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
#endif

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
    if (!ws.active.load(std::memory_order_acquire) || WAV_BUF_FRAMES == 0) return 0;
    return (int)((ws.availableRead() * 100u) / (uint32_t)WAV_BUF_FRAMES);
}

template<typename TI2S, typename TCodec>
int AudioMixer<TI2S, TCodec>::getWavBufferFrames(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    const WavState& ws = _channels[channel].wav;
    if (!ws.active.load(std::memory_order_acquire)) return 0;
    return (int)ws.availableRead();
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

#if SFX_AUDIO_PACE_TELEMETRY
    uint32_t nextTelemetryMs = millis() + 1000;
    uint32_t prevUnderruns   = mixer._underruns.load(std::memory_order_relaxed);
#endif

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

#if SFX_AUDIO_PACE_TELEMETRY
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
#endif  // SFX_AUDIO_PACE_TELEMETRY
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

// ============================================================================
//  DECODER TASK (Phase 6, feature/audio-decode-prefetch, 2026-05-28)
// ============================================================================
//
// Owns the entire decode pipeline on Core 0.  Producer task on Core 1
// only reads from the per-channel SPSC rings.  This is the structural
// fix for the play-start under+ spike and the 5-channel sustained
// stutter, both of which were rooted in synchronous libhelix decode
// blocking the producer task for ~30-100 ms.
//
// Wake sources:
//   - xTaskNotifyGive from notifyDecoder() (play() / stop() / restart)
//   - kDecoderTickMs (5 ms) periodic timeout — catches channels whose
//     rings are draining without a wake (steady-state mixing)
//
// Each pass walks AUDIO_MAX_CHANNELS and calls refillDrainBuffer for
// any channel with availableWrite() >= kRefillThreshold (currently
// half the ring).  refillDrainBuffer itself is now lock-free against
// the producer — see audio_mixer.ipp line ~890 for the SPSC write
// implementation.

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::notifyDecoder() {
    if (_decoderTaskHandle) {
        xTaskNotifyGive(_decoderTaskHandle);
    }
}

// ── Lazy SD scratch (Phase 7 polish, 2026-05-28) ─────────────────────
//
// WavPreloadSource::openAndPreload calls `acquireSdScratch()` to get a
// 32 KB DMA-cap SD read buffer, then releases it in the source's
// destructor.  Refcount-protected so two preload opens running back
// to back share the same buffer.  When the count drops to zero, the
// buffer is freed and DMA-cap SRAM returned to the heap pool — keeping
// it available for I²S DMA / SDMMC ISR when no preload is in flight.
//
// Concurrency: callers are the producer task (Core 1) servicing play()
// commands.  The mutex is overkill for the current single-caller case
// but futureproofs against play() being moved to another core.

template<typename TI2S, typename TCodec>
uint8_t* AudioMixer<TI2S, TCodec>::acquireSdScratch() {
#if SFX_PLATFORM_ESP32
    if (!_sdScratchMutexInit) {
        sfxMutexInit(_sdScratchMutex);
        _sdScratchMutexInit = true;
    }
    sfxMutexLock(_sdScratchMutex);
    if (!_sdReadBuf) {
        _sdReadBuf = static_cast<uint8_t*>(
            heap_caps_malloc(WAV_SD_READ_BYTES,
                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (_sdReadBuf) {
            MIXER_LOG("SD scratch: %d KB in DMA-cap SRAM (lazy; users=1)",
                      WAV_SD_READ_BYTES / 1024);
        } else {
            MIXER_WARN("SD scratch: DMA-cap SRAM exhausted (%u KB) — falling back to PSRAM (~1 MB/s)",
                       (unsigned)(WAV_SD_READ_BYTES / 1024));
            _sdReadBuf = static_cast<uint8_t*>(sfxPsramMalloc(WAV_SD_READ_BYTES));
        }
        if (!_sdReadBuf) {
            MIXER_ERROR("SD scratch alloc failed (%d bytes) — preload disabled this play",
                        WAV_SD_READ_BYTES);
            sfxMutexUnlock(_sdScratchMutex);
            return nullptr;
        }
    }
    ++_sdScratchUsers;
    uint8_t* p = _sdReadBuf;
    sfxMutexUnlock(_sdScratchMutex);
    return p;
#else
    // Pico: keep simple — single eager PSRAM alloc on first call.
    if (!_sdReadBuf) {
        _sdReadBuf = static_cast<uint8_t*>(sfxPsramMalloc(WAV_SD_READ_BYTES));
    }
    if (_sdReadBuf) ++_sdScratchUsers;
    return _sdReadBuf;
#endif
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::releaseSdScratch() {
#if SFX_PLATFORM_ESP32
    if (!_sdScratchMutexInit) return;   // never acquired → nothing to release
    sfxMutexLock(_sdScratchMutex);
    if (_sdScratchUsers > 0) --_sdScratchUsers;
    if (_sdScratchUsers == 0 && _sdReadBuf) {
        sfxPsramFree(_sdReadBuf);   // wraps heap_caps_free, accepts any pool
        _sdReadBuf = nullptr;
        MIXER_LOG("SD scratch: released %d KB", WAV_SD_READ_BYTES / 1024);
    }
    sfxMutexUnlock(_sdScratchMutex);
#else
    if (_sdScratchUsers > 0) --_sdScratchUsers;
    if (_sdScratchUsers == 0 && _sdReadBuf) {
        sfxPsramFree(_sdReadBuf);
        _sdReadBuf = nullptr;
    }
#endif
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::decoderTaskFunc(void* /*param*/) {
    auto& mixer = AudioMixer::instance();
    MIXER_LOG("Decoder task started on core %d (priority %d)",
              xPortGetCoreID(), uxTaskPriorityGet(nullptr));

    constexpr uint32_t kRefillThresholdFrames = (uint32_t)WAV_BUF_FRAMES / 2;

    while (mixer._decoderRunning.load(std::memory_order_acquire)) {
        // Wait for a wake or the periodic tick.  Tick = 5 ms keeps the
        // worst-case latency on idle-but-active channels short.
        ulTaskNotifyTake(/*clearOnExit*/ pdTRUE, pdMS_TO_TICKS(kDecoderTickMs));

        for (int i = 0; i < AUDIO_MAX_CHANNELS; ++i) {
            Channel& ch = mixer._channels[i];
            WavState& ws = ch.wav;
            // Producer-published `active` with acquire ordering; if the
            // channel was just torn down the source pointer might be
            // null mid-pass — the refillDrainBuffer guard catches that.
            if (!ws.active.load(std::memory_order_acquire)) continue;
            if (ws.sourceExhausted.load(std::memory_order_acquire)) continue;

            const bool needs = ws.needsPrefill.load(std::memory_order_relaxed);
            const bool hungry = ws.availableWrite() >= kRefillThresholdFrames;
            if (needs || hungry) {
                mixer.refillDrainBuffer(ch);
            }
        }
    }

    MIXER_LOG("Decoder task exiting");
    mixer._decoderExited.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}

template<typename TI2S, typename TCodec>
bool AudioMixer<TI2S, TCodec>::startDecoderTask() {
    if (_decoderRunning.load(std::memory_order_acquire)) return true;
    if (!_initialized.load(std::memory_order_acquire)) {
        MIXER_ERROR("startDecoderTask() called before begin()");
        return false;
    }

    _decoderExited .store(false, std::memory_order_release);
    _decoderRunning.store(true,  std::memory_order_release);

    BaseType_t result = xTaskCreatePinnedToCore(
        decoderTaskFunc, "AudioDecoder",
        _decoderStackSize, nullptr,
        _decoderPriority, &_decoderTaskHandle,
        _decoderCore
    );
    if (result != pdPASS) {
        _decoderRunning.store(false, std::memory_order_release);
        MIXER_ERROR("Failed to create decoder task (err=%d)", result);
        return false;
    }

    MIXER_LOG("Decoder task created: core=%d priority=%d stack=%d",
              _decoderCore, _decoderPriority, _decoderStackSize);
    return true;
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::stopDecoderTask() {
    if (!_decoderRunning.load(std::memory_order_acquire)) return;

    MIXER_LOG("Stopping decoder task...");
    _decoderRunning.store(false, std::memory_order_release);
    // Wake it so it sees the flag and exits.
    if (_decoderTaskHandle) xTaskNotifyGive(_decoderTaskHandle);

    TaskHandle_t handle = _decoderTaskHandle;
    _decoderTaskHandle = nullptr;
    if (handle) {
        for (int i = 0; i < 40; ++i) {
            if (_decoderExited.load(std::memory_order_acquire)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!_decoderExited.load(std::memory_order_acquire)) {
            MIXER_WARN("Decoder task did not exit cleanly after 200 ms — forcing delete");
        }
        vTaskDelete(handle);
    }
    MIXER_LOG("Decoder task stopped");
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::suspendAudio() {
    MIXER_LOG("Suspending audio (decoder + producer + consumer)...");

    // ORDER MATTERS — the decoder MUST stop before stopAll() runs, or
    // the decoder's per-channel walk can race against stop()'s source
    // destruction (decoder reads active=true → source destroyed by
    // stop() → decoder calls source->readFrames on a destroyed
    // pointer → crash → watchdog reboot).  Discovered 2026-05-28
    // diagnosing the upload-induced reboot AFTER the Phase 7 stack /
    // UART trims were already reverted — the SUSPEND ORDER itself was
    // wrong since Phase 6 introduced the decoder task.

    // 1. Stop decoder task on Core 0 — no more refills in flight.
    stopDecoderTask();

    // 2. Stop producer task on Core 1 — no more reads from rings.
    stopProducerTask();

    // 3. NOW safe to destroy sources / clear channel state.
    stopAll(AudioStopMode::Immediate);

    // 4. Suspend consumer task (I²S output on Core 1).
    if (_consumerTaskHandle) {
        vTaskSuspend(_consumerTaskHandle);
    }

    MIXER_LOG("Audio suspended — Core 1 freed");
}

template<typename TI2S, typename TCodec>
void AudioMixer<TI2S, TCodec>::resumeAudio() {
    MIXER_LOG("Resuming audio (consumer + producer + decoder)...");

    // 1. Resume consumer task first (higher priority)
    if (_consumerTaskHandle) {
        vTaskResume(_consumerTaskHandle);
    }

    // 2. Restart producer task on Core 1
    startProducerTask(_producerCore, _producerPriority, _producerStackSize);

    // 3. Restart decoder task on Core 0 (Phase 6)
    startDecoderTask();

    MIXER_LOG("Audio resumed — consumer + producer + decoder restarted");
}

#endif // SFX_PLATFORM_ESP32


#endif // SFX_HAS_AUDIO
