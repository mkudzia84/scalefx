/**
 * HubFX Logging Configuration
 *
 * Module-specific logging macros that route through the DiagLog singleton
 * (defined in serial_diag_log.h, initialized by PicoServer::begin()).
 * Messages are sent to the PC client as binary COBS packets
 * (CorePacket::LOG_MESSAGE = 0xFD) when connected.
 *
 * Replaces the old debug_config.h which used Serial.printf (unsafe — corrupts
 * the binary COBS protocol on the same serial port).
 *
 * Usage:
 *   #include "hubfx_log.h"      // or via audio_config.h, etc.
 *   MIXER_LOG("Playing %s on ch %d", path, channel);
 *
 * These macros add module-tag prefixes on top of the universal SFX_LOG_*
 * macros from serial_diag_log.h. Use SFX_LOG_* for untagged messages.
 */

#ifndef HUBFX_LOG_H
#define HUBFX_LOG_H

#include <serial_diag_log.h>

// ============================================================================
//  COMPILE-TIME FEATURE FLAGS
// ============================================================================

/**
 * Audio Debug — controls inclusion of debug/diagnostic methods in codec
 * drivers (testCommunication, readRegisterCache, printStatus, etc.)
 * These are compile-time method guards, NOT logging macros.
 */
#ifndef AUDIO_DEBUG
#define AUDIO_DEBUG 1
#endif

/**
 * Mock I2S Mode — for testing without audio hardware.
 * Captures audio data to memory buffer for statistics.
 */
#ifndef AUDIO_MOCK_I2S
#define AUDIO_MOCK_I2S 0
#endif

// ============================================================================
//  MODULE LOGGING MACROS
// ============================================================================
//
//  All macros route through the DiagLog ring buffer. Each module gets a
//  tagged prefix for easy filtering on the client side.
//
//  Log levels:
//    info  — normal operational events
//    debug — verbose tracing (filtered by DiagLog::setMinLevel)
//    warn  — recoverable issues
//    error — failures

// --- Main / Core ---
#define MAIN_LOG(fmt, ...) \
    do { DiagLog::instance().info("[MAIN] " fmt, ##__VA_ARGS__); } while(0)

#define CORE1_LOG(fmt, ...) \
    do { DiagLog::instance().info("[CORE1] " fmt, ##__VA_ARGS__); } while(0)

// --- Audio Mixer (info / warn / error) ---
#define MIXER_LOG(fmt, ...) \
    do { DiagLog::instance().info("[Mixer] " fmt, ##__VA_ARGS__); } while(0)

#define MIXER_WARN(fmt, ...) \
    do { DiagLog::instance().warn("[Mixer] " fmt, ##__VA_ARGS__); } while(0)

#define MIXER_ERROR(fmt, ...) \
    do { DiagLog::instance().error("[Mixer] " fmt, ##__VA_ARGS__); } while(0)

// --- Config ---
#define CONFIG_LOG(fmt, ...) \
    do { DiagLog::instance().info("[Config] " fmt, ##__VA_ARGS__); } while(0)

// --- SD Card ---
#define SD_LOG(fmt, ...) \
    do { DiagLog::instance().info("[SD] " fmt, ##__VA_ARGS__); } while(0)

// --- Effects ---
#define EFFECTS_LOG(tag, fmt, ...) \
    do { DiagLog::instance().info("[" tag "] " fmt, ##__VA_ARGS__); } while(0)

// --- TAS5825 Codec ---
#define TAS5825_LOG(fmt, ...) \
    do { DiagLog::instance().info("[TAS5825] " fmt, ##__VA_ARGS__); } while(0)

// --- Mock I2S ---
#define MOCK_LOG(fmt, ...) \
    do { DiagLog::instance().info("[MockI2S] " fmt, ##__VA_ARGS__); } while(0)

// --- GunFX (slave relay) ---
#define GUNFX_LOG(fmt, ...) \
    do { DiagLog::instance().info("[GunFX] " fmt, ##__VA_ARGS__); } while(0)

#endif // HUBFX_LOG_H
