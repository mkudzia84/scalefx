/*
 * DiagLog — Diagnostic Log Output Implementation
 *
 * See diag_log.h for documentation.
 */

#include "diag_log.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#if SFX_ENABLE_DIAG_LOG

using namespace SfxWire;

// ============================================================================
// Log Entry Formatting (mutex-protected, safe from any core)
// ============================================================================

void DiagLog::logv(uint8_t level, const char* fmt, va_list args) {
    if (level < _minLevel.load(std::memory_order_relaxed)) return;
    if (!_serial.load(std::memory_order_acquire)) return;

    sfxMutexLock(_mutex);

    // Write at _head position (load once under mutex)
    uint16_t curHead = _head.load(std::memory_order_relaxed);
    LogEntry& entry = _ring[curHead];
    entry.timestamp_ms = SFX_MILLIS();
    entry.level = level;
    int written = vsnprintf(entry.message, MAX_MSG_LEN, fmt, args);
    entry.len = (written < 0) ? 0 : ((size_t)written >= MAX_MSG_LEN ? MAX_MSG_LEN - 1 : (uint8_t)written);

    uint16_t nextHead = (curHead + 1) % RING_SIZE;
    if (nextHead == _tail.load(std::memory_order_relaxed)) {
        // Ring full — overwrite oldest by advancing tail
        _tail.store((_tail.load(std::memory_order_relaxed) + 1) % RING_SIZE, std::memory_order_release);
        _overwritten.fetch_add(1, std::memory_order_relaxed);
    }
    _head.store(nextHead, std::memory_order_release);

    // Live-stream LOG_MESSAGE for hosts that have subscribed.
    uint8_t wireMin = _wireMinLevel.load(std::memory_order_relaxed);
    bool emitNow = (wireMin <= level);

    sfxMutexUnlock(_mutex);

    if (emitNow) emitLive(_ring[curHead]);
}

// ============================================================================
// Ingest — Insert pre-formatted message into ring buffer
// ============================================================================

void DiagLog::ingest(uint8_t level, const char* message) {
    if (level < _minLevel.load(std::memory_order_relaxed)) return;
    if (!_serial.load(std::memory_order_acquire)) return;

    sfxMutexLock(_mutex);

    // Write at _head position (load once under mutex)
    uint16_t curHead = _head.load(std::memory_order_relaxed);
    LogEntry& entry = _ring[curHead];
    entry.timestamp_ms = SFX_MILLIS();  // re-stamp with local time
    entry.level = level;
    size_t msgLen = strlen(message);
    if (msgLen >= MAX_MSG_LEN) msgLen = MAX_MSG_LEN - 1;
    memcpy(entry.message, message, msgLen);
    entry.message[msgLen] = '\0';
    entry.len = (uint8_t)msgLen;

    uint16_t nextHead = (curHead + 1) % RING_SIZE;
    if (nextHead == _tail.load(std::memory_order_relaxed)) {
        // Ring full — overwrite oldest by advancing tail
        _tail.store((_tail.load(std::memory_order_relaxed) + 1) % RING_SIZE, std::memory_order_release);
        _overwritten.fetch_add(1, std::memory_order_relaxed);
    }
    _head.store(nextHead, std::memory_order_release);

    uint8_t wireMin = _wireMinLevel.load(std::memory_order_relaxed);
    bool emitNow = (wireMin <= level);

    sfxMutexUnlock(_mutex);

    if (emitNow) emitLive(_ring[curHead]);
}

// ============================================================================
// Emit one entry as a TAG_ASYNC LOG_MESSAGE packet (live-stream path)
// ============================================================================

void DiagLog::emitLive(const LogEntry& entry) {
    sfx::Stream* serial = _serial.load(std::memory_order_acquire);
    if (!serial) return;

    uint8_t payload[1 + 4 + MAX_MSG_LEN];
    payload[0] = entry.level;
    putU32LE(&payload[1], entry.timestamp_ms);
    memcpy(&payload[5], entry.message, entry.len);
    size_t payloadLen = 5 + entry.len;

    uint8_t buffer[COBS_BUFFER_SIZE];
    size_t encodedLen = encodePacket(buffer, _packetType, TAG_ASYNC, payload, payloadLen);
    if (encodedLen > 0) {
        serial->write(buffer, encodedLen);
    }
}

// ============================================================================
// Send History — Send buffered messages WITHOUT draining (for DIAG_HISTORY)
// ============================================================================

uint16_t DiagLog::sendHistory(uint16_t max) {
    sfx::Stream* serial = _serial.load(std::memory_order_acquire);
    if (!serial) return 0;

    // Snapshot indices — atomics provide acquire barrier, no mutex needed
    uint16_t readPos = _tail.load(std::memory_order_acquire);
    uint16_t endPos = _head.load(std::memory_order_acquire);

    // Total entries currently held; honour `max` by skipping forward
    // to the most recent N (tail-style trimming) so the operator sees
    // newest output instead of stale boot junk on a tiny request.
    uint16_t available = (endPos >= readPos)
        ? (endPos - readPos)
        : (uint16_t)(RING_SIZE - readPos + endPos);
    if (max > 0 && available > max) {
        uint16_t skip = available - max;
        readPos = (uint16_t)((readPos + skip) % RING_SIZE);
    }

    uint16_t sent = 0;
    // Iterate from oldest-kept to newest (head-1) without advancing _tail.
    // USB CDC backpressure: yield every 16 packets so the host has time
    // to drain.  A full ESP32 ring (512 × ~140 bytes ≈ 71 KB) used to
    // saturate the CDC buffer and look like a hang to the Go client —
    // the yield turns that into a steady drip the reader can keep up
    // with, and the Go side's 15 s ceiling has comfortable margin.
    constexpr uint16_t kYieldEvery = 16;
    while (readPos != endPos) {
        const LogEntry& entry = _ring[readPos];

        // Build payload: [level:u8][millis:u32LE][message:str]
        uint8_t payload[1 + 4 + MAX_MSG_LEN];
        payload[0] = entry.level;
        putU32LE(&payload[1], entry.timestamp_ms);
        memcpy(&payload[5], entry.message, entry.len);
        size_t payloadLen = 5 + entry.len;

        // Encode as COBS packet with TAG_ASYNC
        uint8_t buffer[COBS_BUFFER_SIZE];
        size_t encodedLen = encodePacket(buffer, _packetType, TAG_ASYNC, payload, payloadLen);
        if (encodedLen > 0) {
            serial->write(buffer, encodedLen);
        }

        readPos = (readPos + 1) % RING_SIZE;
        sent++;
        if ((sent % kYieldEvery) == 0) {
            // 1 ms is enough to wake the USB CDC writer task on both
            // ESP32-S3 (FreeRTOS) and RP2040 (Pico-SDK polling loop).
            SFX_DELAY_MS(1);
        }
    }

    return sent;
}

// ============================================================================
// ESP-IDF Log Capture — Redirect ESP_LOGx into DiagLog ring buffer
// ============================================================================

#ifdef ESP32
#include <esp_log.h>

/**
 * @brief Custom vprintf callback installed by captureEspLog().
 *
 * ESP-IDF formats log output as: "X (timestamp) tag: message\n"
 * where X = E/W/I/D/V. We parse the first character to determine
 * the DiagLevel, format the full string, strip trailing newline,
 * and ingest into the ring buffer.
 *
 * Special care:
 * - This runs on whatever core called ESP_LOGx (could be Core 0 or 1)
 * - ingest() is mutex-protected, so it's safe cross-core
 * - Messages before DiagLog::begin() are silently dropped (ingest checks _serial)
 * - Output is NOT echoed to UART0 — that's the whole point
 */
static int diagLogVprintf(const char* fmt, va_list args) {
    // Format into a local buffer
    char buf[160];  // ESP-IDF messages are typically <120 chars
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len <= 0) return 0;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;

    // Strip trailing newline(s)
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len == 0) return 0;

    // Parse log level from first character (ESP-IDF format: "X (ts) tag: msg")
    uint8_t level = DiagLevel::INFO;
    switch (buf[0]) {
        case 'E': level = DiagLevel::ERR;   break;
        case 'W': level = DiagLevel::WARN;  break;
        case 'I': level = DiagLevel::INFO;  break;
        case 'D': level = DiagLevel::DEBUG; break;
        case 'V': level = DiagLevel::DEBUG; break;  // Verbose → DEBUG
    }

    // Prefix with [IDF] to distinguish from SFX_LOG_* messages
    char tagged[172];
    snprintf(tagged, sizeof(tagged), "[IDF] %s", buf);

    DiagLog::instance().ingest(level, tagged);
    return len;
}

void DiagLog::captureEspLog() {
    esp_log_set_vprintf(diagLogVprintf);
}

#endif // ESP32

#endif // SFX_ENABLE_DIAG_LOG
