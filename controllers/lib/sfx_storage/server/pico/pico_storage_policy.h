/*
 * PicoStoragePolicy — RP2040/RP2350 Single-Core Storage Implementation
 *
 * Trivial policy for single-core Pico targets:
 *   - Single heap buffer (16 KB) for chunked uploads
 *   - Blocking inline writes (no async offloading)
 *
 * Most hooks are trivial inlines — the Pico has no FreeRTOS tasks,
 * no PSRAM, and no dual-core writer offloading.
 *
 * See storage_service.ipp for the platform-agnostic protocol handlers.
 */

#ifndef PICO_STORAGE_POLICY_H
#define PICO_STORAGE_POLICY_H

#include <cstdint>
#include <cstddef>
#include <serial/core/core.h>  // SerialError::NOT_SUPPORTED

// LFSFile and StorageSharedState are declared in headers already included
// by storage_service.h before this file (flash.h + storage_service.h itself).

struct StorageSharedState;  // Forward declaration

class PicoStoragePolicy {
public:
    // --- Constants ---
    static constexpr size_t UPLOAD_WRITE_BUF_SIZE = 16384;   // 16 KB

    // SD card uploads/downloads are not supported on Pico StorageServer builds.
    // Reason: SdCardModule's FileHandle is SdFat's File32 on Pico, while the
    // shared upload-buffer file handle is LittleFS's fs::File. Supporting both
    // would require a union/variant on _shared.uploadFile. Pico boards only
    // need flash (config YAML round-trip), so we gate SD out here.
    static constexpr bool SdSupported = false;

    // --- Initialization ---
    void init(StorageSharedState* state) { _state = state; }

    // --- Buffer lifecycle ---
    bool allocateUploadBuffers();
    void freeUploadBuffers();

    // --- Upload data handling ---
    bool onUploadBufferFull();

    /// No async writer — always healthy
    bool checkAsyncWriterHealth() { return true; }

    /// SD open helpers — always return NOT_SUPPORTED on Pico. Pico boards
    /// only expose flash (LittleFS) through StorageServer; SD card upload/
    /// download paths take a different file-handle type and are disabled.
    uint8_t sdOpenRead(const char* /*path*/, LFSFile& /*file*/) {
        return SerialError::NOT_SUPPORTED;
    }
    uint8_t sdOpenWrite(const char* /*path*/, LFSFile& /*file*/, bool /*truncate*/) {
        return SerialError::NOT_SUPPORTED;
    }

    // --- Upload lifecycle (trivial on Pico) ---
    void onUploadActivated() {}
    bool onChunkedEnd(const char*& /*errMsg*/){ return true; }
    void onChunkedCleanup()                   {}

    // --- Buffer diagnostics ---
    /// Fill buffer percentage (0-100) for segment ACK reporting.
    uint8_t bufferFillPercent() const {
        return (_state->uploadBufCapacity > 0)
            ? (uint8_t)((_state->uploadWriteBufLen * 100) / _state->uploadBufCapacity)
            : 0;
    }

    /// Writer stats — Pico writes are synchronous so nothing interesting to
    /// report. Returned zeros just make the shared UPLOAD_END log harmless.
    struct WriterStats {
        uint32_t bytesWritten = 0;
        uint32_t writeCount = 0;
        uint32_t totalStallTime_ms = 0;
        uint32_t maxWriteLatency_ms = 0;
    };
    WriterStats writerStats() const { return {}; }

private:
    StorageSharedState* _state = nullptr;
};

#endif // PICO_STORAGE_POLICY_H
