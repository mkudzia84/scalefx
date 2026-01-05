/**
 * Storage Module Configuration
 * 
 * Compile-time configuration for SD Card and Flash modules.
 */

#ifndef STORAGE_CONFIG_H
#define STORAGE_CONFIG_H

// ============================================================================
//  DEBUG CONFIGURATION
// ============================================================================

/**
 * Storage Debug Mode
 * 
 * When enabled (1), includes verbose logging for storage operations:
 *  - Initialization details
 *  - File operations
 *  - Transfer progress
 * 
 * Disable to reduce serial output in production builds.
 */
#ifndef STORAGE_DEBUG
#define STORAGE_DEBUG               1  // 0 = Minimal logging, 1 = Full debug
#endif

// ============================================================================
//  DEBUG LOGGING MACRO
// ============================================================================

#if STORAGE_DEBUG
#define STORAGE_LOG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define STORAGE_LOG(tag, fmt, ...)
#endif

#endif // STORAGE_CONFIG_H
