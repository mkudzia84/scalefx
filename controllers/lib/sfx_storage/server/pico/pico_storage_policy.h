/*
 * PicoStoragePolicy — RP2040/RP2350 Single-Core Storage Implementation
 *
 * Trivial policy for single-core Pico targets:
 *   - Single heap buffer (16 KB) for chunked uploads
 *   - Blocking inline writes (no async offloading)
 *   - Direct staging buffer (16 KB) for stream uploads
 *
 * Most hooks are trivial inlines — the Pico has no FreeRTOS tasks,
 * no PSRAM, and no dual-core writer offloading.
 *
 * See storage_server.ipp for the platform-agnostic protocol handlers.
 */

#ifndef PICO_STORAGE_POLICY_H
#define PICO_STORAGE_POLICY_H

#include <cstdint>
#include <cstddef>

struct StorageSharedState;  // Forward declaration

class PicoStoragePolicy {
public:
    // --- Constants ---
    static constexpr size_t UPLOAD_WRITE_BUF_SIZE = 16384;   // 16 KB
    static constexpr size_t STREAM_WRITE_CHUNK    = 16384;   // 16 KB

    // --- Initialization ---
    void init(StorageSharedState* state) { _state = state; }

    // --- Buffer lifecycle ---
    bool allocateUploadBuffers();
    void freeUploadBuffers();
    bool allocateStreamBuffers();
    void freeStreamBuffers();

    // --- Upload data handling ---
    bool onUploadBufferFull();

    /// No async writer — always healthy
    bool checkAsyncWriterHealth() { return true; }

    // --- Upload lifecycle (trivial on Pico) ---
    void onUploadActivated(bool /*isStream*/) {}
    bool onStreamStart()                      { return true; }
    bool onStreamEnd(const char*& /*errMsg*/) { return true; }
    bool onChunkedEnd(const char*& /*errMsg*/){ return true; }
    void onStreamCleanup()                    {}
    void onChunkedCleanup()                   {}

    // --- Stream data processing ---
    void onStreamDataReceived(const uint8_t* data, size_t len);
    void onStreamReceiveComplete();
    size_t streamBufferCapacityForLog() const { return STREAM_WRITE_CHUNK; }

private:
    StorageSharedState* _state = nullptr;
};

#endif // PICO_STORAGE_POLICY_H
