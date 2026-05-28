/**
 * Audio System Configuration
 * 
 * Compile-time configuration for I2S, codec, and mixer settings.
 * Modify these values to match your hardware setup and requirements.
 *
 * All values are #ifndef-guarded — override via build flags (-D) in
 * platformio.ini or CMakeLists.txt if needed.
 */

#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

// ============================================================================
//  TESTING CONFIGURATION
// ============================================================================

/**
 * Enable verbose audio debug logging
 * Set via build flag: -DAUDIO_DEBUG=1
 */
#ifndef AUDIO_DEBUG
#define AUDIO_DEBUG                 0
#endif

// ============================================================================
//  I2S AUDIO CONFIGURATION
// ============================================================================

/**
 * Sample Rate Selection
 * 
 * Common rates: 44100 (CD quality), 48000 (pro audio), 22050 (lower quality)
 */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE           48000
#endif

/**
 * Bit Depth
 * 
 * Supported: 16, 24, 32
 */
#ifndef AUDIO_BIT_DEPTH
#define AUDIO_BIT_DEPTH             16
#endif

/**
 * I2S Frame Format
 * 
 * RP2040/RP2350: Fixed 32 bits per channel (PIO I2S requirement).
 * ESP32-S3: Configurable — typically matches AUDIO_BIT_DEPTH.
 */
#if defined(ARDUINO_ARCH_RP2040)
    #define I2S_BITS_PER_CHANNEL    32
#elif defined(ARDUINO_ARCH_ESP32)
    #define I2S_BITS_PER_CHANNEL    AUDIO_BIT_DEPTH
#else
    #define I2S_BITS_PER_CHANNEL    32
#endif

#define I2S_CHANNELS                2

// Derived I2S Timing (DO NOT MODIFY - calculated automatically)
#define I2S_LRCLK_FREQ              AUDIO_SAMPLE_RATE
#define I2S_BCLK_FREQ               (AUDIO_SAMPLE_RATE * I2S_BITS_PER_CHANNEL * I2S_CHANNELS)
#define I2S_DATA_RATE               (AUDIO_SAMPLE_RATE * AUDIO_BIT_DEPTH * I2S_CHANNELS)

// ============================================================================
//  I2S PIN CONFIGURATION
// ============================================================================

/**
 * I2S pin assignment — passed to I2S output begin()
 */
struct I2SPinConfig {
    uint8_t dataPin;     // I2S serial data output (DIN/DOUT)
    uint8_t bclkPin;     // I2S bit clock (BCLK/SCK)
    uint8_t lrclkPin;    // I2S word select / left-right clock (LRCLK/WS)
};

// ============================================================================
//  AUDIO MIXER CONFIGURATION
// ============================================================================

/**
 * Maximum Simultaneous Channels
 *
 * More channels = higher CPU usage + more RAM (per-channel decode
 * buffer 96 KB × 2 in PSRAM + page-cache prefetch slots).  Reduced
 * from 8 → 6 in Phase 4 polish (2026-05-27) to free PSRAM for the
 * page_cache's larger pages and prefetch headroom.
 *
 * Recommended: 4-8.
 */
#ifndef AUDIO_MAX_CHANNELS
#define AUDIO_MAX_CHANNELS          6
#endif

/**
 * Mix Buffer Size (samples per DMA transfer)
 * 
 * Smaller = lower latency, higher CPU overhead
 * Larger  = higher latency, more efficient
 * 
 * Typical: 256-1024 samples
 * At 44.1kHz: 512 samples = 11.6ms latency
 */
#ifndef AUDIO_MIX_BUFFER_SIZE
#if defined(ARDUINO_ARCH_ESP32)
#define AUDIO_MIX_BUFFER_SIZE       1024
#else
#define AUDIO_MIX_BUFFER_SIZE       512
#endif
#endif

/**
 * Stream Buffer Size (bytes per SD card read)
 * 
 * Larger buffers reduce SD card access overhead
 * Must be multiple of 512 (SD card sector size)
 * 
 * Typical: 2048-8192 bytes
 */
#ifndef AUDIO_STREAM_BUFFER_SIZE
#if defined(ARDUINO_ARCH_ESP32)
#define AUDIO_STREAM_BUFFER_SIZE    8192
#else
#define AUDIO_STREAM_BUFFER_SIZE    2048
#endif
#endif

// ============================================================================
//  WIRING TOLERANCE CONFIGURATION
// ============================================================================

/**
 * I2S Wire Length Warning Threshold
 * 
 * At high BCLK frequencies (2.8+ MHz), wire length significantly impacts
 * signal integrity. Enable runtime warnings if your wiring setup is
 * particularly sensitive.
 */
#ifndef I2S_WIRE_LENGTH_WARNING
#define I2S_WIRE_LENGTH_WARNING     1   // 1 = warn about timing at boot
#endif

/**
 * Maximum I2S Wire Length (informational)
 * 
 * For BCLK > 2 MHz: < 6 inches (150 mm) recommended
 * For BCLK > 5 MHz: < 3 inches (75 mm) recommended
 * 
 * Bit period at 2.8 MHz BCLK = 355 ns
 * Wire propagation delay ≈ 1.5 ns per inch
 */
#define I2S_MAX_WIRE_LENGTH_MM      150     // 6 inches

// ============================================================================
//  DEBUGGING / DIAGNOSTICS
// ============================================================================

/**
 * Enable verbose I2S timing diagnostics at boot
 */
#ifndef AUDIO_DEBUG_TIMING
#define AUDIO_DEBUG_TIMING          0
#endif

/**
 * Enable codec register dump at initialization
 */
#ifndef AUDIO_DEBUG_CODEC_REGS
#define AUDIO_DEBUG_CODEC_REGS      0
#endif

/**
 * Mixer pace-log telemetry (Phase 7 polish, 2026-05-28).
 *
 * When set, the producer task emits a 1 Hz `[pace]` log line with the
 * minimum ring fill %, max refill latency, slow-refill counts, and a
 * per-channel starve summary.  The atomics + counters that back this
 * live in the mixer singleton's DRAM BSS:
 *   - _ringMinFillPct       1 B
 *   - _maxSdReadUs          4 B
 *   - _slowSdReads          4 B
 *   - _verySlowSdReads      4 B
 *   - _channelStarves[N]    N × 4 B
 * plus the entire periodic-logger code block in producerTaskFunc
 * (~120 lines, ~1 KB of flash IRAM via the format string).
 *
 * Defaults
 *   `_debug` env       → 1 (pace log on)
 *   `_release` env     → 0 (pace log compiled out, fields gone)
 *
 * Override per-firmware: `-DSFX_AUDIO_PACE_TELEMETRY=1` or =0.
 * Always-on stats (used by AUDIO_STATUS_RESP):
 *   _underruns, _consumeLoops, _consumeFrames — kept under all builds.
 */
#ifndef SFX_AUDIO_PACE_TELEMETRY
#  if defined(DEBUG_ENABLED) && DEBUG_ENABLED
#    define SFX_AUDIO_PACE_TELEMETRY 1
#  elif defined(CORE_DEBUG_LEVEL) && CORE_DEBUG_LEVEL >= 3
#    define SFX_AUDIO_PACE_TELEMETRY 1
#  else
#    define SFX_AUDIO_PACE_TELEMETRY 0
#  endif
#endif

// ============================================================================
//  COMPILE-TIME VALIDATION
// ============================================================================

// Validate sample rate is reasonable
#if AUDIO_SAMPLE_RATE < 8000 || AUDIO_SAMPLE_RATE > 192000
    #error "AUDIO_SAMPLE_RATE must be between 8000 and 192000 Hz"
#endif

// Validate bit depth
#if AUDIO_BIT_DEPTH != 16 && AUDIO_BIT_DEPTH != 24 && AUDIO_BIT_DEPTH != 32
    #error "AUDIO_BIT_DEPTH must be 16, 24, or 32"
#endif

// Warn about high BCLK frequencies
#if I2S_BCLK_FREQ > 10000000
    #warning "I2S BCLK > 10 MHz - use very short wires (<3 inches) and proper PCB design"
#elif I2S_BCLK_FREQ > 5000000
    #warning "I2S BCLK > 5 MHz - keep wires short (<6 inches) and use good signal integrity practices"
#endif

// Validate buffer sizes
#if AUDIO_MIX_BUFFER_SIZE < 64 || AUDIO_MIX_BUFFER_SIZE > 4096
    #error "AUDIO_MIX_BUFFER_SIZE must be between 64 and 4096"
#endif

#if AUDIO_STREAM_BUFFER_SIZE % 512 != 0
    #error "AUDIO_STREAM_BUFFER_SIZE must be multiple of 512 (SD sector size)"
#endif

#endif // AUDIO_CONFIG_H
