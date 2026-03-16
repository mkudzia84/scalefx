/*
 * StorageServerPico — Pico Single-Core Storage Implementation
 *
 * Platform-specific overrides for StorageServerBase:
 *
 *   Buffer allocation:
 *     - Single 16 KB heap buffer for chunked uploads (blocking writes)
 *     - Single 16 KB staging buffer for stream uploads (inline flush)
 *
 *   Writes:
 *     - All writes are blocking on Core 0 (no async writer tasks)
 *     - Buffer-full triggers immediate flushUploadBuffer() call
 *     - Stream data accumulates in staging, flushed when full
 *
 * See storage_server.cpp for the platform-agnostic protocol handlers.
 */

#include <platform/sfx_platform.h>

#if !SFX_PLATFORM_ESP32

#include "storage_server.h"
#include <platform/diag_log.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// Buffer Allocation (Single heap buffer)
// ============================================================================

bool StorageServerPico::allocateUploadBuffers() {
    if (_uploadWriteBuf) return true;  // Already allocated

    _uploadWriteBuf = (uint8_t*)malloc(UPLOAD_WRITE_BUF_SIZE);
    if (!_uploadWriteBuf) {
        STORAGE_LOG("Failed to allocate upload buffer (%u KB)",
                    (unsigned)(UPLOAD_WRITE_BUF_SIZE / 1024));
        return false;
    }

    _uploadBufCapacity = UPLOAD_WRITE_BUF_SIZE;
    _uploadWriteBufLen = 0;

    STORAGE_LOG("Allocated upload buffer: %u KB", (unsigned)(UPLOAD_WRITE_BUF_SIZE / 1024));
    return true;
}

void StorageServerPico::freeUploadBuffers() {
    free(_uploadWriteBuf);
    _uploadWriteBuf = nullptr;
    _uploadWriteBufLen = 0;
    _uploadBufCapacity = 0;
}


// ============================================================================
// Stream Buffer Allocation (Staging only, no ring buffer)
// ============================================================================

bool StorageServerPico::allocateStreamBuffers() {
    _streamStaging = (uint8_t*)malloc(STREAM_WRITE_CHUNK);
    if (!_streamStaging) {
        STORAGE_LOG("Failed to allocate stream staging buffer (%u KB)",
                    (unsigned)(STREAM_WRITE_CHUNK / 1024));
        return false;
    }

    _streamStagingLen = 0;
    STORAGE_LOG("Stream staging buffer allocated: %u KB", (unsigned)(STREAM_WRITE_CHUNK / 1024));
    return true;
}

void StorageServerPico::freeStreamBuffers() {
    free(_streamStaging);
    _streamStaging = nullptr;
    _streamStagingLen = 0;
    _streamReceiving = false;
}


// ============================================================================
// Upload Data Handling (Blocking flush)
// ============================================================================

bool StorageServerPico::onUploadBufferFull() {
    // Single-core: blocking write on Core 0
    return flushUploadBuffer();
}


// ============================================================================
// Stream Data Processing (Staging + inline SD flush)
// ============================================================================

void StorageServerPico::onStreamDataReceived(const uint8_t* data, size_t len) {
    // Accumulate into staging buffer, flush to SD when full
    size_t offset = 0;
    while (offset < len) {
        size_t space = STREAM_WRITE_CHUNK - _streamStagingLen;
        size_t chunk = (len - offset < space) ? (len - offset) : space;

        memcpy(_streamStaging + _streamStagingLen, data + offset, chunk);
        _streamStagingLen += chunk;
        offset += chunk;

        // Flush when staging is full
        if (_streamStagingLen >= STREAM_WRITE_CHUNK) {
            size_t written = _uploadFile.write(_streamStaging, _streamStagingLen);
            if (written != _streamStagingLen) {
                STORAGE_LOG("Stream staging flush failed: expected %u wrote %u",
                            (unsigned)_streamStagingLen, (unsigned)written);
                _streamWriteError = true;
                return;
            }
            _streamBytesWrittenToSD += written;
            _streamStagingLen = 0;
        }
    }
}

void StorageServerPico::onStreamReceiveComplete() {
    STORAGE_LOG("Stream receive complete: %lu bytes written to SD so far, "
                "%u bytes remaining in staging",
                (unsigned long)_streamBytesWrittenToSD,
                (unsigned)_streamStagingLen);
}

bool StorageServerPico::onStreamEnd(const char*& errMsg) {
    // Flush any remaining data in staging buffer
    if (_streamStagingLen > 0) {
        size_t written = _uploadFile.write(_streamStaging, _streamStagingLen);
        if (written != _streamStagingLen) {
            STORAGE_LOG("UPLOAD_END stream: final staging flush failed");
            errMsg = "Final staging write failed";
            return false;
        }
        _streamBytesWrittenToSD += written;
        _streamStagingLen = 0;
    }

    STORAGE_LOG("UPLOAD_END stream: %lu bytes total to SD",
                (unsigned long)_streamBytesWrittenToSD);
    return true;
}

size_t StorageServerPico::streamBufferCapacityForLog() const {
    return STREAM_WRITE_CHUNK;
}

#endif  // !SFX_PLATFORM_ESP32
