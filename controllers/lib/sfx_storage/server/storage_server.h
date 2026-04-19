/*
 * Storage Server — Policy-Based Template for SD Card & Flash File Operations
 *
 * StorageServerT<TPolicy> provides all protocol handling for HubFX storage
 * operations. Platform-specific behavior (buffer allocation, write
 * backend) is resolved at compile time via a policy composed into the
 * server template:
 *
 *   Esp32StoragePolicy  — 64 KB PSRAM fill buffer, blocking inline writes on Core 0
 *   PicoStoragePolicy   — Single heap buffer, blocking inline writes
 *
 * The correct policy is auto-included at the bottom of this header based on
 * SFX_PLATFORM_ESP32, and the fully-specialized type is aliased as
 * `StorageServer` for transparent consumer usage.
 *
 * Design rationale (compile-time dispatch via policy composition):
 *   - Only ONE StorageServer exists per binary — the platform is known at
 *     compile time, making virtual dispatch wasteful.
 *   - Policy composition is preferred over CRTP because it avoids
 *     self-referential complexity (friend declarations, static_cast self())
 *     while preserving zero-cost dispatch.
 *   - The policy receives a pointer to StorageSharedState for access to
 *     shared buffers, file handles, and flags — cleaner than inheritance.
 *   - Platform-specific public API is accessed via the policy() accessor
 *     rather than polluting the server's interface.
 *
 * HubFX exclusivity contract (Rule 28):
 *   An active upload (any mode, including batch/stream burst) runs inline
 *   on Core 0. Callers on HubFX MUST suspend every other feature while
 *   isUploadActive() is true — audio, engine, USB host, codec health,
 *   periodic diagnostics. The onTransferStart/onTransferEnd callbacks
 *   provide a one-shot hook pair for that suspend/resume cycle.
 *
 * Handles SD card management, flash management, and file operations:
 *   - SD_INIT (0x93)          — (re)initialize SD card
 *   - SD_STATUS_REQ (0x94)    — SD card status query
 *   - FLASH_STATUS_REQ (0x99) — LittleFS status
 *   - FILE_LIST (0x9A)        — streamed directory listing (SD/flash)
 *   - FILE_TREE (0xA9)        — streamed recursive directory tree (SD/flash)
 *   - FILE_DELETE (0x9B)      — delete file or directory (recursive for dirs)
 *   - FILE_MKDIR (0x9C)       — create directory (SD/flash)
 *   - FILE_INFO (0x9D)        — file/dir info query (SD/flash)
 *   - FILE_DOWNLOAD (0x9F)    — streamed file download (SD/flash)
 *   - FILE_UPLOAD_BEGIN (0xA0) — start file upload (SD/flash)
 *   - FILE_UPLOAD_DATA (0xA1) — upload data chunk with CRC-16
 *   - FILE_UPLOAD_END (0xA2)  — finalize upload
 *   - FILE_UPLOAD_CANCEL (0xA3)— cancel in-progress upload
 *
 * Shared library component (controllers/lib/sfx_storage/).
 * Depends on: sfx_serial, sfx_platform, sfx_storage (flash + sd_card singletons).
 */

#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include <serial/core/stream.h>
#include <storage/flash.h>
#include <storage/sd_card.h>
#include <MD5Builder.h>
#include <platform/sfx_platform.h>
#include <platform/diag_log.h>
#include <functional>


// ============================================================================
// Shared state between the server template and the platform policy.
//
// Contains the buffers, file handles, and flags that both the protocol
// handlers (in .ipp) and the platform hooks (policy) need to access.
// Protocol-only state (upload path, MD5, sequence tracking) stays private
// on StorageServerT.
// ============================================================================

struct StorageSharedState {
    // Active fill buffer (Core 0 writes into this) — dynamically allocated
    uint8_t* uploadWriteBuf    = nullptr;
    size_t   uploadWriteBufLen = 0;
    size_t   uploadBufCapacity = 0;

    // File handle for active upload
    LFSFile  uploadFile;

    /// Flush buffered upload data to file (blocking on the current core).
    /// Used by both the .ipp (handleUploadEnd final partial flush) and
    /// single-core policies that write inline from onUploadBufferFull().
    bool flushUploadBuffer() {
        if (uploadWriteBufLen == 0) return true;

        size_t written = uploadFile.write(uploadWriteBuf, uploadWriteBufLen);
        if (written != uploadWriteBufLen) {
            SFX_LOG_INFO("[Storage] Write buffer flush failed: expected %u wrote %u",
                         (unsigned)uploadWriteBufLen, (unsigned)written);
            uploadWriteBufLen = 0;
            return false;
        }

        uploadWriteBufLen = 0;
        return true;
    }
};


/**
 * @brief Policy-based storage server template.
 *
 * TPolicy must provide the following methods (called via _policy member):
 *
 *   void init(StorageSharedState* state);
 *
 *   // Buffer lifecycle:
 *   bool allocateUploadBuffers();
 *   void freeUploadBuffers();
 *
 *   // Upload data handling:
 *   bool onUploadBufferFull();
 *   bool checkAsyncWriterHealth();
 *
 *   // Upload lifecycle:
 *   void onUploadActivated();
 *   bool onChunkedEnd(const char*& errMsg);
 *   void onChunkedCleanup();
 *
 *   // Buffer diagnostics:
 *   uint8_t bufferFillPercent() const;   // 0-100, fill-buffer level (for segment ACK)
 */
template <typename TPolicy>
class StorageServerT : public BusServer {
public:
    StorageServerT() { _policy.init(&_shared); }

    const char* handlerName() const override { return "StorageServer"; }

    /// Cancel any active upload (called on SHUTDOWN to clean up state)
    void cancelActiveUpload();

    /**
     * @brief Check for upload inactivity timeout
     *
     * Call this from the main loop. If an upload is active and no
     * upload packet has arrived for UPLOAD_TIMEOUT_MS, the upload
     * is automatically cancelled and the partial file deleted.
     */
    void checkUploadTimeout();

    /// True while a file upload is in progress (any mode).
    bool isUploadActive() const { return _uploadActive; }

    /// True while a raw binary stream upload is active.
    /// When true, the main loop should call processStream() instead of
    /// server.loop() — COBS parsing is bypassed, serial data is read raw.
    bool isStreamActive() const { return _streamActive; }

    /**
     * @brief Process raw binary stream (batch-mode) data during an upload.
     *
     * Reads available bytes from Serial into the policy's fill buffer,
     * flushes each full buffer inline to storage, tracks segment
     * boundaries, and sends SEGMENT_ACKs (FILE_UPLOAD_PROGRESS) when each
     * segment is fully received.
     *
     * After the final segment, exits stream mode so the next loop()
     * iteration can process the COBS-framed UPLOAD_END from the client.
     *
     * MUST only be called when isStreamActive() returns true.
     */
    void processStream();

    /// Access the platform policy.
    TPolicy& policy() { return _policy; }
    const TPolicy& policy() const { return _policy; }

    // -----------------------------------------------------------------
    // Transfer lifecycle callbacks (for serial exclusivity)
    // -----------------------------------------------------------------

    /**
     * @brief Register callback invoked when ANY file transfer starts.
     *
     * Fires at the start of uploads (all modes), downloads, list, and tree.
     * Use to suppress verbose STATUS_UPDATE packets during transfer.
     * Pairs with onTransferEnd which fires when the transfer completes.
     */
    void onTransferStart(std::function<void()> cb) { _onTransferStart = std::move(cb); }

    /**
     * @brief Register callback invoked when ANY file transfer ends.
     *
     * Fires after download completes, upload ends/cancels/times out,
     * or list/tree streaming finishes. Guaranteed exactly once per
     * onTransferStart invocation.
     */
    void onTransferEnd(std::function<void()> cb) { _onTransferEnd = std::move(cb); }

    // -----------------------------------------------------------------
    // Upload lifecycle callbacks (for exclusive-upload resource mgmt)
    // -----------------------------------------------------------------

    /**
     * @brief Register callback invoked when ANY upload (sync or batch) starts.
     *
     * Fires at UPLOAD_BEGIN for both UPLOAD_SYNC and UPLOAD_STREAM. Use
     * to suspend competing resources (audio tasks, engine, USB host
     * poll, etc.) so the upload runs exclusively on the main loop.
     */
    void onUploadStart(std::function<void()> cb) { _onUploadStart = std::move(cb); }

    /**
     * @brief Register callback invoked when ANY upload ends or is cancelled.
     *
     * Fires after upload state is cleaned up (success, cancel, timeout,
     * or error). Guaranteed to fire exactly once per onUploadStart.
     * Use to resume resources suspended in onUploadStart.
     */
    void onUploadEnd(std::function<void()> cb) { _onUploadEnd = std::move(cb); }

protected:
    // --- BusServer overrides ---
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

    uint8_t moduleRangeLow()  const override { return 0x93; }
    uint8_t moduleRangeHigh() const override { return 0xAA; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

    // ================================================================
    // Shared state (accessible to .ipp template methods)
    // ================================================================
    StorageSharedState _shared;

private:
    // ================================================================
    // Platform policy (composed, compile-time dispatch)
    // ================================================================
    TPolicy _policy;

    // --- Command handlers ---
    void handleSdInit(const uint8_t* payload, size_t len);
    void handleSdStatus();
    void handleFlashStatus();
    void handleFileList(const uint8_t* payload, size_t len);
    void handleFileTree(const uint8_t* payload, size_t len);
    void handleFileDelete(const uint8_t* payload, size_t len);
    void handleFileMkdir(const uint8_t* payload, size_t len);
    void handleFileInfo(const uint8_t* payload, size_t len);
    void handleFileDownload(const uint8_t* payload, size_t len);

    // --- Upload handlers ---
    void handleUploadBegin(const uint8_t* payload, size_t len);
    void handleUploadData(const uint8_t* payload, size_t len);
    void handleUploadEnd();
    void handleUploadCancel();

    /// Clean up upload state (close file, unlock storage, delete partial)
    void cleanupUpload(bool deletePartial);

    /// Notify transfer started (fires callback, guards against double-start)
    void notifyTransferStart();

    /// Notify transfer ended (fires callback, guards against double-end)
    void notifyTransferEnd();

    // --- Storage helpers ---
    bool checkStorageReady(HubFxStorage::StorageTarget target);
    void lockStorage(HubFxStorage::StorageTarget target);
    void unlockStorage(HubFxStorage::StorageTarget target);
    static const char* targetName(HubFxStorage::StorageTarget target);
    uint8_t mapStorageError(uint8_t err);
    uint8_t extractPathAndTarget(const uint8_t* payload, size_t len,
                                  char* path, size_t pathBufSize,
                                  HubFxStorage::StorageTarget& target,
                                  uint8_t* flagsOut = nullptr);
    static bool isValidPath(const char* path);

    // --- Upload state (protocol-only, policies never touch these) ---
    bool     _uploadActive       = false;
    HubFxStorage::StorageTarget _uploadTarget = HubFxStorage::TARGET_SD;
    HubFxStorage::UploadMode    _uploadMode   = HubFxStorage::UPLOAD_SYNC;
    char     _uploadPath[128]    = {};
    uint32_t _uploadExpectedSize = 0;
    uint32_t _uploadBytesWritten = 0;
    uint16_t _uploadExpectedSeq  = 0;
    MD5Builder _uploadMd5;

    // --- Stream upload state ---
    bool     _streamActive          = false;    // True while raw binary streaming
    uint32_t _streamSegmentSize     = 0;        // Bytes per segment (set in handleUploadBegin)
    uint16_t _streamSegmentIndex    = 0;        // Current segment number (0-based)
    uint32_t _streamSegBytesRemaining = 0;      // Bytes left in current segment
    uint16_t _streamSegmentCount    = 0;        // Total segment count (for logging)

    // --- Stream diagnostics ---
    uint32_t _streamStartTime_ms    = 0;        // Stream mode activation time
    uint32_t _streamSegStartTime_ms = 0;        // Current segment start time
    uint32_t _streamEndTime_ms      = 0;        // Stream mode deactivation time
    uint32_t _streamLastLogTime_ms  = 0;        // Last periodic progress log
    uint32_t _streamLastLogBytes    = 0;        // Bytes at last periodic log
    uint32_t _streamLastCallTime_ms = 0;        // Last processStream() call time
    uint32_t _streamMaxGap_ms       = 0;        // Worst-case gap between calls
    uint32_t _streamIterCount       = 0;        // processStream() call count per segment

    static constexpr uint32_t STREAM_SEGMENT_SIZE        = 524288;  // SD: 512 KB per segment
    static constexpr uint32_t STREAM_SEGMENT_SIZE_FLASH  = 131072;  // flash: 128 KB per segment
    static constexpr uint32_t STREAM_INACTIVITY_MS       = 5000;    // 5s timeout per segment

    /// Send segment ACK (FILE_UPLOAD_PROGRESS) after each stream segment.
    void sendStreamSegmentAck();

    // --- Transfer lifecycle callbacks (serial exclusivity) ---
    std::function<void()> _onTransferStart;
    std::function<void()> _onTransferEnd;
    bool _transferNotified = false;  // Guard: ensures exactly one onTransferEnd per onTransferStart

    // --- Upload lifecycle callbacks (exclusive-upload resource mgmt) ---
    std::function<void()> _onUploadStart;
    std::function<void()> _onUploadEnd;
    bool _uploadSuspended = false;  // Guard: ensures exactly one onUploadEnd per onUploadStart

    static constexpr uint32_t UPLOAD_TIMEOUT_MS = 30000;
    uint32_t _uploadLastActivity_ms = 0;

    static constexpr uint32_t MAX_UPLOAD_SIZE_FLASH = 2  * 1024 * 1024;
    static constexpr uint32_t MAX_UPLOAD_SIZE_SD    = 256 * 1024 * 1024;
};

// ============================================================================
// Template implementation
// ============================================================================
#include "storage_server.ipp"

// ============================================================================
// Platform-specific policy (auto-selected by build target)
// ============================================================================
#if SFX_PLATFORM_ESP32
#include "esp32/esp32_storage_policy.h"
using StorageServer = StorageServerT<Esp32StoragePolicy>;
#else
#include "pico/pico_storage_policy.h"
using StorageServer = StorageServerT<PicoStoragePolicy>;
#endif

#endif // STORAGE_SERVER_H
