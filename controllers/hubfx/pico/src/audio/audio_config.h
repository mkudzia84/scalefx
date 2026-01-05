/**
 * HubFX Audio System Configuration
 * 
 * Compile-time configuration for I2S, codec, and mixer settings.
 * Modify these values to match your hardware setup and requirements.
 */

#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include "../debug_config.h"

// ============================================================================
//  TESTING CONFIGURATION
// ============================================================================

// AUDIO_DEBUG and AUDIO_MOCK_I2S are defined in debug_config.h

// ============================================================================
//  I2S AUDIO CONFIGURATION
// ============================================================================

/**
 * Sample Rate Selection
 * 
 * Common rates: 44100 (CD quality), 48000 (pro audio), 22050 (lower quality)
 * 
 * WARNING: Changing this requires recalculating WM8960_PLL_K_VALUE!
 * See PLL calculation section below.
 */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE           44100
#endif

/**
 * Bit Depth
 * 
 * Supported: 16, 24, 32
 * Note: WM8960 uses 16-bit internally, but I2S frames are always 32-bit
 */
#ifndef AUDIO_BIT_DEPTH
#define AUDIO_BIT_DEPTH             16
#endif

/**
 * I2S Frame Format
 * 
 * Fixed at 32 bits per channel (required by RP2040 I2S implementation)
 * Total BCLK cycles per sample = I2S_BITS_PER_CHANNEL × 2 channels = 64
 */
#define I2S_BITS_PER_CHANNEL        32
#define I2S_CHANNELS                2

// Derived I2S Timing (DO NOT MODIFY - calculated automatically)
#define I2S_LRCLK_FREQ              AUDIO_SAMPLE_RATE
#define I2S_BCLK_FREQ               (AUDIO_SAMPLE_RATE * I2S_BITS_PER_CHANNEL * I2S_CHANNELS)
#define I2S_DATA_RATE               (AUDIO_SAMPLE_RATE * AUDIO_BIT_DEPTH * I2S_CHANNELS)

// ============================================================================
//  WM8960 CODEC CONFIGURATION
// ============================================================================

/**
 * I2C Bus Speed (Hz)
 * 
 * Standard: 100000 (100 kHz)
 * Reduced:  50000  (50 kHz)  - recommended for breadboard/long wires
 * Slow:     10000  (10 kHz)  - for very poor signal integrity
 * 
 * Lower speeds are more tolerant of wire capacitance and length.
 * Use 50 kHz if you experience I2C timeout errors (error code 5).
 */
#ifndef WM8960_I2C_SPEED
#define WM8960_I2C_SPEED            50000
#endif

/**
 * WM8960 PLL Configuration
 * 
 * The WM8960 requires SYSCLK = 256 × sample_rate for optimal operation.
 * Since Pico doesn't generate MCLK, we use the PLL to multiply BCLK.
 * 
 * PLL Input:  BCLK = sample_rate × 64 (for 32-bit stereo I2S)
 * PLL Output: SYSCLK = sample_rate × 256
 * Multiplier: 4× (with fractional adjustment via K value)
 * 
 * K Value Calculation:
 *   K = (SYSCLK × 2^24) / (BCLK × 4)
 * 
 * For 44.1 kHz:
 *   BCLK = 2,822,400 Hz
 *   SYSCLK = 11,289,600 Hz
 *   K = (11,289,600 × 16,777,216) / (2,822,400 × 4) = 0x0C93E9
 * 
 * For 48 kHz:
 *   BCLK = 3,072,000 Hz
 *   SYSCLK = 12,288,000 Hz  
 *   K = (12,288,000 × 16,777,216) / (3,072,000 × 4) = 0x0C0000
 */
#if AUDIO_SAMPLE_RATE == 44100
    #define WM8960_PLL_K_VALUE      0x0C93E9    // K for 44.1 kHz
#elif AUDIO_SAMPLE_RATE == 48000
    #define WM8960_PLL_K_VALUE      0x0C0000    // K for 48 kHz
#elif AUDIO_SAMPLE_RATE == 22050
    #define WM8960_PLL_K_VALUE      0x0C93E9    // Same as 44.1k (half BCLK, half SYSCLK)
#else
    #error "Unsupported AUDIO_SAMPLE_RATE - must calculate PLL K value"
    // Use formula: K = (Fs × 256 × 2^24) / (Fs × 64 × 4)
    //            = (256 × 16777216) / 256 = 0x0C93E9 (for 44.1k family)
    //            = (256 × 16777216) / 256 = 0x0C0000 (for 48k family)
#endif

#define WM8960_PLL_PRESCALE         0x04        // Divide by 4
#define WM8960_PLL_K_HIGH          ((WM8960_PLL_K_VALUE >> 16) & 0xFF)
#define WM8960_PLL_K_MID           ((WM8960_PLL_K_VALUE >> 8) & 0xFF)
#define WM8960_PLL_K_LOW           (WM8960_PLL_K_VALUE & 0xFF)

// ============================================================================
//  AUDIO MIXER CONFIGURATION
// ============================================================================

/**
 * Maximum Simultaneous Channels
 * 
 * More channels = higher CPU usage + more RAM
 * Recommended: 4-8 for most applications
 */
#ifndef AUDIO_MAX_CHANNELS
#define AUDIO_MAX_CHANNELS          8
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
#define AUDIO_MIX_BUFFER_SIZE       512
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
#define AUDIO_STREAM_BUFFER_SIZE    2048
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
