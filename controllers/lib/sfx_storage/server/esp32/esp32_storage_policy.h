/*
 * Esp32StoragePolicy — ESP32-S3 Dual-Core Storage Policy
 *
 * Platform policy for StorageServerT<>. Provides ESP32-specific
 * implementations:
 *   - PSRAM SPSC ring buffer (2 MB) decoupling protocol handling from SD writes
 *   - FreeRTOS writer task on Core 1 drains ring → staging buffer → SD card
 *   - Core 0 never blocks on SD I/O — maximum protocol throughput
 *
 * Data flow:
 *   Core 0 (protocol): UPLOAD_DATA → uploadWriteBuf (64 KB) → ring buffer (2 MB)
 *   Core 1 (writer):   ring buffer → staging (256 KB) → SD card
 *
 * Buffer lifecycle (on-demand):
 *   - Ring buffer (2 MB) + staging (256 KB) allocated on first upload via
 *     allocateWriterBuffers(), freed after upload completes via freeWriterBuffers().
 *   - Fill buffer (64 KB) allocated/freed per-upload by allocateUploadBuffers()/
 *     freeUploadBuffers().
 *   - When no upload is active, ~0 bytes of PSRAM are used by storage.
 *
 * Included automatically from server/storage_server.h — do not include directly.
 * See server/storage_server.h for the full public API.
 */

#ifndef ESP32_STORAGE_POLICY_H
#define ESP32_STORAGE_POLICY_H

#ifndef STORAGE_SERVER_H
#error "Include <server/storage_server.h> instead of this file directly"
#endif

#include <platform/spsc_ring_buffer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <atomic>

/// 2 MB byte-oriented SPSC ring buffer for upload data (PSRAM, power-of-2)
static constexpr uint32_t UPLOAD_RING_CAPACITY = 2u << 20;  // 2,097,152 bytes
using UploadRingBuffer = SpscRingBuffer<uint8_t, UPLOAD_RING_CAPACITY>;


class Esp32StoragePolicy {
public:
    Esp32StoragePolicy() = default;

    /// SD card uploads/downloads are supported on ESP32 — both LittleFS flash
    /// and SD_MMC use fs::File, so `_shared.uploadFile` (LFSFile) and
    /// SdCardModule::FileHandle are type-compatible.
    static constexpr bool SdSupported = true;

    /// Bind to shared state (called by StorageServerT constructor)
    void init(StorageSharedState* state) { _state = state; }

    /// SD open helpers routed through the policy so the non-portable
    /// SdCardModule::FileHandle type never appears in the shared ipp.
    uint8_t sdOpenRead(const char* path, LFSFile& file) {
        return SdCardModule::instance().openRead(path, file);
    }
    uint8_t sdOpenWrite(const char* path, LFSFile& file, bool truncate) {
        return SdCardModule::instance().openWrite(path, file, truncate);
    }

    // -----------------------------------------------------------------
    // Platform hooks (called by StorageServerT<> via _policy member)
    // -----------------------------------------------------------------

    // --- Buffer management ---
    bool allocateUploadBuffers();
    void freeUploadBuffers();

    // --- Upload data handling ---
    bool onUploadBufferFull();
    bool checkAsyncWriterHealth();

    // --- Upload lifecycle ---
    void onUploadActivated();
    bool onChunkedEnd(const char*& errMsg);
    void onChunkedCleanup();

    // --- Buffer diagnostics ---
    /// Ring buffer fill percentage (0-100) for segment ACK reporting.
    /// Returns fill buffer percentage as fallback when ring is unavailable.
    uint8_t bufferFillPercent() const;

    /// SD writer performance counters (read from Core 0, written by Core 1).
    struct WriterStats {
        uint32_t bytesWritten;      ///< Total bytes written to SD in current upload
        uint32_t writeCount;        ///< Number of SD write calls
        uint32_t maxWriteLatency_ms;///< Worst single SD write latency
        uint32_t totalStallTime_ms; ///< Cumulative time spent in SD write calls
    };

    /// Snapshot current writer stats (safe to call from any core).
    WriterStats writerStats() const;

    // -----------------------------------------------------------------
    // ESP32-specific public API (accessed via storageServer.policy())
    // -----------------------------------------------------------------

    /**
     * @brief Allocate writer buffers from PSRAM (on-demand).
     *
     * Allocates the 2 MB SPSC ring buffer and 256 KB staging buffer.
     * Called automatically by onUploadActivated() before starting the
     * writer task. Idempotent — safe to call multiple times.
     * Call freeWriterBuffers() after upload completes to release PSRAM.
     */
    void allocateWriterBuffers();

    /**
     * @brief Free writer buffers (ring + staging + drain semaphore).
     *
     * Releases the 2 MB ring buffer and 256 KB staging from PSRAM.
     * Called automatically after upload completes (success or cancel).
     * Idempotent — safe to call when buffers are not allocated.
     */
    void freeWriterBuffers();

private:
    StorageSharedState* _state = nullptr;

    // --- Writer task lifecycle (on-demand per upload) ---
    void startWriterTask();
    void stopWriterTask();

    // --- Buffer size constants ---
    static constexpr size_t UPLOAD_FILL_BUF_SIZE     = 65536;   // 64 KB fill buffer (PSRAM)
    static constexpr size_t UPLOAD_FILL_BUF_FALLBACK = 65536;   // 64 KB fill buffer (internal RAM)
    static constexpr size_t WRITER_STAGING_SIZE      = 262144;  // 256 KB staging (PSRAM)

    // --- Ring buffer (on-demand, allocated in allocateWriterBuffers) ---
    UploadRingBuffer  _ring;                         // 2 MB SPSC, PSRAM-backed
    uint8_t*          _writerStaging = nullptr;      // 256 KB staging for ring→SD writes

    // --- Writer task ---
    TaskHandle_t      _writerTask       = nullptr;
    std::atomic<bool> _writerError{false};           // True if SD write failed
    std::atomic<bool> _writerActive{false};          // True while writer task running
    std::atomic<bool> _writerDone{false};            // Task signals before self-delete
    std::atomic<bool> _drainRequested{false};        // Signal writer to drain ring fully
    SemaphoreHandle_t _drainComplete    = nullptr;   // Writer signals when drain done

    // --- Writer task priority (same as audio producer for fair scheduling) ---
    static constexpr UBaseType_t WRITER_TASK_PRIORITY = configMAX_PRIORITIES - 2;
    static constexpr uint32_t    WRITER_TASK_STACK    = 8192;

    // --- Writer performance counters (Core 1 writes, Core 0 reads) ---
    std::atomic<uint32_t> _writerBytesWritten{0};    // Bytes written to SD
    std::atomic<uint32_t> _writerWriteCount{0};      // Number of SD write calls
    std::atomic<uint32_t> _writerMaxLatency_ms{0};   // Worst single write latency
    std::atomic<uint32_t> _writerTotalStall_ms{0};   // Cumulative SD write time

    // --- Internal worker methods ---
    static void writerTaskFunc(void* param);
};

#endif // ESP32_STORAGE_POLICY_H
