/**
 * Audio Module Logging Macros
 *
 * Module-specific logging macros that route through the DiagLog singleton.
 * Messages are sent as binary COBS packets when a serial connection is active.
 *
 * This header provides the same logging interface as hubfx_log.h but is
 * decoupled from the HubFX firmware - usable by any controller that
 * includes the components and serial libraries.
 *
 * Usage:
 *   #include "audio_log.h"
 *   MIXER_LOG("Playing %s on ch %d", path, channel);
 */

#ifndef AUDIO_LOG_H
#define AUDIO_LOG_H

#include "serial/diag_log.h"

// --- Audio Mixer (info / warn / error) ---
#define MIXER_LOG(fmt, ...) \
    do { DiagLog::instance().info("[Mixer] " fmt, ##__VA_ARGS__); } while(0)

#define MIXER_WARN(fmt, ...) \
    do { DiagLog::instance().warn("[Mixer] " fmt, ##__VA_ARGS__); } while(0)

#define MIXER_ERROR(fmt, ...) \
    do { DiagLog::instance().error("[Mixer] " fmt, ##__VA_ARGS__); } while(0)

// --- TAS5825 Codec ---
#define TAS5825_LOG(fmt, ...) \
    do { DiagLog::instance().info("[TAS5825] " fmt, ##__VA_ARGS__); } while(0)

// --- PCM5102A Codec ---
#define PCM5102_LOG(fmt, ...) \
    do { DiagLog::instance().info("[PCM5102] " fmt, ##__VA_ARGS__); } while(0)

// --- Mock I2S ---
#define MOCK_LOG(fmt, ...) \
    do { DiagLog::instance().info("[MockI2S] " fmt, ##__VA_ARGS__); } while(0)

#endif // AUDIO_LOG_H
