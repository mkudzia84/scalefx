/*
 * DiagLog — Diagnostic Log Output Implementation
 *
 * See serial_diag_log.h for documentation.
 */

#include "serial_diag_log.h"

#if SFX_ENABLE_DIAG_LOG

using namespace CoreProtocol;

// ============================================================================
// Log Entry Formatting (mutex-protected, safe from any core)
// ============================================================================

void DiagLog::logv(uint8_t level, const char* fmt, va_list args) {
    if (level < _minLevel) return;
    if (!_serial) return;

    mutex_enter_blocking(&_mutex);

    // Check if ring buffer is full
    uint16_t nextHead = (_head + 1) % RING_SIZE;
    if (nextHead == _tail) {
        _dropped++;
        mutex_exit(&_mutex);
        return;
    }

    // Fill entry
    LogEntry& entry = _ring[_head];
    entry.timestamp_ms = millis();
    entry.level = level;
    int written = vsnprintf(entry.message, MAX_MSG_LEN, fmt, args);
    entry.len = (written < 0) ? 0 : ((size_t)written >= MAX_MSG_LEN ? MAX_MSG_LEN - 1 : (uint8_t)written);

    _head = nextHead;

    mutex_exit(&_mutex);
}

// ============================================================================
// Ingest — Insert pre-formatted message into ring buffer
// ============================================================================

void DiagLog::ingest(uint8_t level, const char* message) {
    if (level < _minLevel) return;
    if (!_serial) return;

    mutex_enter_blocking(&_mutex);

    uint16_t nextHead = (_head + 1) % RING_SIZE;
    if (nextHead == _tail) {
        _dropped++;
        mutex_exit(&_mutex);
        return;
    }

    LogEntry& entry = _ring[_head];
    entry.timestamp_ms = millis();  // re-stamp with local time
    entry.level = level;
    size_t msgLen = strlen(message);
    if (msgLen >= MAX_MSG_LEN) msgLen = MAX_MSG_LEN - 1;
    memcpy(entry.message, message, msgLen);
    entry.message[msgLen] = '\0';
    entry.len = (uint8_t)msgLen;

    _head = nextHead;

    mutex_exit(&_mutex);
}

// ============================================================================
// Flush — Send buffered messages as COBS packets
// ============================================================================

uint16_t DiagLog::flush() {
    if (!_serial) return 0;

    uint16_t flushed = 0;

    while (_tail != _head) {
        const LogEntry& entry = _ring[_tail];

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
            _serial->write(buffer, encodedLen);
        }

        _tail = (_tail + 1) % RING_SIZE;
        flushed++;
    }

    return flushed;
}

#endif // SFX_ENABLE_DIAG_LOG
