/*
 * Serial Stream — Chunked Data Streaming Over COBS
 *
 * Provides StreamWriter for sending data larger than MAX_PAYLOAD_SIZE.
 * Data is broken into segments with sequence numbers and CRC-16 checksums,
 * enabling integrity checking and progress monitoring.
 *
 * This is a reusable library component — any code with a
 * `sfx_core::BoardServerBase&` (typically inside a system-service policy
 * handler) can use StreamWriter to stream arbitrary data to the client.
 *
 * Streaming wire format (3 packet types, chosen by caller):
 *   STREAM_BEGIN: [totalBytes:u32LE]            (0 = unknown size)
 *   STREAM_DATA:  [seqNum:u16LE][crc16:u16LE][data:0-N bytes]
 *   STREAM_END:   [totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]
 *
 * N = MAX_PAYLOAD_SIZE - 4 (chunk header). Platform-specific:
 *     Pico:  508 bytes/segment  (MAX_PAYLOAD_SIZE = 512)
 *     ESP32: 2044 bytes/segment (MAX_PAYLOAD_SIZE = 2048)
 * CRC-16 uses CCITT polynomial (0x1021), init 0xFFFF.
 *
 * Usage:
 *   // In a policy handler (uses default STREAM_BEGIN/DATA/END types):
 *   StreamWriter stream(*_ctx, currentTag());
 *   stream.begin(totalSize);   // announce size (0 = unknown)
 *   stream.write(data, len);   // auto-chunks when buffer fills
 *   stream.printf("line: %s\n", name);
 *   stream.end();              // flush + send END
 *
 *   // Or with custom packet types (e.g., controller-specific range):
 *   StreamWriter stream(*_ctx, currentTag(), MY_BEGIN, MY_DATA, MY_END);
 */

#ifndef SERIAL_STREAM_H
#define SERIAL_STREAM_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <serial/core/core.h>

// ============================================================================
// Stream Protocol Constants
// ============================================================================

namespace StreamProtocol {

    // --- Default Streaming Packet Types (0xA4-0xA6) ---
    // These are protocol-level infrastructure. Any controller can use
    // StreamWriter with these default types, or supply custom ones.
    constexpr uint8_t STREAM_BEGIN = 0xA4;  // [totalBytes:u32LE] (0 = unknown)
    constexpr uint8_t STREAM_DATA  = 0xA5;  // [seqNum:u16LE][crc16:u16LE][data:N]
    constexpr uint8_t STREAM_END   = 0xA6;  // [totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]

    /// Per-chunk header size: [seqNum:u16LE][crc16:u16LE]
    constexpr size_t CHUNK_HEADER_SIZE = 4;

    /// Maximum data bytes per STREAM_DATA chunk
    constexpr size_t MAX_CHUNK_DATA = SfxWire::MAX_PAYLOAD_SIZE - CHUNK_HEADER_SIZE;

    /**
     * @brief CRC-16/CCITT checksum
     *
     * Polynomial 0x1021, initial value 0xFFFF.
     * Used for per-segment integrity and full-stream verification.
     *
     * @param data Input bytes
     * @param len  Data length
     * @return CRC-16 value
     */
    uint16_t crc16(const uint8_t* data, size_t len);

    /**
     * @brief Update a running CRC-16 with additional data
     *
     * @param crc Current CRC value (start with 0xFFFF)
     * @param data Input bytes
     * @param len  Data length
     * @return Updated CRC-16
     */
    uint16_t crc16_update(uint16_t crc, const uint8_t* data, size_t len);

}  // namespace StreamProtocol


// ============================================================================
// StreamWriter — Sends Chunked Data Over COBS
// ============================================================================

/**
 * @brief Writes data as a sequence of COBS packets with CRC integrity
 *
 * Accumulates data in an internal buffer and automatically sends a
 * STREAM_DATA packet when the buffer fills. Call end() to flush
 * remaining data and send the STREAM_END marker.
 *
 * All packets use the same tag for correlation. The receiver gets:
 *   BEGIN (with size) → DATA chunks → END (with verification)
 *
 * Thread safety: NOT thread-safe internally. Caller must ensure
 * exclusive access to the BoardServerBase's serial port during streaming.
 */
namespace sfx_core { class BoardServerBase; }

class StreamWriter {
public:
    /**
     * @brief Construct a StreamWriter with default packet types
     *
     * Uses StreamProtocol::STREAM_BEGIN/DATA/END (0xA4-0xA6).
     *
     * @param sink BoardServerBase (or anything exposing sendRawPacket) to
     *             route packets through
     * @param tag  Correlation tag for all stream packets
     */
    StreamWriter(sfx_core::BoardServerBase& sink, uint8_t tag)
        : StreamWriter(sink, tag,
                       StreamProtocol::STREAM_BEGIN,
                       StreamProtocol::STREAM_DATA,
                       StreamProtocol::STREAM_END) {}

    /**
     * @brief Construct a StreamWriter with custom packet types
     */
    StreamWriter(sfx_core::BoardServerBase& sink, uint8_t tag,
                 uint8_t beginPacketType, uint8_t dataPacketType, uint8_t endPacketType);

    /// Destructor — frees heap-allocated chunk buffer
    ~StreamWriter();

    // Non-copyable, non-movable (owns heap buffer)
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;
    StreamWriter(StreamWriter&&) = delete;
    StreamWriter& operator=(StreamWriter&&) = delete;

    /**
     * @brief Send STREAM_BEGIN to announce the stream
     *
     * Should be called before any write(). Sends a BEGIN packet
     * with the expected total data size (0 if unknown).
     *
     * @param totalBytes Expected total data size (0 = unknown)
     */
    void begin(uint32_t totalBytes = 0);

    /**
     * @brief Write raw bytes (auto-chunks when buffer fills)
     *
     * @param data Source data
     * @param len  Number of bytes
     * @return true if all data written successfully
     */
    bool write(const uint8_t* data, size_t len);

    /**
     * @brief Write null-terminated string
     *
     * @param str String to write (strlen determines length)
     * @return true if written successfully
     */
    bool writeString(const char* str);

    /**
     * @brief Printf-style formatted write
     *
     * Uses a 256-byte internal format buffer. Output exceeding 255
     * characters is truncated.
     *
     * @param fmt Format string
     * @return true if written successfully
     */
    bool printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    /**
     * @brief Flush buffer and send STREAM_END
     *
     * Sends any remaining buffered data as a final DATA chunk,
     * then sends the END packet with total segment count,
     * total byte count, and full-stream CRC-16.
     *
     * @return true if completed successfully
     */
    bool end();

    /// Number of DATA segments sent so far
    uint16_t segmentCount() const { return _seqNum; }

    /// Total data bytes sent so far (excluding chunk headers)
    uint32_t totalBytes() const { return _totalBytes; }

private:
    /// Flush current buffer as a STREAM_DATA packet
    bool flush();

    sfx_core::BoardServerBase& _server;
    uint8_t _tag;
    uint8_t _beginType;
    uint8_t _dataType;
    uint8_t _endType;
    uint16_t _seqNum;
    uint32_t _totalBytes;
    uint16_t _crcAll;   ///< Running CRC-16 across ALL data in the stream
    uint8_t* _buf;      ///< Heap-allocated chunk buffer (MAX_CHUNK_DATA bytes)
    size_t _bufLen;
};

#endif // SERIAL_STREAM_H
