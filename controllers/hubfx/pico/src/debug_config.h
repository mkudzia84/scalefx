/**
 * HubFX Debug Configuration
 * 
 * Master compile-time debug flag control.
 * Set individual flags to 0 for production builds to save flash space.
 * 
 * Estimated savings (when disabled):
 *   AUDIO_DEBUG    = 0  →  ~3.3 KB
 *   EFFECTS_DEBUG  = 0  →  ~0.8 KB
 *   STORAGE_DEBUG  = 0  →  ~1.5 KB
 *   CONFIG_DEBUG   = 0  →  ~0.5 KB
 *   MIXER_DEBUG    = 0  →  ~0.8 KB
 *   TOTAL (all off)     →  ~7.0 KB
 */

#ifndef DEBUG_CONFIG_H
#define DEBUG_CONFIG_H

// ============================================================================
//  MASTER DEBUG CONTROL
// ============================================================================

/**
 * Set to 0 to disable ALL debug output for production builds.
 * Individual flags below can still override if needed.
 */
#ifndef DEBUG_ENABLED
#define DEBUG_ENABLED               1
#endif

// ============================================================================
//  MODULE DEBUG FLAGS
// ============================================================================

/**
 * Main/Startup Debug
 * Includes: Boot progress, initialization status, component startup messages
 */
#ifndef MAIN_DEBUG
#define MAIN_DEBUG                  DEBUG_ENABLED
#endif

/**
 * Audio Codec Debug
 * Includes: testCommunication(), readRegisterCache(), writeRegisterDebug(),
 *           printStatus(), reinitialize(), getCommunicationInterface()
 */
#ifndef AUDIO_DEBUG
#define AUDIO_DEBUG                 DEBUG_ENABLED
#endif

/**
 * Effects Debug (EngineFX, GunFX)
 * Includes: State transitions, PWM input values, sound playback events
 */
#ifndef EFFECTS_DEBUG
#define EFFECTS_DEBUG               DEBUG_ENABLED
#endif

/**
 * Storage Debug (SD Card, Flash)
 * Includes: Init diagnostics, file operations, transfer progress
 */
#ifndef STORAGE_DEBUG
#define STORAGE_DEBUG               DEBUG_ENABLED
#endif

/**
 * Config Reader Debug
 * Includes: YAML parsing progress, key-value pairs, section tracking
 */
#ifndef CONFIG_DEBUG
#define CONFIG_DEBUG                DEBUG_ENABLED
#endif

/**
 * Audio Mixer Debug
 * Includes: Channel play/stop/fade events, file open status
 */
#ifndef MIXER_DEBUG
#define MIXER_DEBUG                 DEBUG_ENABLED
#endif

// ============================================================================
//  MOCK I2S (Testing Only)
// ============================================================================

/**
 * Mock I2S Mode - for testing without audio hardware
 * Captures audio data to memory buffer for statistics.
 */
#ifndef AUDIO_MOCK_I2S
#define AUDIO_MOCK_I2S              0
#endif

// ============================================================================
//  DEBUG LOGGING MACROS
// ============================================================================

#if MAIN_DEBUG
#define MAIN_LOG(fmt, ...) Serial.printf("[MAIN] " fmt "\n", ##__VA_ARGS__)
#define CORE1_LOG(fmt, ...) Serial.printf("[CORE1] " fmt "\n", ##__VA_ARGS__)
#else
#define MAIN_LOG(fmt, ...)
#define CORE1_LOG(fmt, ...)
#endif

#if MIXER_DEBUG
#define MIXER_LOG(fmt, ...) Serial.printf("[AudioMixer] " fmt "\n", ##__VA_ARGS__)
#else
#define MIXER_LOG(fmt, ...)
#endif

#if CONFIG_DEBUG
#define CONFIG_LOG(fmt, ...) Serial.printf("[Config] " fmt "\n", ##__VA_ARGS__)
#else
#define CONFIG_LOG(fmt, ...)
#endif

#if STORAGE_DEBUG
#define SD_LOG(fmt, ...) Serial.printf("[SD] " fmt "\n", ##__VA_ARGS__)
#else
#define SD_LOG(fmt, ...)
#endif

#if EFFECTS_DEBUG
#define EFFECTS_LOG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define EFFECTS_LOG(tag, fmt, ...)
#endif

#if AUDIO_DEBUG
#define WM8960_LOG(fmt, ...) Serial.printf("[WM8960] " fmt "\n", ##__VA_ARGS__)
#define TAS5825_LOG(fmt, ...) Serial.printf("[TAS5825M] " fmt "\n", ##__VA_ARGS__)
#else
#define WM8960_LOG(fmt, ...)
#define TAS5825_LOG(fmt, ...)
#endif

#endif // DEBUG_CONFIG_H
