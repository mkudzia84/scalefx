/**
 * Audio Mixer - Implementation
 * 
 * SPSC Ring Buffer Architecture:
 *   Core 0 (Producer): WAV decode + float mixing → ring buffer
 *   Core 1 (Consumer): ring buffer → I2S DMA
 * 
 * All audio math uses float, leveraging hardware FPU where available.
 * File I/O is decoupled via AudioFileOpenFn callback (injected by controller).
 *
 * Platform I2S backends:
 *   RP2040/RP2350: Arduino-Pico I2S library (per-sample write16())
 *   ESP32-S3:      ESP-IDF I2S driver (bulk i2s_channel_write())
 */

#if defined(SFX_HAS_AUDIO)

#include "audio_mixer.h"
#include "audio_config.h"
#include "audio_codec.h"
#include "audio_ring_buffer.h"
#include "audio_log.h"

// ============================================================================
//  I2S BACKEND SELECTION
// ============================================================================

#if AUDIO_MOCK_I2S
    #include "mock_i2s_sink.h"
#elif defined(ARDUINO_ARCH_RP2040)
    #include <I2S.h>
#elif defined(ARDUINO_ARCH_ESP32)
    // ESP-IDF 4.4 legacy I2S driver (v5 uses i2s_std.h but not available here)
    #include <driver/i2s.h>
    #include <driver/gpio.h>
#else
    #error "No I2S backend available for this platform"
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

#if AUDIO_MOCK_I2S
    static MockI2SSink i2sOutput;
#elif defined(ARDUINO_ARCH_RP2040)
    static I2S i2sOutput(OUTPUT);
#elif defined(ARDUINO_ARCH_ESP32)
    static bool _i2sInstalled = false;
    // Batch buffer for ESP32 bulk writes (512 stereo frames = 2 KB)
    static int16_t _i2sBatchBuf[1024];
#endif

// Reference to ring buffer singleton
static inline AudioRingBuffer& ringBuf() {
    return AudioRingBuffer::instance();
}

// ============================================================================
//  LIFECYCLE
// ============================================================================

AudioMixer::~AudioMixer() {
    shutdown();
}

bool AudioMixer::begin(uint8_t i2s_data_pin, uint8_t i2s_bclk_pin, uint8_t i2s_lrclk_pin,
                       AudioCodec* codec) {
    if (_initialized) return true;
    
    // Verify SD card is available via singleton
    if (!SD_CARD().isInitialized()) {
        MIXER_ERROR("SD card not initialized");
        return false;
    }

    _masterVolume = 1.0f;

    // Initialize all channels
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i] = Channel{};
        _channelPlaying[i] = false;
        _channelRemainingMs[i] = 0;
    }

    // Reset ring buffer
    ringBuf().reset();
    _underruns = 0;
    
    // Store I2S pins for beginI2S() (called from Core 1)
    _i2sDataPin  = i2s_data_pin;
    _i2sBclkPin  = i2s_bclk_pin;
    _i2sLrclkPin = i2s_lrclk_pin;
    
    // Initialize audio codec if provided
    _codec = codec;
    
    // Initialize command queue mutex
    sfxMutexInit(_cmdMutex);
    _cmdQueueHead = 0;
    _cmdQueueTail = 0;
    
    _initialized.store(true, std::memory_order_release);  // visible to Core 1
    
    MIXER_LOG("Phase 1 complete: %d channels, ring=%dKB, codec=%s (I2S pending on Core 1)",
             AUDIO_MAX_CHANNELS, RING_BYTES / 1024,
             _codec ? _codec->getModelName() : "none");
    return true;
}

bool AudioMixer::beginI2S() {
    // MUST be called from Core 1 — I2S library expects init and write on same core
    if (_i2sRunning) return true;
    if (!_initialized) {
        MIXER_ERROR("beginI2S() called before begin() - init order wrong");
        return false;
    }
    
    MIXER_LOG("Phase 2: configuring I2S on Core 1 (data=GP%d, bclk=GP%d, lrclk=GP%d)",
             _i2sDataPin, _i2sBclkPin, _i2sLrclkPin);

#if AUDIO_MOCK_I2S
    // Mock I2S — no real hardware
    i2sOutput.setBCLK(_i2sBclkPin);
    i2sOutput.setDATA(_i2sDataPin);
    i2sOutput.setBitsPerSample(AUDIO_BIT_DEPTH);
    if (!i2sOutput.begin(AUDIO_SAMPLE_RATE)) {
        MIXER_ERROR("Mock I2S init failed");
        return false;
    }
#elif defined(ARDUINO_ARCH_RP2040)
    // RP2040/RP2350: Arduino-Pico I2S library
    // NOTE: LRCLK is always BCLK+1 on RP2040/RP2350 PIO I2S — setBCLK implicitly
    // sets LRCLK to the next GPIO. Verify our pin assignment matches this constraint.
    if (_i2sLrclkPin != _i2sBclkPin + 1) {
        MIXER_WARN("LRCLK (GP%d) is not BCLK+1 (GP%d) — RP2 I2S PIO requires adjacent pins!",
                   _i2sLrclkPin, _i2sBclkPin);
    }
    
    i2sOutput.setBCLK(_i2sBclkPin);
    i2sOutput.setDATA(_i2sDataPin);
    i2sOutput.setBitsPerSample(AUDIO_BIT_DEPTH);
    i2sOutput.setBuffers(8, 1024);  // 8 DMA buffers × 1024 bytes = 8 KB

    if (!i2sOutput.begin(AUDIO_SAMPLE_RATE)) {
        MIXER_ERROR("I2S init failed on Core 1 (rate=%u, bits=%d, data=GP%d, bclk=GP%d)",
                    AUDIO_SAMPLE_RATE, AUDIO_BIT_DEPTH, _i2sDataPin, _i2sBclkPin);
        return false;
    }
#elif defined(ARDUINO_ARCH_ESP32)
    // ESP32-S3: Legacy I2S driver (IDF 4.4)
    i2s_config_t i2s_cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,     // 8 x 512 samples = 16 KB DMA total
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };
    
    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, nullptr);
    if (err != ESP_OK) {
        MIXER_ERROR("i2s_driver_install failed: %d", err);
        return false;
    }
    
    i2s_pin_config_t pin_cfg = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = _i2sBclkPin,
        .ws_io_num  = _i2sLrclkPin,
        .data_out_num = _i2sDataPin,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    
    err = i2s_set_pin(I2S_NUM_0, &pin_cfg);
    if (err != ESP_OK) {
        MIXER_ERROR("i2s_set_pin failed: %d", err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    
    _i2sInstalled = true;
#endif

    // Pre-fill ring buffer with silence so consumer doesn't underrun immediately.
    uint32_t preFilled = 0;
    while (ringBuf().availableWrite() > 0) {
        ringBuf().write(0, 0);
        preFilled++;
    }
    MIXER_LOG("Pre-filled ring buffer with %u frames of silence", preFilled);

    _i2sRunning.store(true, std::memory_order_release);  // visible to Core 0
    
    MIXER_LOG("Phase 2 complete: I2S %uHz/%dbit running on Core 1 (data=GP%d, bclk=GP%d)",
             AUDIO_SAMPLE_RATE, AUDIO_BIT_DEPTH, _i2sDataPin, _i2sBclkPin);
    return true;
}

void AudioMixer::shutdown() {
    if (!_initialized) return;

    // Stop all playback
    stopAll(AudioStopMode::Immediate);

    // Close all open files
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (_channels[i].wav.file) {
            _channels[i].wav.file.close();
        }
    }

    // Stop I2S — set flag BEFORE calling end() so Core 1's consume()
    // sees the flag and stops writing to i2sOutput during teardown
    if (_i2sRunning.load(std::memory_order_acquire)) {
        _i2sRunning.store(false, std::memory_order_release);
        SFX_DELAY_MS(2);  // Let Core 1 drain current consumeAndOutput() iteration

#if AUDIO_MOCK_I2S
        i2sOutput.end();
#elif defined(ARDUINO_ARCH_RP2040)
        i2sOutput.end();
#elif defined(ARDUINO_ARCH_ESP32)
        if (_i2sInstalled) {
            i2s_stop(I2S_NUM_0);
            i2s_driver_uninstall(I2S_NUM_0);
            _i2sInstalled = false;
        }
#endif
    }

    _initialized.store(false, std::memory_order_release);
    MIXER_LOG("Shutdown complete");
}

// ============================================================================
//  PAN CALCULATION
// ============================================================================

void AudioMixer::updatePan(Channel& ch) {
    // Constant-power panning: preserves perceived loudness at center
    //   pan=-1 → L=1.0, R=0.0 | pan=0 → L≈0.707, R≈0.707 | pan=+1 → L=0.0, R=1.0
    float angle = (ch.pan + 1.0f) * 3.14159265f * 0.25f;  // maps [-1,+1] → [0, π/2]
    ch.panL = cosf(angle);
    ch.panR = sinf(angle);
}

// ============================================================================
//  PLAYBACK CONTROL
// ============================================================================

bool AudioMixer::play(int channel, const char* filename, const AudioPlaybackOptions& options) {
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

    // Open the WAV file using injected file opener
    if (!_fileOpener) {
        MIXER_ERROR("Ch%d: No file opener set", channel);
        return false;
    }
    if (!_fileOpener(ws.file, filename)) {
        MIXER_ERROR("Ch%d: Failed to open: %s", channel, filename);
        return false;
    }

    // Parse WAV header
    if (!parseWavHeader(ws)) {
        MIXER_ERROR("Ch%d: Invalid WAV: %s", channel, filename);
        ws.file.close();
        return false;
    }

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
    ch.output = options.output;
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

    // Pre-fill WAV buffer
    refillWavBuffer(ws);
    
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

const char* AudioMixer::getFilename(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return nullptr;
    return _channels[channel].wav.active ? _channels[channel].filename : nullptr;
}

float AudioMixer::getChannelVolume(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0.0f;
    return _channels[channel].volume;
}

bool AudioMixer::isLooping(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return false;
    return _channels[channel].wav.loop;
}

int AudioMixer::getLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.loopCount;
}

int AudioMixer::getInitialLoopCount(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.loopCountInit;
}

AudioOutput AudioMixer::getOutput(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return AudioOutput::Stereo;
    return _channels[channel].output;
}

uint32_t AudioMixer::getSampleRate(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.sampleRate_Hz;
}

uint16_t AudioMixer::getNumChannels(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.numChannels;
}

uint16_t AudioMixer::getBitsPerSample(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.bitsPerSample;
}

uint32_t AudioMixer::getTotalSamples(int channel) const {
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return 0;
    return _channels[channel].wav.totalFrames;
}

void AudioMixer::stop(int channel, AudioStopMode mode) {
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
            if (ws.file) ws.file.close();
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

void AudioMixer::stopAll(AudioStopMode mode) {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        stop(i, mode);
    }
}

void AudioMixer::stopLooping(int channel) {
    if (!_initialized) return;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    _channels[channel].wav.loop = false;
    _channels[channel].wav.loopCount = 0;
}

void AudioMixer::stopLoopingAll() {
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        _channels[i].wav.loop = false;
        _channels[i].wav.loopCount = 0;
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

bool AudioMixer::enqueueToChannel(int channel, const char* filename, const AudioPlaybackOptions& options,
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
    if (_channels[channel].wav.loop) return -1;
    return _channelRemainingMs[channel];
}

// ============================================================================
//  WAV PARSING
// ============================================================================

bool AudioMixer::parseWavHeader(WavState& ws) {
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
//  WAV BUFFER REFILL (Producer side — Core 0)
// ============================================================================

bool AudioMixer::refillWavBuffer(WavState& ws) {
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
                    ws.active = false;
                    return ws.bufLen > 0;
                }
                ws.loopCount--;
            }
            ws.file.seek(ws.dataStart);
            ws.framesRead = 0;
            framesLeft = (int)ws.totalFrames;
        } else {
            ws.active = false;
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

    int bytesRead = ws.file.read(_sdReadBuf, bytesToRead);
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

bool AudioMixer::getWavSample(WavState& ws, float& outL, float& outR) {
    // Ensure at least 2 frames available for linear interpolation
    while (ws.bufPos + 1 >= ws.bufLen) {
        if (!refillWavBuffer(ws)) {
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
//  FLOAT MIXING PIPELINE — PRODUCER (Core 0)
// ============================================================================

bool AudioMixer::produceFrame() {
    if (ringBuf().isFull()) return false;

    float mixL = 0.0f;
    float mixR = 0.0f;

    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        Channel& ch = _channels[i];
        WavState& ws = ch.wav;
        
        if (!ws.active || ch.mute || ch.volume <= 0.0f) continue;

        float trackL = 0.0f;
        float trackR = 0.0f;

        // Get one resampled stereo frame from WAV
        if (!getWavSample(ws, trackL, trackR)) {
            // End of playback
            ws.active = false;
            _channelPlaying[i] = false;
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
                if (ws.file) ws.file.close();
                checkAndPlayNextQueued(i);
                continue;
            }
        }

        // Apply track volume
        trackL *= effectiveVolume;
        trackR *= effectiveVolume;

        // Apply output routing
        switch (ch.output) {
            case AudioOutput::Left:
                trackR = 0.0f;
                break;
            case AudioOutput::Right:
                trackL = 0.0f;
                break;
            case AudioOutput::Stereo:
            default:
                // Apply constant-power pan
                float pL = trackL * ch.panL;
                float pR = trackR * ch.panR;
                trackL = pL;
                trackR = pR;
                break;
        }

        // Accumulate into stereo mix
        mixL += trackL;
        mixR += trackR;

        // Update remaining time status
        if (ws.sampleRate_Hz > 0) {
            uint32_t framesLeft = ws.totalFrames - ws.framesRead + (ws.bufLen - ws.bufPos);
            _channelRemainingMs[i] = (int)((framesLeft * 1000UL) / ws.sampleRate_Hz);
        }
    }

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

int AudioMixer::produce(int maxFrames) {
    if (!_initialized) return 0;

    static uint32_t lastLogTime = 0;
    static int totalProduced = 0;
    
    // Process any pending commands first
    processCommands();

    // Produce frames
    int produced = 0;
    for (int i = 0; i < maxFrames; i++) {
        if (!produceFrame()) break;
        produced++;
    }
    return produced;
}

// ============================================================================
//  I2S OUTPUT — CONSUMER (Core 1)
// ============================================================================

void AudioMixer::consumeAndOutput() {
    uint32_t avail = ringBuf().availableRead();
    
    static bool firstLog = true;
    if (firstLog) {
        MIXER_LOG("Consumer first call: avail=%u init=%d i2s=%d",
                  avail, (bool)_initialized, (bool)_i2sRunning);
        firstLog = false;
    }
    
    if (avail > 0) {
#if AUDIO_MOCK_I2S || defined(ARDUINO_ARCH_RP2040)
        // Per-sample write path (Pico I2S / Mock)
        uint32_t count = (avail > 512) ? 512 : avail;
        for (uint32_t i = 0; i < count; i++) {
            StereoFrame f = ringBuf().read();
#if AUDIO_MOCK_I2S
            i2sOutput.write(f.left);
            i2sOutput.write(f.right);
#else
            i2sOutput.write16(f.left, f.right);
#endif
        }
        _consumeFrames.fetch_add(count, std::memory_order_relaxed);
#elif defined(ARDUINO_ARCH_ESP32)
        // Bulk write path (ESP32 I2S driver)
        uint32_t count = (avail > 512) ? 512 : avail;  // 512 frames = 2 KB batch
        for (uint32_t i = 0; i < count; i++) {
            StereoFrame f = ringBuf().read();
            _i2sBatchBuf[i * 2]     = f.left;
            _i2sBatchBuf[i * 2 + 1] = f.right;
        }
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, _i2sBatchBuf, count * 4, &bytesWritten, portMAX_DELAY);
        _consumeFrames.fetch_add(count, std::memory_order_relaxed);
#endif
    } else {
        // Ring buffer empty — underrun! Write silence to I2S to keep clocks
        // running cleanly (DAC needs continuous BCLK/LRCLK).
        _underruns.fetch_add(1, std::memory_order_relaxed);
#if AUDIO_MOCK_I2S
        i2sOutput.write((int16_t)0);
        i2sOutput.write((int16_t)0);
#elif defined(ARDUINO_ARCH_RP2040)
        i2sOutput.write16(0, 0);
#elif defined(ARDUINO_ARCH_ESP32)
        int16_t silence[2] = {0, 0};
        size_t bytesWritten = 0;
        i2s_write(I2S_NUM_0, silence, 4, &bytesWritten, portMAX_DELAY);
#endif
    }
}

void AudioMixer::consume() {
    _consumeLoops.fetch_add(1, std::memory_order_relaxed);
    if (!_initialized || !_i2sRunning) return;
    consumeAndOutput();
}

void AudioMixer::process() {
    // Legacy single-call: does both produce and consume
    // For optimal performance, call produce() and consume() separately
    // from their respective cores
    produce(256);
    consume();
}

// ============================================================================
//  RING BUFFER STATS
// ============================================================================

uint32_t AudioMixer::getRingAvailableRead() const {
    return ringBuf().availableRead();
}

uint32_t AudioMixer::getRingAvailableWrite() const {
    return ringBuf().availableWrite();
}

int AudioMixer::getRingFillPercent() const {
    return ringBuf().fillPercent();
}

// ============================================================================
//  DUAL-CORE COMMAND QUEUE
// ============================================================================

bool AudioMixer::queueCommand(const Command& cmd) {
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

void AudioMixer::processCommands() {
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

void AudioMixer::executeCommand(const Command& cmd) {
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
//  ASYNC API (Safe to call from any core — uses command queue)
// ============================================================================

bool AudioMixer::playAsync(int channel, const char* filename, const AudioPlaybackOptions& options) {
    Command cmd{};
    cmd.type = CommandType::Play;
    cmd.channelId = channel;
    strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
    cmd.options = options;
    return queueCommand(cmd);
}

void AudioMixer::stopAsync(int channel, AudioStopMode mode) {
    Command cmd{};
    cmd.type = (channel < 0) ? CommandType::StopAll : CommandType::Stop;
    cmd.channelId = channel;
    cmd.stopMode = mode;
    queueCommand(cmd);
}

void AudioMixer::setVolumeAsync(int channel, float vol) {
    Command cmd{};
    cmd.type = CommandType::SetVolume;
    cmd.channelId = channel;
    cmd.volume = vol;
    queueCommand(cmd);
}

void AudioMixer::setMasterVolumeAsync(float vol) {
    Command cmd{};
    cmd.type = CommandType::SetMasterVolume;
    cmd.volume = vol;
    queueCommand(cmd);
}

bool AudioMixer::queueSoundAsync(int channel, const char* filename, const AudioPlaybackOptions& options,
                                  QueueLoopBehavior loopBehavior) {
    Command cmd{};
    cmd.type = CommandType::QueueSound;
    cmd.channelId = channel;
    strncpy(cmd.filename, filename, sizeof(cmd.filename) - 1);
    cmd.options = options;
    cmd.loopBehavior = loopBehavior;
    return queueCommand(cmd);
}

void AudioMixer::clearQueueAsync(int channel) {
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

#endif // SFX_HAS_AUDIO
