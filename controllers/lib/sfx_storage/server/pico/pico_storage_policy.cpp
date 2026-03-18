/*
 * PicoStoragePolicy — RP2040/RP2350 Single-Core Storage Implementation
 *
 * Simple blocking writes with single heap buffers.
 * See pico_storage_policy.h for the policy interface.
 */

#include <platform/sfx_platform.h>

#if !SFX_PLATFORM_ESP32

#include <server/storage_server.h>
#include <platform/diag_log.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// Buffer Allocation (single heap buffers)
// ============================================================================

bool PicoStoragePolicy::allocateUploadBuffers() {
    if (_state->uploadWriteBuf) return true;  // Already allocated

    _state->uploadWriteBuf = (uint8_t*)malloc(UPLOAD_WRITE_BUF_SIZE);
    if (!_state->uploadWriteBuf) {
        STORAGE_LOG("Failed to allocate upload buffer (%u bytes)", (unsigned)UPLOAD_WRITE_BUF_SIZE);
        return false;
    }

    _state->uploadBufCapacity = UPLOAD_WRITE_BUF_SIZE;
    _state->uploadWriteBufLen = 0;
    STORAGE_LOG("Allocated upload buffer: %u KB", (unsigned)(UPLOAD_WRITE_BUF_SIZE / 1024));
    return true;
}

void PicoStoragePolicy::freeUploadBuffers() {
    free(_state->uploadWriteBuf);
    _state->uploadWriteBuf = nullptr;
    _state->uploadWriteBufLen = 0;
    _state->uploadBufCapacity = 0;
}


// ============================================================================
// Stream Buffer Allocation (staging buffer only)
// ============================================================================

bool PicoStoragePolicy::allocateStreamBuffers() {
    _state->streamStaging = (uint8_t*)malloc(STREAM_WRITE_CHUNK);
    if (!_state->streamStaging) {
        STORAGE_LOG("Failed to allocate stream staging (%u bytes)", (unsigned)STREAM_WRITE_CHUNK);
        return false;
    }
    _state->streamStagingLen = 0;
    STORAGE_LOG("Stream staging allocated: %u KB", (unsigned)(STREAM_WRITE_CHUNK / 1024));
    return true;
}

void PicoStoragePolicy::freeStreamBuffers() {
    if (_state->streamStaging) {
        free(_state->streamStaging);
        _state->streamStaging = nullptr;
    }
    _state->streamStagingLen = 0;
    _state->streamReceiving = false;
}


// ============================================================================
// Upload Data Handling (blocking write)
// ============================================================================

bool PicoStoragePolicy::onUploadBufferFull() {
    return _state->flushUploadBuffer();
}


// ============================================================================
// Stream Data Processing (staging buffer → SD)
// ============================================================================

void PicoStoragePolicy::onStreamDataReceived(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t space = STREAM_WRITE_CHUNK - _state->streamStagingLen;
        size_t toCopy = (len - offset < space) ? (len - offset) : space;

        memcpy(_state->streamStaging + _state->streamStagingLen, data + offset, toCopy);
        _state->streamStagingLen += toCopy;
        offset += toCopy;

        // Flush staging when full
        if (_state->streamStagingLen >= STREAM_WRITE_CHUNK) {
            size_t written = _state->uploadFile.write(_state->streamStaging, _state->streamStagingLen);
            if (written != _state->streamStagingLen) {
                STORAGE_LOG("Stream write failed: expected %u wrote %u",
                            (unsigned)_state->streamStagingLen, (unsigned)written);
                _state->streamWriteError = true;
                return;
            }
            _state->streamBytesWrittenToSD += written;
            _state->streamStagingLen = 0;
        }
    }
}

void PicoStoragePolicy::onStreamReceiveComplete() {
    // Flush remaining staging data
    if (_state->streamStagingLen > 0) {
        size_t written = _state->uploadFile.write(_state->streamStaging, _state->streamStagingLen);
        if (written != _state->streamStagingLen) {
            STORAGE_LOG("Final stream write failed: expected %u wrote %u",
                        (unsigned)_state->streamStagingLen, (unsigned)written);
            _state->streamWriteError = true;
            return;
        }
        _state->streamBytesWrittenToSD += written;
        _state->streamStagingLen = 0;
    }
    STORAGE_LOG("Stream receive complete: %lu bytes written to SD",
                (unsigned long)_state->streamBytesWrittenToSD);
}

#endif  // !SFX_PLATFORM_ESP32
