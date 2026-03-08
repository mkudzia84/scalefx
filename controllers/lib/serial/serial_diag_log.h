/*
 * DiagLog — Diagnostic Log Output over Serial Protocol (Singleton)
 *
 * Sends human-readable log messages to the PC client as binary COBS packets.
 * Messages are buffered in a ring buffer with mutex protection, allowing safe
 * logging from both cores on RP2040.
 *
 * Universal across all ScaleFX boards — initialized by PicoServer::begin()
 * so every controller (GunFX, LightFX, GearControl, HubFX) can log via the
 * SFX_LOG_* macros without any local state.
 *
 * Wire format (CorePacket::LOG_MESSAGE = 0xFD):
 *   [level:u8][millis:u32LE][message:str]
 *
 * Log levels:
 *   0 = DEBUG   — verbose tracing (slave polling, state transitions)
 *   1 = INFO    — operational events (init, config loaded, connected)
 *   2 = WARN    — recoverable issues (SD not found, retry)
 *   3 = ERROR   — failures (hardware init failed, comm error)
 *
 * Usage (via macros — preferred):
 *   SFX_LOG_INFO("Initialized at %lu MHz", F_CPU / 1000000UL);
 *   SFX_LOG_WARN("Retry %d of %d", attempt, max);
 *   SFX_LOG_ERROR("Hardware init failed");
 *   SFX_LOG_DEBUG("State=%d val=%u", state, val);
 *
 * Usage (via singleton directly):
 *   DiagLog::instance().info("Custom message");
 *
 * Initialization (handled by PicoServer::begin()):
 *   DiagLog::instance().begin(&Serial);
 *   // flush is automatic in PicoServer::loop()
 *
 * Thread Safety:
 *   A pico mutex guards the format+enqueue step to prevent interleaved
 *   writes from multiple cores. Only the flushing core reads from the
 *   ring buffer (single-consumer).
 *
 * Compile-time stripping:
 *   Set SFX_ENABLE_DIAG_LOG=0 in build_flags to replace DiagLog with a
 *   zero-overhead stub. SFX_LOG_* macros compile to ((void)0).
 *
 * Ingestion (HubFX relay):
 *   HubFX can ingest log messages from slave boards into the ring
 *   buffer using DiagLog::instance().ingest(level, message) — the
 *   message is re-timestamped with local millis().
 */

#ifndef SERIAL_DIAG_LOG_H
#define SERIAL_DIAG_LOG_H

// Compile-time flag: set to 0 to strip all DiagLog code and RAM
// (e.g., -DSFX_ENABLE_DIAG_LOG=0 in platformio.ini build_flags)
#ifndef SFX_ENABLE_DIAG_LOG
#define SFX_ENABLE_DIAG_LOG 1
#endif

#include <Arduino.h>
#include <serial_core.h>

// ============================================================================
// Log Levels (always available — used by BusClient relay even when stub)
// ============================================================================

namespace DiagLevel {
    constexpr uint8_t DEBUG = 0;
    constexpr uint8_t INFO  = 1;
    constexpr uint8_t WARN  = 2;
    constexpr uint8_t ERR   = 3;  // ERROR conflicts with some macros

    inline const char* name(uint8_t level) {
        switch (level) {
            case DEBUG: return "DEBUG";
            case INFO:  return "INFO";
            case WARN:  return "WARN";
            case ERR:   return "ERROR";
            default:    return "?";
        }
    }
}

#if SFX_ENABLE_DIAG_LOG

#include <stdarg.h>
#include <pico/mutex.h>

// ============================================================================
// DiagLog Singleton
// ============================================================================

class DiagLog {
public:
    /**
     * @brief Access the single DiagLog instance
     *
     * Thread-safe: the instance is a static local with trivial construction.
     * The mutex inside is initialized in begin().
     */
    static DiagLog& instance() {
        static DiagLog inst;
        return inst;
    }

    /**
     * @brief Initialize with serial stream
     *
     * Must be called once (typically by PicoServer::begin()) before any
     * logging occurs. Safe to call multiple times — subsequent calls
     * update the stream pointer.
     *
     * @param serial The Stream to write COBS-encoded log packets on
     * @param packetType Packet type override (default CorePacket::LOG_MESSAGE)
     */
    void begin(Stream* serial, uint8_t packetType = CorePacket::LOG_MESSAGE) {
        _serial = serial;
        _packetType = packetType;
        if (!_mutexInitialized) {
            mutex_init(&_mutex);
            _mutexInitialized = true;
        }
    }

    /**
     * @brief Set minimum log level (messages below this are discarded)
     * @param level Minimum level (DiagLevel::DEBUG..DiagLevel::ERR)
     */
    void setMinLevel(uint8_t level) { _minLevel = level; }

    /**
     * @brief Get current minimum log level
     */
    uint8_t minLevel() const { return _minLevel; }

    // ========================================================================
    // Logging Methods (safe from any core, mutex-protected, buffered)
    // ========================================================================

    void debug(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        logv(DiagLevel::DEBUG, fmt, args);
        va_end(args);
    }

    void info(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        logv(DiagLevel::INFO, fmt, args);
        va_end(args);
    }

    void warn(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        logv(DiagLevel::WARN, fmt, args);
        va_end(args);
    }

    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        logv(DiagLevel::ERR, fmt, args);
        va_end(args);
    }

    /**
     * @brief Ingest a pre-formatted log message into the ring buffer
     *
     * Used by HubFX to relay log messages from slave boards. The message
     * is re-timestamped with local millis() and stored as-is (caller
     * should prepend source tag like "[GunFX] ").
     *
     * @param level Log level (DiagLevel::DEBUG..ERR)
     * @param message Pre-formatted message string
     */
    void ingest(uint8_t level, const char* message);

    /**
     * @brief Flush all buffered log messages to serial as COBS packets
     *
     * Called automatically by PicoServer::loop(). Each buffered message
     * is sent as a LOG_MESSAGE packet with TAG_ASYNC.
     *
     * @return Number of messages flushed
     */
    uint16_t flush();

    /**
     * @brief Get number of messages currently buffered
     */
    uint16_t pending() const {
        return (_head - _tail + RING_SIZE) % RING_SIZE;
    }

    /**
     * @brief Get count of dropped messages (ring buffer overflow)
     */
    uint32_t droppedCount() const { return _dropped; }

private:
    DiagLog() = default;
    ~DiagLog() = default;
    DiagLog(const DiagLog&) = delete;
    DiagLog& operator=(const DiagLog&) = delete;

    // Ring buffer constants
    static constexpr size_t MAX_MSG_LEN = 128;    // max message text per entry
    static constexpr uint16_t RING_SIZE = 64;     // power of 2, holds boot + reconnect history

    // Ring buffer entry — pre-formatted message with metadata
    struct LogEntry {
        uint32_t timestamp_ms;
        uint8_t  level;
        uint8_t  len;                     // message length (excluding null)
        char     message[MAX_MSG_LEN];    // null-terminated
    };

    LogEntry _ring[RING_SIZE];
    volatile uint16_t _head = 0;   // next write position
    volatile uint16_t _tail = 0;   // next read position
    volatile uint32_t _dropped = 0;

    Stream* _serial = nullptr;
    uint8_t _packetType = CorePacket::LOG_MESSAGE;
    uint8_t _minLevel = DiagLevel::INFO;  // default: INFO and above
    mutex_t _mutex;
    bool _mutexInitialized = false;

    /**
     * @brief Format and enqueue a log message (mutex-protected)
     */
    void logv(uint8_t level, const char* fmt, va_list args);
};

#else // SFX_ENABLE_DIAG_LOG == 0

// ============================================================================
// DiagLog Stub — zero overhead singleton, all methods compile to nothing
// ============================================================================

class DiagLog {
public:
    static DiagLog& instance() {
        static DiagLog inst;
        return inst;
    }

    void begin(Stream*, uint8_t = CorePacket::LOG_MESSAGE) {}
    void setMinLevel(uint8_t) {}
    uint8_t minLevel() const { return 0; }

    void debug(const char*, ...) {}
    void info(const char*, ...) {}
    void warn(const char*, ...) {}
    void error(const char*, ...) {}

    void ingest(uint8_t, const char*) {}
    uint16_t flush() { return 0; }
    uint16_t pending() const { return 0; }
    uint32_t droppedCount() const { return 0; }

private:
    DiagLog() = default;
    DiagLog(const DiagLog&) = delete;
    DiagLog& operator=(const DiagLog&) = delete;
};

#endif // SFX_ENABLE_DIAG_LOG

// ============================================================================
// Universal Logging Macros — use across all ScaleFX boards
// ============================================================================
//
//  SFX_LOG_INFO("Initialized at %lu MHz", freq);
//  SFX_LOG_WARN("Retry %d of %d", attempt, max);
//  SFX_LOG_ERROR("Hardware init failed");
//  SFX_LOG_DEBUG("State=%d val=%u", state, val);
//
//  When SFX_ENABLE_DIAG_LOG=0 these compile to nothing (zero overhead).
//  The singleton is initialized by PicoServer::begin() — logging before
//  that is silently discarded (begin() not yet called → _serial is null).

#if SFX_ENABLE_DIAG_LOG

#define SFX_LOG_DEBUG(fmt, ...) \
    DiagLog::instance().debug(fmt, ##__VA_ARGS__)

#define SFX_LOG_INFO(fmt, ...) \
    DiagLog::instance().info(fmt, ##__VA_ARGS__)

#define SFX_LOG_WARN(fmt, ...) \
    DiagLog::instance().warn(fmt, ##__VA_ARGS__)

#define SFX_LOG_ERROR(fmt, ...) \
    DiagLog::instance().error(fmt, ##__VA_ARGS__)

#else // SFX_ENABLE_DIAG_LOG == 0

#define SFX_LOG_DEBUG(fmt, ...) ((void)0)
#define SFX_LOG_INFO(fmt, ...)  ((void)0)
#define SFX_LOG_WARN(fmt, ...)  ((void)0)
#define SFX_LOG_ERROR(fmt, ...) ((void)0)

#endif // SFX_ENABLE_DIAG_LOG

#endif // SERIAL_DIAG_LOG_H
