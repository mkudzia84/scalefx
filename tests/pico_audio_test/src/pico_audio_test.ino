/*
 * Pico 2 Audio Test — WAV File Mixer with Float Pipeline (Dual-Core)
 *
 * Two-track audio mixer for RP2350 + Waveshare Pico-Audio (PCM5101A DAC).
 * Supports two source types per track:
 *   - Sine generators (sinf on hardware FPU)
 *   - WAV file playback from SD card (with linear-interpolation resampling)
 *
 * All audio math uses float, leveraging the Cortex-M33 hardware FPU.
 * WAV samples are converted to float for mixing; int16 conversion happens
 * only at the final output stage. WAV files at non-native sample rates
 * are resampled via linear interpolation.
 *
 * DUAL-CORE ARCHITECTURE:
 *   Core 0 (Producer): WAV decode + sinf generation + float mixing
 *                       → SPSC ring buffer. Also handles serial UI.
 *   Core 1 (Consumer): SPSC ring buffer → I2S DMA → PCM5101A DAC.
 *
 * Controls (via Serial @ 115200):
 *   1-9              Presets (sine generators)
 *   load<1-2> <path> Load WAV to track    e.g. load1 /sounds/KA50/engine_loop.wav
 *   stop<1-2>        Stop WAV playback    e.g. stop1
 *   loop<1-2>        Toggle WAV looping   e.g. loop1
 *   v<1-2> <0-100>   Track volume %       e.g. v1 80
 *   m<1-2>           Toggle track mute    e.g. m1
 *   p<1-2> <-100..100> Track pan L/R      e.g. p1 -50
 *   f<1-4> <hz>      Generator frequency  e.g. f1 440
 *   a<1-4> <0-100>   Generator amplitude  e.g. a1 50
 *   mv <0-100>       Master volume %      e.g. mv 80
 *   sd               SD card info
 *   ls <path>        List SD directory    e.g. ls /sounds
 *   0                Status
 *   h                Help
 *
 * Hardware:
 *   - Waveshare Pico-Audio: PCM5101A DAC, 3-wire I2S (no MCLK)
 *   - DIN = GP26, BCK = GP27, LRCLK = GP28
 *   - SD Card module on SPI0: CS=GP17, SCK=GP18, MOSI=GP19, MISO=GP16
 */

#include <I2S.h>
#include <SPI.h>
#include <SdFat.h>
#include <math.h>

// ============================================================================
// Pin Configuration
// ============================================================================

// I2S Audio (Waveshare Pico-Audio — PCM5101A)
static constexpr int I2S_PIN_DATA = 26;   // DIN  → GP26
static constexpr int I2S_PIN_BCLK = 27;   // BCK  → GP27
static constexpr int I2S_PIN_WS   = 28;   // LRCK → GP28

// SD Card SPI (same pins as HubFX)
static constexpr int PIN_SD_CS   = 17;    // Chip Select → GP17
static constexpr int PIN_SD_SCK  = 18;    // SPI Clock   → GP18
static constexpr int PIN_SD_MOSI = 19;    // Master Out  → GP19
static constexpr int PIN_SD_MISO = 16;    // Master In   → GP16

// ============================================================================
// Audio Configuration
// ============================================================================

static constexpr int SAMPLE_RATE     = 48000;
static constexpr int BITS_PER_SAMPLE = 16;
static constexpr int NUM_CHANNELS    = 2;   // stereo output

// ============================================================================
// Track & Generator Configuration
// ============================================================================

static constexpr int     NUM_TRACKS      = 2;     // mixer tracks
static constexpr int     GENS_PER_TRACK  = 2;     // sine generators per track
static constexpr int     TOTAL_GENS      = NUM_TRACKS * GENS_PER_TRACK;  // 4
static constexpr int16_t MAX_AMPLITUDE   = 24000;  // ~73% of 32767 (headroom)
static constexpr float   TWO_PI_F        = 2.0f * (float)M_PI;

// WAV playback buffer sizes
static constexpr int WAV_BUF_FRAMES    = 1024;   // decoded float frames per track
static constexpr int WAV_SD_READ_BYTES = 4096;   // bytes per SD card read batch

// ============================================================================
// SPSC Ring Buffer
// ============================================================================
//
// Lock-free single-producer single-consumer ring buffer for stereo int16
// sample frames. Core 0 writes, Core 1 reads.
//
// Each "frame" = 1 left sample + 1 right sample = 4 bytes.
// Buffer size must be a power of 2 for fast modulo via bitmask.

static constexpr int RING_FRAMES_LOG2 = 14;                          // 16384 frames
static constexpr int RING_FRAMES      = 1 << RING_FRAMES_LOG2;       // 16384
static constexpr int RING_MASK        = RING_FRAMES - 1;             // 0x3FFF
static constexpr int RING_BYTES       = RING_FRAMES * NUM_CHANNELS * sizeof(int16_t);  // 64 KB

// Sample storage — each entry is a stereo frame (L, R)
struct StereoFrame {
    int16_t left;
    int16_t right;
};

static StereoFrame g_ringBuf[RING_FRAMES];

// SPSC indices — only written by one core each
static volatile uint32_t g_writeIdx = 0;  // Core 0 writes, Core 1 reads
static volatile uint32_t g_readIdx  = 0;  // Core 1 writes, Core 0 reads

static inline uint32_t ringAvailableRead() {
    uint32_t w = __atomic_load_n(&g_writeIdx, __ATOMIC_ACQUIRE);
    uint32_t r = g_readIdx;
    return (w - r);  // works with wrapping unsigned arithmetic
}

static inline uint32_t ringAvailableWrite() {
    uint32_t w = g_writeIdx;
    uint32_t r = __atomic_load_n(&g_readIdx, __ATOMIC_ACQUIRE);
    return RING_FRAMES - (w - r);
}

static inline void ringWrite(int16_t left, int16_t right) {
    uint32_t idx = g_writeIdx & RING_MASK;
    g_ringBuf[idx].left  = left;
    g_ringBuf[idx].right = right;
    __atomic_store_n(&g_writeIdx, g_writeIdx + 1, __ATOMIC_RELEASE);
}

static inline StereoFrame ringRead() {
    uint32_t idx = g_readIdx & RING_MASK;
    StereoFrame f = g_ringBuf[idx];
    __atomic_store_n(&g_readIdx, g_readIdx + 1, __ATOMIC_RELEASE);
    return f;
}

// ============================================================================
// I2S Output (Core 1)
// ============================================================================

static I2S i2sOut(OUTPUT);

// ============================================================================
// SD Card
// ============================================================================

static SdFat32 g_sd;
static bool    g_sdOk = false;
static uint8_t g_sdReadBuf[WAV_SD_READ_BYTES];  // shared temp buffer for SD reads

// ============================================================================
// Track-based Mixer (Core 0 only)
// ============================================================================
//
// Each track can be driven by either sine generators or WAV file playback.
// All audio math uses float (hardware FPU). Conversion to int16 happens
// only at the final output stage.

enum TrackSource : uint8_t {
    SOURCE_GENERATORS = 0,   // sine wave generators
    SOURCE_WAV        = 1    // WAV file from SD card
};

struct Generator {
    float freqHz;
    float amplitude;      // 0.0 .. 1.0
    float phase;          // 0.0 .. 2π, wraps
    float phaseInc;       // pre-computed: 2π * freq / sampleRate
    bool  active;
};

struct WavState {
    File32   file;
    bool     active         = false;
    bool     loop           = true;

    // WAV format (from header)
    uint32_t sampleRate_Hz  = 0;
    uint16_t numChannels    = 0;
    uint16_t bitsPerSample  = 0;
    uint32_t dataStart      = 0;     // byte offset of PCM data in file
    uint32_t totalFrames    = 0;     // total sample frames
    uint32_t framesRead     = 0;     // frames consumed from file so far
    char     filename[80]   = {};

    // Decoded float buffer for mixing
    float bufL[WAV_BUF_FRAMES];
    float bufR[WAV_BUF_FRAMES];
    int   bufLen            = 0;     // valid frames in buffer
    int   bufPos            = 0;     // next frame to consume

    // Linear interpolation resampler
    float resampleRatio     = 1.0f;  // srcRate / outputRate
    float resampleFrac      = 0.0f;  // fractional sample position
};

struct Track {
    TrackSource source;
    Generator   gen[GENS_PER_TRACK];
    WavState    wav;
    float       volume;     // 0.0 .. 1.0
    float       pan;        // -1.0 (full left) .. 0.0 (center) .. +1.0 (full right)
    float       panL;       // pre-computed left gain (constant-power pan)
    float       panR;       // pre-computed right gain (constant-power pan)
    bool        mute;
};

static Track g_tracks[NUM_TRACKS];
static float g_masterVol = 1.0f;    // 0.0 .. 1.0

// --- Generator helpers ---

static void updatePhaseInc(Generator& gen) {
    gen.phaseInc = TWO_PI_F * gen.freqHz / (float)SAMPLE_RATE;
}

static void updatePan(Track& trk) {
    // Constant-power panning: preserves perceived loudness at center
    //   pan=-1 → L=1.0, R=0.0 | pan=0 → L≈0.707, R≈0.707 | pan=+1 → L=0.0, R=1.0
    float angle = (trk.pan + 1.0f) * (float)M_PI * 0.25f;  // maps [-1,+1] → [0, π/2]
    trk.panL = cosf(angle);
    trk.panR = sinf(angle);
}

// Set generator by flat index (0-3 internal, maps to track/gen)
// Automatically switches the track source to generators.
static void setGen(int flatIdx, float freq, float amp) {
    int t = flatIdx / GENS_PER_TRACK;
    int g = flatIdx % GENS_PER_TRACK;
    Generator& gen = g_tracks[t].gen[g];
    gen.freqHz    = freq;
    gen.amplitude = (amp < 0.0f) ? 0.0f : (amp > 1.0f) ? 1.0f : amp;
    gen.active    = (freq > 0.0f && gen.amplitude > 0.0f);
    updatePhaseInc(gen);
    g_tracks[t].source = SOURCE_GENERATORS;
}

static float getGenAmplitude(int flatIdx) {
    return g_tracks[flatIdx / GENS_PER_TRACK].gen[flatIdx % GENS_PER_TRACK].amplitude;
}

static float getGenFreqHz(int flatIdx) {
    return g_tracks[flatIdx / GENS_PER_TRACK].gen[flatIdx % GENS_PER_TRACK].freqHz;
}

static void setTrackVolume(int t, float vol) {
    g_tracks[t].volume = (vol < 0.0f) ? 0.0f : (vol > 1.0f) ? 1.0f : vol;
}

static void setTrackPan(int t, float pan) {
    g_tracks[t].pan = (pan < -1.0f) ? -1.0f : (pan > 1.0f) ? 1.0f : pan;
    updatePan(g_tracks[t]);
}

// --- WAV helpers ---

static void stopWav(int t) {
    WavState& ws = g_tracks[t].wav;
    if (ws.file.isOpen()) ws.file.close();
    ws.active       = false;
    ws.bufLen       = 0;
    ws.bufPos       = 0;
    ws.framesRead   = 0;
    ws.resampleFrac = 0.0f;
    ws.filename[0]  = '\0';
}

static void silenceAll() {
    for (int t = 0; t < NUM_TRACKS; t++) {
        g_tracks[t].source = SOURCE_GENERATORS;
        for (int g = 0; g < GENS_PER_TRACK; g++) {
            Generator& gen = g_tracks[t].gen[g];
            gen.freqHz    = 0;
            gen.amplitude = 0;
            gen.phase     = 0;
            gen.phaseInc  = 0;
            gen.active    = false;
        }
        stopWav(t);
        g_tracks[t].volume = 1.0f;
        g_tracks[t].pan    = 0.0f;
        g_tracks[t].mute   = false;
        updatePan(g_tracks[t]);
    }
    g_masterVol = 1.0f;
}

// ============================================================================
// Presets
// ============================================================================

static void applyPreset(uint8_t preset) {
    silenceAll();

    switch (preset) {
        case 1:  // Track 1: A3 (220 Hz)
            setGen(0, 220.0f, 1.0f);
            break;

        case 2:  // Track 1: A4 (440 Hz)
            setGen(0, 440.0f, 1.0f);
            break;

        case 3:  // Two tracks: T1=A3 (220), T2=E4 (330)
            setGen(0, 220.0f, 1.0f);     // Track 1, Gen 1
            setGen(2, 329.6f, 1.0f);     // Track 2, Gen 1
            break;

        case 4:  // Track 1: 1 kHz test tone
            setGen(0, 1000.0f, 0.8f);
            break;

        case 5:  // Harmonics split: T1 (110+220), T2 (440+880)
            setGen(0, 110.0f, 0.7f);
            setGen(1, 220.0f, 0.5f);
            setGen(2, 440.0f, 0.4f);
            setGen(3, 880.0f, 0.25f);
            setTrackVolume(0, 0.8f);
            setTrackVolume(1, 0.6f);
            break;

        case 6:  // Beating: T1=220 Hz, T2=223 Hz
            setGen(0, 220.0f, 1.0f);
            setGen(2, 223.0f, 1.0f);
            break;

        case 7:  // C major split: T1 (C3+E3), T2 (G3)
            setGen(0, 130.8f, 0.6f);
            setGen(1, 164.8f, 0.6f);
            setGen(2, 196.0f, 0.8f);
            break;

        case 8:  // Stereo demo: T1 bass panned left, T2 tone panned right
            setGen(0, 55.0f, 0.7f);
            setGen(1, 110.0f, 0.5f);
            setGen(2, 440.0f, 0.8f);
            setTrackPan(0, -0.7f);       // T1 left
            setTrackPan(1,  0.7f);       // T2 right
            break;

        case 9:  // Silence
        default:
            break;
    }
}

// ============================================================================
// SD Card Init
// ============================================================================

static bool initSdCard() {
    SPI.setCS(PIN_SD_CS);
    SPI.setSCK(PIN_SD_SCK);
    SPI.setTX(PIN_SD_MOSI);
    SPI.setRX(PIN_SD_MISO);

    // Try descending SPI speeds
    const uint8_t speeds[] = {20, 15, 10, 5};
    for (uint8_t i = 0; i < sizeof(speeds); i++) {
        if (g_sd.begin(PIN_SD_CS, SD_SCK_MHZ(speeds[i]))) {
            Serial.printf("SD card OK at %d MHz SPI\n", speeds[i]);
            return true;
        }
        delay(100);
    }
    Serial.println(F("SD card init FAILED"));
    return false;
}

// ============================================================================
// WAV File Operations
// ============================================================================

static bool parseWavHeader(WavState& ws) {
    uint8_t header[44];
    if (ws.file.read(header, 44) != 44) return false;

    // Validate RIFF/WAVE/fmt
    if (memcmp(header, "RIFF", 4) != 0) return false;
    if (memcmp(header + 8, "WAVE", 4) != 0) return false;
    if (memcmp(header + 12, "fmt ", 4) != 0) return false;

    // PCM only (audioFormat == 1)
    uint16_t audioFormat = header[20] | (header[21] << 8);
    if (audioFormat != 1) return false;

    ws.numChannels   = header[22] | (header[23] << 8);
    ws.sampleRate_Hz = header[24] | (header[25] << 8) |
                       (header[26] << 16) | (header[27] << 24);
    ws.bitsPerSample = header[34] | (header[35] << 8);

    if (ws.numChannels < 1 || ws.numChannels > 2) return false;
    if (ws.bitsPerSample != 8 && ws.bitsPerSample != 16) return false;

    // Find data chunk (may not be at fixed offset in all WAV files)
    ws.file.seek(12);
    while (ws.file.available()) {
        uint8_t chunkHdr[8];
        if (ws.file.read(chunkHdr, 8) != 8) return false;
        uint32_t chunkSize = chunkHdr[4] | (chunkHdr[5] << 8) |
                             (chunkHdr[6] << 16) | (chunkHdr[7] << 24);
        if (memcmp(chunkHdr, "data", 4) == 0) {
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

// Refill WAV float buffer from SD card. Returns false if no data available.
static bool refillWavBuffer(WavState& ws) {
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

    int bytesRead = ws.file.read(g_sdReadBuf, bytesToRead);
    int framesGot = bytesRead / bytesPerFrame;
    ws.framesRead += framesGot;

    // Convert raw PCM to float [-1.0, +1.0]
    for (int i = 0; i < framesGot; i++) {
        float l, r;
        if (ws.bitsPerSample == 16) {
            const int16_t* buf16 = (const int16_t*)g_sdReadBuf;
            if (ws.numChannels == 2) {
                l = buf16[i * 2]     * (1.0f / 32768.0f);
                r = buf16[i * 2 + 1] * (1.0f / 32768.0f);
            } else {
                l = r = buf16[i] * (1.0f / 32768.0f);
            }
        } else {  // 8-bit unsigned
            if (ws.numChannels == 2) {
                l = (g_sdReadBuf[i * 2]     - 128) * (1.0f / 128.0f);
                r = (g_sdReadBuf[i * 2 + 1] - 128) * (1.0f / 128.0f);
            } else {
                l = r = (g_sdReadBuf[i] - 128) * (1.0f / 128.0f);
            }
        }
        ws.bufL[remaining + i] = l;
        ws.bufR[remaining + i] = r;
    }
    ws.bufLen = remaining + framesGot;
    return ws.bufLen > 0;
}

// Get one resampled stereo float frame from a WAV track.
// Uses linear interpolation for sample rate conversion.
// Returns false if no more data (non-looping, reached end).
static bool getWavSample(WavState& ws, float& outL, float& outR) {
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

// Load a WAV file from SD card into a track for playback
static bool loadWav(int t, const char* path) {
    if (!g_sdOk) {
        Serial.println(F("ERROR: SD card not initialized"));
        return false;
    }

    stopWav(t);
    WavState& ws = g_tracks[t].wav;

    if (!ws.file.open(&g_sd, path, O_RDONLY)) {
        Serial.printf("ERROR: Cannot open %s\n", path);
        return false;
    }

    if (!parseWavHeader(ws)) {
        Serial.printf("ERROR: Invalid WAV format: %s\n", path);
        ws.file.close();
        return false;
    }

    // Configure resampler
    ws.resampleRatio = (float)ws.sampleRate_Hz / (float)SAMPLE_RATE;
    ws.resampleFrac  = 0.0f;
    ws.bufLen        = 0;
    ws.bufPos        = 0;
    ws.active        = true;
    strncpy(ws.filename, path, sizeof(ws.filename) - 1);
    ws.filename[sizeof(ws.filename) - 1] = '\0';

    // Pre-fill buffer
    refillWavBuffer(ws);

    g_tracks[t].source = SOURCE_WAV;

    float duration_s = (float)ws.totalFrames / (float)ws.sampleRate_Hz;
    Serial.printf("Track %d: loaded %s\n", t + 1, path);
    Serial.printf("  WAV: %luHz %d-bit %s, %lu frames (%.1fs)\n",
        (unsigned long)ws.sampleRate_Hz,
        ws.bitsPerSample,
        ws.numChannels == 2 ? "stereo" : "mono",
        (unsigned long)ws.totalFrames,
        (double)duration_s);
    if (ws.resampleRatio != 1.0f) {
        Serial.printf("  Resample: ratio=%.4f (%luHz -> %dHz)\n",
            (double)ws.resampleRatio,
            (unsigned long)ws.sampleRate_Hz,
            SAMPLE_RATE);
    }

    return true;
}

// ============================================================================
// Core 0 — Float Mixing Pipeline (Producer)
// ============================================================================

// Generate one stereo frame using the float mixing pipeline.
// Handles both generator and WAV sources per track.
// All arithmetic stays in float until the final int16 conversion.
// Returns true if a frame was produced, false if buffer full.
static bool produceFrame() {
    if (ringAvailableWrite() == 0) return false;

    float mixL = 0.0f;
    float mixR = 0.0f;

    for (int t = 0; t < NUM_TRACKS; t++) {
        Track& trk = g_tracks[t];
        if (trk.mute || trk.volume <= 0.0f) continue;

        float trackL = 0.0f;
        float trackR = 0.0f;

        if (trk.source == SOURCE_WAV && trk.wav.active) {
            // --- WAV source: get one resampled stereo frame ---
            getWavSample(trk.wav, trackL, trackR);

        } else if (trk.source == SOURCE_GENERATORS) {
            // --- Generator source: sum sinf() generators (mono) ---
            float mono = 0.0f;
            for (int g = 0; g < GENS_PER_TRACK; g++) {
                Generator& gen = trk.gen[g];
                if (!gen.active) continue;
                mono += sinf(gen.phase) * gen.amplitude;
                gen.phase += gen.phaseInc;
                if (gen.phase >= TWO_PI_F) gen.phase -= TWO_PI_F;
            }
            trackL = mono;
            trackR = mono;
        }

        // Apply track volume
        trackL *= trk.volume;
        trackR *= trk.volume;

        // Apply constant-power pan and accumulate into stereo mix
        mixL += trackL * trk.panL;
        mixR += trackR * trk.panR;
    }

    // Apply master volume
    mixL *= g_masterVol;
    mixR *= g_masterVol;

    // Clamp to [-1.0, +1.0] (float domain — no integer wrap)
    if (mixL >  1.0f) mixL =  1.0f;
    if (mixL < -1.0f) mixL = -1.0f;
    if (mixR >  1.0f) mixR =  1.0f;
    if (mixR < -1.0f) mixR = -1.0f;

    // Convert to int16 for ring buffer
    int16_t sampleL = (int16_t)(mixL * MAX_AMPLITUDE);
    int16_t sampleR = (int16_t)(mixR * MAX_AMPLITUDE);

    ringWrite(sampleL, sampleR);
    return true;
}

// Produce a batch of frames (up to `count`). Returns number produced.
static int produceBatch(int count) {
    int produced = 0;
    for (int i = 0; i < count; i++) {
        if (!produceFrame()) break;
        produced++;
    }
    return produced;
}

// ============================================================================
// Synchronisation & Stats
// ============================================================================

static volatile bool     g_core1Ready    = false;
static volatile uint32_t g_consumeLoops  = 0;
static volatile uint32_t g_produceLoops  = 0;
static volatile uint32_t g_underruns     = 0;   // Core 1 ran out of data

// ============================================================================
// Core 0 — Serial Command Interface
// ============================================================================

static void printHelp() {
    Serial.println(F("\n=== Pico 2 Audio Test - WAV Mixer ==="));
    Serial.println(F("Presets (sine generators): 1-9"));
    Serial.println(F("  1  A3 (220 Hz)              5  Harmonics split"));
    Serial.println(F("  2  A4 (440 Hz)              6  Beating (220+223)"));
    Serial.println(F("  3  Two-track (A3+E4)        7  C major chord"));
    Serial.println(F("  4  1 kHz test tone          8  Stereo pan demo"));
    Serial.println(F("  9  Silence"));
    Serial.println(F("WAV playback:"));
    Serial.println(F("  load<1-2> <path>    Load WAV file    e.g. load1 /sounds/KA50/engine_loop.wav"));
    Serial.println(F("  stop<1-2>           Stop playback    e.g. stop1"));
    Serial.println(F("  loop<1-2>           Toggle looping   e.g. loop1"));
    Serial.println(F("Track controls:"));
    Serial.println(F("  v<1-2> <0-100>      Volume %         e.g. v1 80"));
    Serial.println(F("  m<1-2>              Toggle mute      e.g. m1"));
    Serial.println(F("  p<1-2> <-100..100>  Pan L/R          e.g. p1 -50"));
    Serial.println(F("Generator controls (gen 1-2 = T1, gen 3-4 = T2):"));
    Serial.println(F("  f<1-4> <hz>         Frequency        e.g. f1 440"));
    Serial.println(F("  a<1-4> <0-100>      Amplitude %      e.g. a1 50"));
    Serial.println(F("Master:"));
    Serial.println(F("  mv <0-100>          Master vol %     e.g. mv 80"));
    Serial.println(F("SD Card:"));
    Serial.println(F("  sd                  SD card info"));
    Serial.println(F("  ls <path>           List directory   e.g. ls /sounds"));
    Serial.println(F("Info:"));
    Serial.println(F("  0  Status    h  Help\n"));
}

static void printStatus() {
    Serial.printf("--- Mixer Status (master vol: %d%%) ---\n",
        (int)(g_masterVol * 100.0f));
    for (int t = 0; t < NUM_TRACKS; t++) {
        Track& trk = g_tracks[t];
        const char* srcStr = (trk.source == SOURCE_WAV) ? "WAV" : "GEN";
        Serial.printf("Track %d [%s]: vol=%3d%%  pan=%+4d%%  %s\n",
            t + 1, srcStr,
            (int)(trk.volume * 100.0f),
            (int)(trk.pan * 100.0f),
            trk.mute ? "MUTED" : "active");

        if (trk.source == SOURCE_WAV) {
            WavState& ws = trk.wav;
            if (ws.active || ws.filename[0]) {
                float pos_s = (float)ws.framesRead / (float)ws.sampleRate_Hz;
                float dur_s = (float)ws.totalFrames / (float)ws.sampleRate_Hz;
                Serial.printf("  WAV: %s %s\n", ws.filename,
                    ws.active ? (ws.loop ? "(looping)" : "(playing)") : "(stopped)");
                Serial.printf("       %luHz %d-bit %s  %.1f/%.1fs  resample=%.4f\n",
                    (unsigned long)ws.sampleRate_Hz, ws.bitsPerSample,
                    ws.numChannels == 2 ? "st" : "mo",
                    (double)pos_s, (double)dur_s,
                    (double)ws.resampleRatio);
            }
        } else {
            for (int g = 0; g < GENS_PER_TRACK; g++) {
                Generator& gen = trk.gen[g];
                Serial.printf("  Gen %d: %7.1f Hz  amp=%3d%%  %s\n",
                    g + 1,
                    (double)gen.freqHz,
                    (int)(gen.amplitude * 100.0f),
                    gen.active ? "ON" : "off");
            }
        }
    }
    uint32_t fill = ringAvailableRead();
    Serial.printf("Ring buffer: %lu / %d frames (%lu%%)\n",
        (unsigned long)fill, RING_FRAMES, (unsigned long)(fill * 100 / RING_FRAMES));
    Serial.printf("Core 0 produce: %lu/sec  Core 1 consume: %lu/sec\n",
        (unsigned long)g_produceLoops, (unsigned long)g_consumeLoops);
    Serial.printf("Underruns: %lu  SD: %s\n",
        (unsigned long)g_underruns, g_sdOk ? "OK" : "FAIL");
    Serial.printf("Free RAM: %lu bytes\n", (unsigned long)rp2040.getFreeHeap());
}

static bool parseGenCmd(const String& line, char prefix) {
    if (line.length() < 3 || line[0] != prefix) return false;
    char genChar = line[1];
    if (genChar < '1' || genChar > '4') return false;
    int spaceIdx = line.indexOf(' ');
    if (spaceIdx < 0) return false;

    int flatIdx = genChar - '1';
    float val = line.substring(spaceIdx + 1).toFloat();

    if (prefix == 'f') {
        setGen(flatIdx, val, getGenAmplitude(flatIdx));
    } else {  // 'a'
        setGen(flatIdx, getGenFreqHz(flatIdx), val / 100.0f);
    }

    int trackNum = flatIdx / GENS_PER_TRACK + 1;
    int genNum   = flatIdx % GENS_PER_TRACK + 1;
    const char* label = (prefix == 'f') ? "Freq" : "Amp";
    Serial.printf("T%d.Gen%d %s: %.1f\n", trackNum, genNum, label, (double)val);
    return true;
}

static bool parseTrackCmd(const String& line, char prefix) {
    if (line.length() < 3 || line[0] != prefix) return false;
    char trkChar = line[1];
    if (trkChar < '1' || trkChar > '2') return false;
    int spaceIdx = line.indexOf(' ');
    if (spaceIdx < 0) return false;

    int t = trkChar - '1';
    float val = line.substring(spaceIdx + 1).toFloat();

    if (prefix == 'v') {
        setTrackVolume(t, val / 100.0f);
        Serial.printf("Track %d vol: %d%%\n", t + 1, (int)(g_tracks[t].volume * 100.0f));
    } else if (prefix == 'p') {
        setTrackPan(t, val / 100.0f);
        Serial.printf("Track %d pan: %+d%%\n", t + 1, (int)(g_tracks[t].pan * 100.0f));
    }
    return true;
}

static void cmdSdInfo() {
    if (!g_sdOk) {
        Serial.println(F("SD card not initialized"));
        return;
    }
    uint32_t cardSize = g_sd.card()->sectorCount();
    Serial.printf("SD card: %lu MB\n", (unsigned long)(cardSize / 2048));
}

static void cmdListDir(const char* path) {
    if (!g_sdOk) {
        Serial.println(F("SD card not initialized"));
        return;
    }
    File32 dir;
    if (!dir.open(&g_sd, path, O_RDONLY) || !dir.isDir()) {
        Serial.printf("Cannot open directory: %s\n", path);
        if (dir.isOpen()) dir.close();
        return;
    }
    Serial.printf("Contents of %s:\n", path);
    File32 entry;
    while (entry.openNext(&dir, O_RDONLY)) {
        char name[64];
        entry.getName(name, sizeof(name));
        if (entry.isDir()) {
            Serial.printf("  [DIR]  %s\n", name);
        } else {
            Serial.printf("  %7lu  %s\n", (unsigned long)entry.fileSize(), name);
        }
        entry.close();
    }
    dir.close();
}

static String g_serialBuf;

static void handleSerial() {
    // Non-blocking character-by-character read to avoid stalling Core 0
    bool gotNewline = false;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (g_serialBuf.length() == 0) continue;  // skip empty lines
            gotNewline = true;
            break;
        }
        g_serialBuf += c;
        if (g_serialBuf.length() > 128) {  // safety limit (long WAV paths)
            g_serialBuf = "";
            return;
        }
    }
    if (g_serialBuf.length() == 0) return;
    if (!gotNewline) {
        // Still accumulating — but process single chars immediately (presets)
        if (g_serialBuf.length() > 1) return;
    }

    String line = g_serialBuf;
    g_serialBuf = "";
    line.trim();
    if (line.length() == 0) return;

    // Single-char commands: presets 1-9, status, help
    if (line.length() == 1) {
        char c = line[0];
        if (c >= '1' && c <= '9') {
            applyPreset(c - '0');
            Serial.printf("Preset %c\n", c);
            return;
        }
        if (c == '0') { printStatus(); return; }
        if (c == 'h' || c == 'H') { printHelp(); return; }
    }

    // WAV load: load1 <path>, load2 <path>
    if (line.startsWith("load1 ")) {
        loadWav(0, line.c_str() + 6);
        return;
    }
    if (line.startsWith("load2 ")) {
        loadWav(1, line.c_str() + 6);
        return;
    }

    // WAV stop: stop1, stop2
    if (line == "stop1") { stopWav(0); g_tracks[0].source = SOURCE_GENERATORS; Serial.println(F("Track 1 WAV stopped")); return; }
    if (line == "stop2") { stopWav(1); g_tracks[1].source = SOURCE_GENERATORS; Serial.println(F("Track 2 WAV stopped")); return; }

    // WAV loop toggle: loop1, loop2
    if (line == "loop1") { g_tracks[0].wav.loop = !g_tracks[0].wav.loop; Serial.printf("Track 1 loop: %s\n", g_tracks[0].wav.loop ? "ON" : "OFF"); return; }
    if (line == "loop2") { g_tracks[1].wav.loop = !g_tracks[1].wav.loop; Serial.printf("Track 2 loop: %s\n", g_tracks[1].wav.loop ? "ON" : "OFF"); return; }

    // Two-char commands: m1, m2 (toggle track mute)
    if (line.length() == 2 && line[0] == 'm' && line[1] >= '1' && line[1] <= '2') {
        int t = line[1] - '1';
        g_tracks[t].mute = !g_tracks[t].mute;
        Serial.printf("Track %d %s\n", t + 1, g_tracks[t].mute ? "MUTED" : "unmuted");
        return;
    }

    // Master volume: mv <0-100>
    if (line.startsWith("mv ")) {
        float val = line.substring(3).toFloat();
        g_masterVol = val / 100.0f;
        if (g_masterVol < 0.0f) g_masterVol = 0.0f;
        if (g_masterVol > 1.0f) g_masterVol = 1.0f;
        Serial.printf("Master vol: %d%%\n", (int)(g_masterVol * 100.0f));
        return;
    }

    // Generator commands: f<1-4> <hz>, a<1-4> <0-100>
    if (parseGenCmd(line, 'f')) return;
    if (parseGenCmd(line, 'a')) return;

    // Track commands: v<1-2> <0-100>, p<1-2> <-100..100>
    if (parseTrackCmd(line, 'v')) return;
    if (parseTrackCmd(line, 'p')) return;

    // SD card commands
    if (line == "sd") { cmdSdInfo(); return; }
    if (line.startsWith("ls ")) { cmdListDir(line.c_str() + 3); return; }
    if (line == "ls")          { cmdListDir("/"); return; }

    Serial.println(F("Unknown command. Type 'h' for help."));
}

// ============================================================================
// Core 0 — setup / loop (PRODUCER)
// ============================================================================

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(10);  // Prevent blocking reads
    delay(1500);

    Serial.println(F("\n+=============================================+"));
    Serial.println(F("|   Pico 2 Audio Test - WAV Mixer            |"));
    Serial.println(F("|   RP2350 + Waveshare Pico-Audio + SD       |"));
    Serial.println(F("+=============================================+"));
    Serial.printf("CPU: %lu MHz  RAM: %lu bytes free\n",
        (unsigned long)(rp2040.f_cpu() / 1000000),
        (unsigned long)rp2040.getFreeHeap());
    Serial.printf("Ring buffer: %d frames (%d KB)\n",
        RING_FRAMES, RING_BYTES / 1024);
    Serial.printf("Tracks: %d x %d generators, float mixing pipeline\n",
        NUM_TRACKS, GENS_PER_TRACK);

    // Initialize SD card
    g_sdOk = initSdCard();

    // Start silent
    silenceAll();

    // Wait for Core 1 I2S init
    Serial.println(F("Waiting for Core 1 I2S init..."));
    while (!g_core1Ready) {
        delay(10);
    }
    Serial.println(F("Core 1 ready — pipeline active (silent)"));

    // Pre-fill ring buffer with silence
    for (int i = 0; i < RING_FRAMES; i++) {
        ringWrite(0, 0);
    }
    Serial.printf("Ring buffer pre-filled (%lu frames)\n", (unsigned long)ringAvailableRead());

    printHelp();
}

void loop() {
    handleSerial();

    // Produce as many frames as the ring buffer can accept
    // Batch size: ~2048 frames per iteration keeps latency low
    produceBatch(2048);

    // Performance counter
    static uint32_t prodCount = 0;
    static uint32_t lastProd_ms = 0;
    prodCount++;
    uint32_t now = millis();
    if (now - lastProd_ms >= 1000) {
        g_produceLoops = prodCount;
        prodCount = 0;
        lastProd_ms = now;
    }
}

// ============================================================================
// Core 1 — setup / loop (CONSUMER: ring buffer → I2S)
// ============================================================================

void setup1() {
    // Configure I2S output
    i2sOut.setBCLK(I2S_PIN_BCLK);
    i2sOut.setDATA(I2S_PIN_DATA);
    i2sOut.setBitsPerSample(BITS_PER_SAMPLE);
    i2sOut.setBuffers(8, 1024);  // 8 DMA buffers × 1024 bytes = 8 KB

    if (!i2sOut.begin(SAMPLE_RATE)) {
        Serial.println(F("[Core1] ERROR: I2S init failed!"));
        g_core1Ready = true;
        while (true) delay(1000);
    }

    g_core1Ready = true;
}

void loop1() {
    // Drain ring buffer into I2S
    uint32_t avail = ringAvailableRead();
    if (avail > 0) {
        // Write up to 512 frames per iteration
        uint32_t count = (avail > 512) ? 512 : avail;
        for (uint32_t i = 0; i < count; i++) {
            StereoFrame f = ringRead();
            // I2S.write() for 16-bit stereo: write L then R as 16-bit values
            i2sOut.write16(f.left, f.right);
        }
    } else {
        // Ring buffer empty — underrun! Core 0 couldn't keep up
        g_underruns++;
        delayMicroseconds(50);
    }

    // Performance counter
    static uint32_t consCount = 0;
    static uint32_t lastCons_ms = 0;
    consCount++;
    uint32_t now = millis();
    if (now - lastCons_ms >= 1000) {
        g_consumeLoops = consCount;
        consCount = 0;
        lastCons_ms = now;
    }
}
