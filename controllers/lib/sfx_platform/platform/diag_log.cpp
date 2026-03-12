/*
 * DiagLog — Diagnostic Log Output Implementation
 *
 * See diag_log.h for documentation.
 */

#include "diag_log.h"

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
    entry.timestamp_ms = millis();
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

    sfxMutexUnlock(_mutex);
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
    entry.timestamp_ms = millis();  // re-stamp with local time
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

    sfxMutexUnlock(_mutex);
}

// ============================================================================
// Send History — Send buffered messages WITHOUT draining (for DIAG_HISTORY)
// ============================================================================

uint16_t DiagLog::sendHistory() {
    Stream* serial = _serial.load(std::memory_order_acquire);
    if (!serial) return 0;

    // Snapshot indices — atomics provide acquire barrier, no mutex needed
    uint16_t readPos = _tail.load(std::memory_order_acquire);
    uint16_t endPos = _head.load(std::memory_order_acquire);
    uint16_t sent = 0;

    // Iterate from oldest (tail) to newest (head-1) without advancing _tail
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
    }

    return sent;
}

#endif // SFX_ENABLE_DIAG_LOG
