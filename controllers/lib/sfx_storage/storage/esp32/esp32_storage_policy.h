/*
 * Esp32StoragePolicy — ESP32-S3 Dual-Core Storage Policy
 *
 * Platform policy for StorageServerT<>. Provides ESP32-specific
 * implementations:
 *   - PSRAM double-buffered chunked uploads (Core 0 fills, Core 1 writes)
 *   - PSRAM ring buffer + FreeRTOS writer task for stream uploads
 *   - PSRAM staging buffers (128 KB for stream, 512 KB for chunked)
 *
 * Included automatically from storage_server.h — do not include directly.
 * See storage_server.h for the full public API.
 */

#ifndef ESP32_STORAGE_POLICY_H
#define ESP32_STORAGE_POLICY_H

#ifndef STORAGE_SERVER_H
#error "Include <storage/storage_server.h> instead of this file directly"
#endif

#include <platform/spsc_ring_buffer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <atomic>

/// 1 MB byte-oriented SPSC ring buffer for streaming uploads (PSRAM)
static constexpr uint32_t STREAM_RING_CAPACITY = 1u << 20;  // 1,048,576 bytes
using StreamRingBuffer = SpscRingBuffer<uint8_t, STREAM_RING_CAPACITY>;


class Esp32StoragePolicy {
public:
    Esp32StoragePolicy() = default;

    /// Bind to shared state (called by StorageServerT constructor)
    void init(StorageSharedState* state) { _state = state; }

    // -----------------------------------------------------------------
    // Platform hooks (called by StorageServerT<> via _policy member)
    // -----------------------------------------------------------------

    // --- Buffer management ---
    bool allocateUploadBuffers();
    void freeUploadBuffers();
    bool allocateStreamBuffers();
    void freeStreamBuffers();

    // --- Upload data handling ---
    bool onUploadBufferFull();
    bool checkAsyncWriterHealth();

    // --- Upload lifecycle ---
    void onUploadActivated(bool isStream);
    bool onStreamStart();
    bool onStreamEnd(const char*& errMsg);
    bool onChunkedEnd(const char*& errMsg);
    void onStreamCleanup();
    void onChunkedCleanup();

    // --- Stream data processing ---
    void onStreamDataReceived(const uint8_t* data, size_t len);
    void onStreamReceiveComplete();
    size_t streamBufferCapacityForLog() const;

    // -----------------------------------------------------------------
    // ESP32-specific public API (accessed via storageServer.policy())
    // -----------------------------------------------------------------

    /**
     * @brief Start the background writer task (ESP32 only)
     *
     * Launches a FreeRTOS task pinned to Core 1 that performs
     * file.write() calls asynchronously. Must be called once
     * during setup(), before any uploads.
     *
     * @param stackSize Stack size for the writer task (default 8KB)
     * @param priority  Task priority (default 2, below audio)
     */
    void startWriterTask(uint32_t stackSize = 8192, UBaseType_t priority = 2);

private:
    StorageSharedState* _state = nullptr;

    // --- Buffer size constants ---
    static constexpr size_t UPLOAD_WRITE_BUF_SIZE     = 524288;   // 512 KB (PSRAM)
    static constexpr size_t UPLOAD_WRITE_BUF_FALLBACK = 65536;    // 64 KB (internal RAM)
    static constexpr size_t STREAM_WRITE_CHUNK        = 131072;   // 128 KB (PSRAM)

    // --- Double-buffer state (chunked mode) ---
    // Second buffer for double-buffering (writer task reads from this)
    uint8_t* _uploadWriteBuf2 = nullptr;

    // --- Chunked writer task (persistent, semaphore-driven) ---
    TaskHandle_t      _writerTask       = nullptr;
    SemaphoreHandle_t _writerDataReady  = nullptr;  // Signaled when buf2 has data
    SemaphoreHandle_t _writerDone       = nullptr;  // Signaled when buf2 write complete
    std::atomic<size_t> _writerBufLen{0};            // Length of data in buf2
    std::atomic<bool>   _writerError{false};         // True if last write failed
    std::atomic<bool>   _writerActive{false};        // True while writer task exists

    // --- Stream ring buffer + writer task (per-upload) ---
    StreamRingBuffer  _streamRingBuf;                   // Core 0 → Core 1 ring buffer
    std::atomic<bool> _streamDrainComplete{false};      // Core 0 done, writer must finish

    TaskHandle_t      _streamWriterTask   = nullptr;
    SemaphoreHandle_t _streamWriterDone   = nullptr;    // Signaled when drain complete
    std::atomic<bool> _streamWriterActive{false};       // Controls task loop lifetime

    /// SD write rate measurement (stream writer populates, DiagLog exposes)
    std::atomic<uint32_t> _streamSdWriteRate_KBps{0};   // Latest SD write rate

    // --- Internal worker methods ---
    bool submitWriteBuffer();
    bool waitWriterDone();
    static void writerTaskFunc(void* param);
    static void streamWriterTaskFunc(void* param);
    bool startStreamWriterTask();
    void stopStreamWriterTask();
};

#endif // ESP32_STORAGE_POLICY_H
