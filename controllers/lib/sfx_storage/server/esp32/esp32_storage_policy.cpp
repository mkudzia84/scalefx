/*
 * Esp32StoragePolicy — ESP32-S3 Single-Core Storage Implementation
 *
 * Platform-specific methods for StorageServerT<Esp32StoragePolicy>:
 *
 *   Buffer allocation (per upload):
 *     - 64 KB PSRAM fill buffer (falls back to internal RAM if PSRAM fails).
 *
 *   Data path (Core 0 only):
 *     - UPLOAD_DATA chunks → fill buffer → uploadFile.write() (blocking).
 *     - No ring buffer, no staging, no writer task, no drain semaphore.
 *
 * Rationale: the dual-core ring+writer pipeline was removed after it
 * correlated with UPLOAD_END drain hangs. The single-core path saturates
 * the 6 Mbps UART (~470 KB/s SD batch/stream). See the header for
 * HubFX exclusivity contract details.
 *
 * See storage_service.ipp for the platform-agnostic protocol handlers.
 */

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_STORAGE_SERVER)

#include <server/storage_service.h>
#include <serial/diag_log.h>
#include <esp_heap_caps.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// SD open helpers (route through policy so SdCardModule::FileHandle stays
// out of the shared ipp)
// ============================================================================

uint8_t Esp32StoragePolicy::sdOpenRead(const char* path, LFSFile& file) {
    return SdCardModule::instance().openRead(path, file);
}

uint8_t Esp32StoragePolicy::sdOpenWrite(const char* path, LFSFile& file, bool truncate) {
    return SdCardModule::instance().openWrite(path, file, truncate);
}


// ============================================================================
// Buffer Allocation (64 KB fill buffer — PSRAM with internal-RAM fallback)
// ============================================================================

bool Esp32StoragePolicy::allocateUploadBuffers() {
    if (_state->uploadWriteBuf) return true;  // Already allocated

    _state->uploadWriteBuf = (uint8_t*)heap_caps_malloc(UPLOAD_FILL_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (_state->uploadWriteBuf) {
        _state->uploadBufCapacity = UPLOAD_FILL_BUF_SIZE;
        STORAGE_LOG("Allocated PSRAM fill buffer: %u KB (free PSRAM: %u KB)",
                    (unsigned)(UPLOAD_FILL_BUF_SIZE / 1024),
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        _state->uploadWriteBuf = (uint8_t*)malloc(UPLOAD_FILL_BUF_FALLBACK);
        if (!_state->uploadWriteBuf) {
            STORAGE_LOG("Failed to allocate fill buffer (%u KB PSRAM, %u KB internal)",
                        (unsigned)(UPLOAD_FILL_BUF_SIZE / 1024),
                        (unsigned)(UPLOAD_FILL_BUF_FALLBACK / 1024));
            return false;
        }
        _state->uploadBufCapacity = UPLOAD_FILL_BUF_FALLBACK;
        STORAGE_LOG("PSRAM unavailable — using internal RAM fill buffer: %u KB",
                    (unsigned)(UPLOAD_FILL_BUF_FALLBACK / 1024));
    }

    _state->uploadWriteBufLen = 0;
    return true;
}

void Esp32StoragePolicy::freeUploadBuffers() {
    free(_state->uploadWriteBuf);
    _state->uploadWriteBuf = nullptr;
    _state->uploadWriteBufLen = 0;
    _state->uploadBufCapacity = 0;
}


// ============================================================================
// Upload Data Handling (blocking inline write on Core 0)
// ============================================================================

bool Esp32StoragePolicy::onUploadBufferFull() {
    if (_state->uploadWriteBufLen == 0) return true;

    uint32_t t0 = millis();
    size_t toWrite = _state->uploadWriteBufLen;
    size_t written = _state->uploadFile.write(_state->uploadWriteBuf, toWrite);
    uint32_t latency = millis() - t0;

    if (written != toWrite) {
        STORAGE_LOG("SD write FAILED: wanted %u wrote %u (lat=%lums)",
                    (unsigned)toWrite, (unsigned)written, (unsigned long)latency);
        _state->uploadWriteBufLen = 0;
        return false;
    }

    _bytesWritten += written;
    _writeCount++;
    _totalStall_ms += latency;
    if (latency > _maxLatency_ms) _maxLatency_ms = latency;

    if (latency > 50) {
        STORAGE_LOG("SD spike: %uKB in %lums (%luKB/s)",
                    (unsigned)(written / 1024),
                    (unsigned long)latency,
                    (unsigned long)(latency > 0 ? written / latency : 0));
    }

    _state->uploadWriteBufLen = 0;
    return true;
}


// ============================================================================
// Upload Lifecycle Hooks
// ============================================================================

void Esp32StoragePolicy::onUploadActivated() {
    _bytesWritten  = 0;
    _writeCount    = 0;
    _maxLatency_ms = 0;
    _totalStall_ms = 0;
}

bool Esp32StoragePolicy::onChunkedEnd(const char*& /*errMsg*/) {
    // No async writer — per-buffer writes already landed on disk
    // inside onUploadBufferFull(). storage_service.ipp will flush any
    // final partial buffer after this returns.
    return true;
}


// ============================================================================
// Buffer Diagnostics
// ============================================================================

uint8_t Esp32StoragePolicy::bufferFillPercent() const {
    if (_state->uploadBufCapacity == 0) return 0;
    return (uint8_t)((_state->uploadWriteBufLen * 100) / _state->uploadBufCapacity);
}


#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_STORAGE_SERVER
