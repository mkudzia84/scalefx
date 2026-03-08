/*
 * Serial Stream — Implementation
 *
 * Chunked data streaming over COBS with CRC-16 integrity.
 * See serial_stream.h for full documentation.
 */

#include "serial_stream.h"
#include "serial_bus_server.h"

using namespace CoreProtocol;

// ============================================================================
// CRC-16/CCITT
// ============================================================================

uint16_t StreamProtocol::crc16(const uint8_t* data, size_t len) {
    return crc16_update(0xFFFF, data, len);
}

uint16_t StreamProtocol::crc16_update(uint16_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// ============================================================================
// StreamWriter
// ============================================================================

StreamWriter::StreamWriter(BusServer& server, uint8_t tag,
                           uint8_t beginType, uint8_t dataType, uint8_t endType)
    : _server(server)
    , _tag(tag)
    , _beginType(beginType)
    , _dataType(dataType)
    , _endType(endType)
    , _seqNum(0)
    , _totalBytes(0)
    , _crcAll(0xFFFF)
    , _bufLen(0)
{
}

void StreamWriter::begin(uint32_t totalBytes) {
    // STREAM_BEGIN: [totalBytes:u32LE]
    uint8_t pkt[4];
    putU32LE(pkt, totalBytes);
    _server.sendRawPacket(_beginType, _tag, pkt, 4);
}

bool StreamWriter::write(const uint8_t* data, size_t len) {
    while (len > 0) {
        size_t space = StreamProtocol::MAX_CHUNK_DATA - _bufLen;
        size_t copy = (len < space) ? len : space;
        memcpy(&_buf[_bufLen], data, copy);
        _bufLen += copy;
        data += copy;
        len -= copy;

        if (_bufLen >= StreamProtocol::MAX_CHUNK_DATA) {
            if (!flush()) return false;
        }
    }
    return true;
}

bool StreamWriter::writeString(const char* str) {
    return write(reinterpret_cast<const uint8_t*>(str), strlen(str));
}

bool StreamWriter::printf(const char* fmt, ...) {
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n <= 0) return true;
    if ((size_t)n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    return write(reinterpret_cast<const uint8_t*>(tmp), n);
}

bool StreamWriter::flush() {
    if (_bufLen == 0) return true;

    // Update running CRC across all stream data
    _crcAll = StreamProtocol::crc16_update(_crcAll, _buf, _bufLen);

    // Build segment: [seqNum:u16LE][crc16:u16LE][data...]
    uint8_t pkt[CoreProtocol::MAX_PAYLOAD_SIZE];
    putU16LE(&pkt[0], _seqNum);
    uint16_t chunkCrc = StreamProtocol::crc16(_buf, _bufLen);
    putU16LE(&pkt[2], chunkCrc);
    memcpy(&pkt[StreamProtocol::CHUNK_HEADER_SIZE], _buf, _bufLen);

    int sent = _server.sendRawPacket(_dataType, _tag, pkt,
                                     StreamProtocol::CHUNK_HEADER_SIZE + _bufLen);
    _seqNum++;
    _totalBytes += _bufLen;
    _bufLen = 0;

    return sent > 0;
}

bool StreamWriter::end() {
    // Flush any remaining buffered data
    if (_bufLen > 0) {
        if (!flush()) return false;
    }

    // STREAM_END: [totalSegs:u16LE][totalBytes:u32LE][crc16All:u16LE]
    uint8_t pkt[8];
    putU16LE(&pkt[0], _seqNum);
    putU32LE(&pkt[2], _totalBytes);
    putU16LE(&pkt[6], _crcAll);
    int sent = _server.sendRawPacket(_endType, _tag, pkt, 8);
    return sent > 0;
}
