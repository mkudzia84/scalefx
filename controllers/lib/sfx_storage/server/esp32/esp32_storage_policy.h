/*
 * Esp32StoragePolicy — ESP32-S3 Single-Core Storage Policy
 *
 * Platform policy for StorageServerT<>. Provides a minimal single-core
 * implementation:
 *   - 64 KB PSRAM fill buffer (per upload; fallback to internal RAM if PSRAM
 *     is unavailable)
 *   - Synchronous SD/flash writes on Core 0 inside the protocol handler
 *
 * Prior revisions used a 2 MB PSRAM ring buffer + Core 1 writer task to
 * decouple protocol handling from SD I/O. That design was removed after it
 * correlated with UPLOAD_END drain hangs on HubFX. The single-core pipeline
 * sustains ~470 KB/s (SD batch/stream) at 6 Mbps UART, which is the
 * effective wire ceiling — the extra plumbing bought nothing but fragility.
 *
 * Exclusivity contract (Rule 28 — see HubFX firmware): when an upload is
 * active, HubFX's main loop is expected to suspend every non-storage
 * feature (audio, engine, USB host, codec health, diagnostics). This is
 * what lets Core 0 block on SD writes without stalling anything that
 * matters. Controllers that can't guarantee that contract should not use
 * burst (batch/stream) upload mode.
 *
 * Included automatically from server/storage_service.h — do not include
 * directly. See server/storage_service.h for the full public API.
 */

#ifndef ESP32_STORAGE_POLICY_H
#define ESP32_STORAGE_POLICY_H

#ifndef STORAGE_SERVICE_H
#error "Include <server/storage_service.h> instead of this file directly"
#endif

#include <cstdint>
#include <cstddef>

class Esp32StoragePolicy {
public:
    Esp32StoragePolicy() = default;

    /// SD card uploads/downloads are supported on ESP32 — both LittleFS flash
    /// and SD_MMC use fs::File, so `_shared.uploadFile` (StorageFile) and
    /// SdCardModule::FileHandle are type-compatible.
    static constexpr bool SdSupported = true;

    /// Bind to shared state (called by StorageServerT constructor)
    void init(StorageSharedState* state) { _state = state; }

    /// SD open helpers routed through the policy so the non-portable
    /// SdCardModule::FileHandle type never appears in the shared ipp.
    uint8_t sdOpenRead(const char* path, StorageFile& file);
    uint8_t sdOpenWrite(const char* path, StorageFile& file, bool truncate);

    // -----------------------------------------------------------------
    // Platform hooks (called by StorageServerT<> via _policy member)
    // -----------------------------------------------------------------

    // --- Buffer management ---
    bool allocateUploadBuffers();
    void freeUploadBuffers();

    // --- Upload data handling ---
    bool onUploadBufferFull();

    /// No async writer — always healthy (writes happen inline on Core 0).
    bool checkAsyncWriterHealth() { return true; }

    // --- Upload lifecycle ---
    void onUploadActivated();
    bool onChunkedEnd(const char*& errMsg);
    void onChunkedCleanup() {}

    // --- Buffer diagnostics ---
    /// Fill buffer percentage (0-100) for segment ACK reporting.
    uint8_t bufferFillPercent() const;

    /// SD writer performance counters (per-upload, reset in onUploadActivated).
    struct WriterStats {
        uint32_t bytesWritten;      ///< Total bytes written to SD in current upload
        uint32_t writeCount;        ///< Number of SD write calls
        uint32_t maxWriteLatency_ms;///< Worst single SD write latency
        uint32_t totalStallTime_ms; ///< Cumulative time spent in SD write calls
    };

    /// Snapshot current writer stats.
    WriterStats writerStats() const {
        return WriterStats{ _bytesWritten, _writeCount, _maxLatency_ms, _totalStall_ms };
    }

private:
    StorageSharedState* _state = nullptr;

    // --- Buffer size constants ---
    //
    // 2026-05-28 (build 489 diag): 64 KB PSRAM upload buffer caused stream-
    // mode UART RX overflow at 750 KB/s.  Root cause: VFS-FAT can't DMA
    // from PSRAM, so writes from a PSRAM source go through an internal
    // bouncer at ~160 KB/s — far below the 750 KB/s incoming rate.  Per
    // flush spike was 408 ms, during which UART (16 KB RX) overran and
    // dropped hundreds of KB.  Sync mode (per-chunk ACK throttles client
    // to ~80 KB/s) was unaffected because the bouncer kept up.
    //
    // Fix: 32 KB DMA-cap internal SRAM buffer.  At full SD speed (~14 MB/s)
    // each flush takes ~2 ms; UART receives < 2 KB during that window —
    // trivially absorbed by the 16 KB RX buffer.  Smaller buffer means
    // 2x flush count per segment but each flush is 100x faster, net huge
    // win.  PSRAM 64 KB fallback retained for boards / configs that don't
    // have DMA-cap headroom.
    static constexpr size_t UPLOAD_FILL_BUF_PRIMARY  = 32768;   // 32 KB DMA-cap SRAM — fast path
    static constexpr size_t UPLOAD_FILL_BUF_FALLBACK = 65536;   // 64 KB PSRAM — slow but works

    // --- Per-upload write counters (plain uint32 — Core 0 only) ---
    uint32_t _bytesWritten  = 0;
    uint32_t _writeCount    = 0;
    uint32_t _maxLatency_ms = 0;
    uint32_t _totalStall_ms = 0;
};

#endif // ESP32_STORAGE_POLICY_H
