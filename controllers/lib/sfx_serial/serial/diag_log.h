/*
 * DiagLog — Diagnostic Log Output over Serial Protocol (Singleton)
 *
 * Sends human-readable log messages to the PC client as binary COBS packets.
 * Messages are buffered in a rolling ring buffer with mutex protection,
 * allowing safe logging from both cores on RP2040/RP2350. When the ring
 * buffer is full, the oldest message is overwritten (rolling/circular
 * behavior) so new messages are never lost.
 *
 * Universal across all ScaleFX boards — initialized by SfxServer::begin()
 * so every controller (GunFX, LightFX, GearControl, HubFX) can log via the
 * SFX_LOG_* macros without any local state.
 *
 * Wire format (LOG_MESSAGE = 0xFD):
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
 * Initialization (handled by SfxServer::begin()):
 *   DiagLog::instance().begin(&Serial);
 *   // messages retrieved on-demand via DIAG_HISTORY command (sendHistory())
 *
 * Thread Safety:
 *   An SfxMutex guards the format+enqueue step to prevent interleaved
 *   writes from multiple cores. Ring indices (_head, _tail) and the
 *   overwrite counter use std::atomic with release/acquire ordering
 *   so sendHistory() can safely snapshot them without the mutex.
 *
 * Compile-time stripping:
 *   Set SFX_ENABLE_DIAG_LOG=0 in build_flags to replace DiagLog with a
 *   zero-overhead stub. SFX_LOG_* macros compile to ((void)0).
 *
 * Ingestion (HubFX relay):
 *   HubFX can ingest log messages from slave boards into the ring
 *   buffer using DiagLog::instance().ingest(level, message) — the
 *   message is re-timestamped with local SFX_MILLIS().
 */

#ifndef SFX_DIAG_LOG_H
#define SFX_DIAG_LOG_H

// Compile-time flag: set to 0 to strip all DiagLog code and RAM
// (e.g., -DSFX_ENABLE_DIAG_LOG=0 in platformio.ini build_flags)
#ifndef SFX_ENABLE_DIAG_LOG
#define SFX_ENABLE_DIAG_LOG 1
#endif

#include <platform/sfx_stream.h>   // sfx::Stream (was <Arduino.h> for Stream)
#include "wire.h"

// Default packet type for log messages (CorePacket::LOG_MESSAGE = 0xFD).
// Defined here as a literal to avoid depending on the full protocol header.
#ifndef SFX_LOG_PACKET_TYPE
#define SFX_LOG_PACKET_TYPE 0xFD
#endif

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
#include <atomic>
#include <platform/sfx_platform.h>

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
     * Must be called once (typically by SfxServer::begin()) before any
     * logging occurs. Safe to call multiple times — subsequent calls
     * update the stream pointer.
     *
     * @param serial The Stream to write COBS-encoded log packets on
     * @param packetType Packet type override (default 0xFD = LOG_MESSAGE)
     */
    void begin(sfx::Stream* serial, uint8_t packetType = SFX_LOG_PACKET_TYPE) {
        // Phase 4 polish (2026-05-27): the ring lives in PSRAM (ESP32) /
        // heap (Pico) instead of the singleton's BSS — keeps ~68 KB
        // (512 × 136 B) out of DRAM on HubFX where the DMA-cap pool is
        // already tight (audio I²S init wants a ~17 KB contiguous block).
        // _serial gates every log entry point; allocate _ring BEFORE
        // we store _serial so a PSRAM-exhaustion failure degrades to
        // "logs silently dropped" instead of a null deref.
        if (!_ring) {
            _ring = static_cast<LogEntry*>(sfxPsramCalloc(RING_SIZE, sizeof(LogEntry)));
            if (!_ring) return;   // alloc failed — leave _serial null
        }
        _packetType = packetType;
        if (!_mutexInitialized.load(std::memory_order_relaxed)) {
            sfxMutexInit(_mutex);
            _mutexInitialized.store(true, std::memory_order_release);
        }
        _serial.store(serial, std::memory_order_release);  // visible to both cores — last
    }

    /**
     * @brief Set minimum log level (messages below this are discarded)
     * @param level Minimum level (DiagLevel::DEBUG..DiagLevel::ERR)
     */
    void setMinLevel(uint8_t level) { _minLevel.store(level, std::memory_order_relaxed); }

    /**
     * @brief Get current minimum log level
     */
    uint8_t minLevel() const { return _minLevel.load(std::memory_order_relaxed); }

    /**
     * @brief Set minimum level for live LOG_MESSAGE emission on the wire
     *
     * Below this level (defaults to 0xFF == disabled) entries land only
     * in the ring buffer and are visible via the DIAG_HISTORY command.
     * Set to DiagLevel::INFO (or DEBUG) to stream every log line as a
     * TAG_ASYNC LOG_MESSAGE packet so the host's `subscribe` command can
     * follow the device live.
     */
    void setWireMinLevel(uint8_t level) { _wireMinLevel.store(level, std::memory_order_relaxed); }

    /**
     * @brief Get current wire-emission minimum level
     */
    uint8_t wireMinLevel() const { return _wireMinLevel.load(std::memory_order_relaxed); }

    /**
     * @brief Check if DiagLog is initialized (serial stream set)
     * 
     * Safe to call from any core. Use this from Core 1's setup1() to
     * wait for Core 0 to initialize DiagLog before logging.
     */
    bool isInitialized() const {
        return _serial.load(std::memory_order_acquire) != nullptr;
    }

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
     * is re-timestamped with local SFX_MILLIS() and stored as-is (caller
     * should prepend source tag like "[GunFX] ").
     *
     * @param level Log level (DiagLevel::DEBUG..ERR)
     * @param message Pre-formatted message string
     */
    void ingest(uint8_t level, const char* message);

    /**
     * @brief Send buffered log messages WITHOUT draining the buffer
     *
     * Unlike flush(), this method does NOT advance _tail. Use this for
     * the DIAG_HISTORY command — allows viewing log history without
     * consuming the rolling buffer.
     *
     * Messages are sent oldest-first (chronological order).
     *
     * @param max Cap on how many messages to send.  0 = unlimited
     *            (entire ring).  When max > 0 and the ring contains
     *            more entries, the NEWEST `max` are sent (tail).
     * @return Number of messages sent
     */
    uint16_t sendHistory(uint16_t max = 0);

    /**
     * @brief Get number of messages currently buffered
     */
    uint16_t pending() const {
        uint16_t h = _head.load(std::memory_order_acquire);
        uint16_t t = _tail.load(std::memory_order_acquire);
        return (h - t + RING_SIZE) % RING_SIZE;
    }

    /**
     * @brief Get count of overwritten messages (ring buffer rollover)
     *
     * When the ring buffer is full, new messages overwrite the oldest.
     * This counter tracks how many messages have been lost to rollover.
     */
    uint32_t overwrittenCount() const { return _overwritten.load(std::memory_order_acquire); }

#ifdef ESP32
    /**
     * @brief Redirect ESP-IDF ESP_LOGx() output into DiagLog ring buffer
     *
     * Installs a custom vprintf function via esp_log_set_vprintf() that
     * intercepts all ESP_LOGE/W/I/D/V output and feeds it into the ring
     * buffer as proper COBS-framed LOG_MESSAGE packets. This prevents
     * raw text from corrupting the binary COBS protocol on UART0.
     *
     * Must be called AFTER begin() (needs serial and mutex initialized).
     * The redirect is permanent — there is no restore method.
     *
     * ESP-IDF log format: "X (timestamp) tag: message\n"
     * where X is E/W/I/D/V. The callback parses X to map to DiagLevel,
     * strips the trailing newline, and ingests the full line.
     */
    void captureEspLog();
#endif

private:
    DiagLog() = default;
    ~DiagLog() {
        // Singleton dtor effectively never runs on embedded (no program
        // exit), but keep alloc/free symmetric for hosted-side unit tests.
        if (_ring) {
            sfxPsramFree(_ring);
            _ring = nullptr;
        }
    }
    DiagLog(const DiagLog&) = delete;
    DiagLog& operator=(const DiagLog&) = delete;

    // Ring buffer constants
    static constexpr size_t MAX_MSG_LEN = 128;    // max message text per entry
#ifdef ESP32
    static constexpr uint16_t RING_SIZE = 512;    // ESP32: ~68 KB ring in PSRAM
#else
    static constexpr uint16_t RING_SIZE = 128;    // Pico: ~17 KB ring in heap
#endif

    // Ring buffer entry — pre-formatted message with metadata
    struct LogEntry {
        uint32_t timestamp_ms;
        uint8_t  level;
        uint8_t  len;                     // message length (excluding null)
        char     message[MAX_MSG_LEN];    // null-terminated
    };

    // Allocated in begin() via sfxPsramCalloc — PSRAM on ESP32, heap on
    // Pico. Set-once after begin(); read by logv/ingest/sendHistory.
    // The _serial atomic is the canonical "DiagLog is ready" gate and is
    // stored AFTER _ring is non-null, so callers never see a non-null
    // _serial paired with a null _ring (release-acquire pair).
    LogEntry* _ring = nullptr;
    std::atomic<uint16_t> _head{0};       // next write position — Core 0/1 write (mutex), sendHistory reads (lock-free)
    std::atomic<uint16_t> _tail{0};       // oldest entry position — Core 0/1 write (mutex), sendHistory reads (lock-free)
    std::atomic<uint32_t> _overwritten{0}; // count of entries lost to rollover

    // Cross-core visibility: Core 0 writes in begin(), Core 1 reads in logv()
    std::atomic<sfx::Stream*> _serial{nullptr};       // Core 0 writes, both cores read
    uint8_t _packetType = SFX_LOG_PACKET_TYPE;
    std::atomic<uint8_t> _minLevel{DiagLevel::INFO};  // Core 0 writes (setMinLevel), both cores read (logv)
    // 0xFF = no live emission (default); set via `setWireMinLevel()` to
    // stream LOG_MESSAGE async packets for any entry at or above the level.
    std::atomic<uint8_t> _wireMinLevel{0xFF};
    SfxMutex _mutex;
    std::atomic<bool> _mutexInitialized{false};       // Core 0 writes, both cores read

    /**
     * @brief Format and enqueue a log message (mutex-protected)
     */
    void logv(uint8_t level, const char* fmt, va_list args);

    /**
     * @brief Emit one buffered entry as a TAG_ASYNC LOG_MESSAGE packet.
     *
     * Called from `logv()` and `ingest()` when the entry's level meets
     * the `_wireMinLevel` gate.  Same wire encoding as `sendHistory()`.
     */
    void emitLive(const LogEntry& entry);
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

    void begin(sfx::Stream*, uint8_t = SFX_LOG_PACKET_TYPE) {}
    void setMinLevel(uint8_t) {}
    uint8_t minLevel() const { return 0; }
    void setWireMinLevel(uint8_t) {}
    uint8_t wireMinLevel() const { return 0xFF; }

    void debug(const char*, ...) {}
    void info(const char*, ...) {}
    void warn(const char*, ...) {}
    void error(const char*, ...) {}

    void ingest(uint8_t, const char*) {}
    bool isInitialized() const { return false; }
    uint16_t sendHistory(uint16_t = 0) { return 0; }
    uint16_t pending() const { return 0; }
    uint32_t overwrittenCount() const { return 0; }

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
//  The singleton is initialized by SfxServer::begin() — logging before
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

#endif // SFX_DIAG_LOG_H
