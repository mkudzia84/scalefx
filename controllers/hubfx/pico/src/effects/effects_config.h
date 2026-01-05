/**
 * Effects Module Configuration
 * 
 * Compile-time configuration for Engine FX and Gun FX modules.
 */

#ifndef EFFECTS_CONFIG_H
#define EFFECTS_CONFIG_H

// ============================================================================
//  DEBUG CONFIGURATION
// ============================================================================

/**
 * Effects Debug Mode
 * 
 * When enabled (1), includes debug logging for EngineFX and GunFX:
 *  - State transitions
 *  - PWM input values
 *  - Sound playback events
 *  - Serial communication status
 * 
 * Disable to save flash space in production builds.
 */
#ifndef EFFECTS_DEBUG
#define EFFECTS_DEBUG               1  // 0 = Minimal logging, 1 = Full debug
#endif

// ============================================================================
//  DEBUG LOGGING MACRO
// ============================================================================

#if EFFECTS_DEBUG
#define EFFECTS_LOG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define EFFECTS_LOG(tag, fmt, ...)
#endif

#endif // EFFECTS_CONFIG_H
