/*
 * Storage Server — Policy-Based Template for SD Card & Flash File Operations
 *
 * StorageServerT<TPolicy> provides all protocol handling for HubFX storage
 * operations.  Platform-specific behavior (buffer allocation, async writes,
 * stream data routing) is resolved at compile time via a POLICY object
 * composed into the server template:
 *
 *   Esp32StoragePolicy  — PSRAM double-buffered, dual-core FreeRTOS writer tasks
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
 *   - Platform-specific public API (e.g., startWriterTask) is accessed via
 *     the policy() accessor rather than polluting the server's interface.
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

    // Staging buffer for SD writes — allocated per-upload
    uint8_t* streamStaging        = nullptr;
    size_t   streamStagingLen     = 0;
    uint32_t streamBytesWrittenToSD = 0;

    // Stream state flags
    bool     streamReceiving  = false;
    bool     streamWriteError = false;

    /// Flush buffered upload data to file (blocking on current core).
    /// Used by both the .ipp (handleUploadEnd final flush) and policies
    /// (Pico: onUploadBufferFull, ESP32: fallback when no writer task).
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
 *   bool allocateStreamBuffers();
 *   void freeStreamBuffers();
 *
 *   // Upload data handling:
 *   bool onUploadBufferFull();
 *   bool checkAsyncWriterHealth();
 *
 *   // Upload lifecycle:
 *   void onUploadActivated(bool isStream);
 *   bool onStreamStart();
 *   bool onStreamEnd(const char*& errMsg);
 *   bool onChunkedEnd(const char*& errMsg);
 *   void onStreamCleanup();
 *   void onChunkedCleanup();
 *
 *   // Stream data processing:
 *   void onStreamDataReceived(const uint8_t* data, size_t len);
 *   void onStreamReceiveComplete();
 *   size_t streamBufferCapacityForLog() const;
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

    /**
     * @brief True when the server is in raw stream receive mode.
     *
     * When true, the main loop MUST call processStreamData() instead of
     * server.loop() so that raw bytes go to the platform-specific buffer.
     */
    bool isStreamReceiving() const { return _shared.streamReceiving; }

    /**
     * @brief Process raw stream data from serial (UPLOAD_STREAM mode)
     *
     * Reads available bytes from the Serial stream, feeds them to the
     * running MD5 hash, and delegates to the policy's
     * onStreamDataReceived() for storage writes.
     *
     * @param serial  Reference to the Serial stream (UART0)
     */
    void processStreamData(Stream& serial);

    /// Access the platform policy (e.g. for ESP32-specific startWriterTask())
    TPolicy& policy() { return _policy; }
    const TPolicy& policy() const { return _policy; }

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

    // --- Storage helpers ---
    bool checkStorageReady(HubFxStorage::StorageTarget target);
    void lockStorage(HubFxStorage::StorageTarget target);
    void unlockStorage(HubFxStorage::StorageTarget target);
    static const char* targetName(HubFxStorage::StorageTarget target);
    uint8_t mapStorageError(uint8_t err);
    uint8_t extractPathAndTarget(const uint8_t* payload, size_t len,
                                  char* path, size_t pathBufSize,
                                  HubFxStorage::StorageTarget& target);
    static bool isValidPath(const char* path);

    // --- Upload state (protocol-only, policies never touch these) ---
    bool     _uploadActive       = false;
    HubFxStorage::StorageTarget _uploadTarget = HubFxStorage::TARGET_SD;
    HubFxStorage::UploadMode    _uploadMode   = HubFxStorage::UPLOAD_SYNC;
    char     _uploadPath[128]    = {};
    uint32_t _uploadExpectedSize = 0;
    uint32_t _uploadBytesWritten = 0;
    uint16_t _uploadExpectedSeq  = 0;
    uint16_t _uploadCrcErrors    = 0;
    MD5Builder _uploadMd5;

    static constexpr uint32_t UPLOAD_TIMEOUT_MS = 30000;
    uint32_t _uploadLastActivity_ms = 0;

    static constexpr uint32_t STREAM_DATA_TIMEOUT_MS = 5000;

    static constexpr uint32_t MAX_UPLOAD_SIZE_FLASH = 2  * 1024 * 1024;
    static constexpr uint32_t MAX_UPLOAD_SIZE_SD    = 256 * 1024 * 1024;

    uint32_t _streamBytesRemaining = 0;
};

// ============================================================================
// Template implementation
// ============================================================================
#include "storage_server.ipp"

// ============================================================================
// Platform-specific policy (auto-selected by build target)
// ============================================================================
#if SFX_PLATFORM_ESP32
#include "../esp32/esp32_storage_policy.h"
using StorageServer = StorageServerT<Esp32StoragePolicy>;
#else
#include "../pico/pico_storage_policy.h"
using StorageServer = StorageServerT<PicoStoragePolicy>;
#endif

#endif // STORAGE_SERVER_H
