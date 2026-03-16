/*
 * StorageServerPico — Pico Single-Core Storage Server
 *
 * Derived from StorageServerBase. Provides Pico-specific implementations:
 *   - Single heap buffer for chunked uploads (blocking writes)
 *   - Staging buffer for stream uploads (inline SD flush when full)
 *   - No async writer tasks (single-core, all writes on Core 0)
 *
 * Included automatically from storage_server.h — do not include directly.
 * See storage_server.h for the full public API (inherited from base class).
 */

#ifndef STORAGE_SERVER_PICO_H
#define STORAGE_SERVER_PICO_H

#ifndef STORAGE_SERVER_H
#error "Include <storage/storage_server.h> instead of this file directly"
#endif

class StorageServerPico : public StorageServerBase {
protected:
    // --- Buffer management ---
    bool allocateUploadBuffers() override;
    void freeUploadBuffers() override;
    bool allocateStreamBuffers() override;
    void freeStreamBuffers() override;

    // --- Upload data handling ---
    bool onUploadBufferFull() override;

    // --- Stream data processing ---
    void onStreamDataReceived(const uint8_t* data, size_t len) override;
    void onStreamReceiveComplete() override;
    bool onStreamEnd(const char*& errMsg) override;
    size_t streamBufferCapacityForLog() const override;

private:
    static constexpr size_t UPLOAD_WRITE_BUF_SIZE = 16384;   // 16 KB (Pico heap)
    static constexpr size_t STREAM_WRITE_CHUNK    = 16384;   // 16 KB (Pico heap)
};

using StorageServer = StorageServerPico;

#endif // STORAGE_SERVER_PICO_H
