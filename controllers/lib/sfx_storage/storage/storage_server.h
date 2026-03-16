/*
 * Storage Server — SD Card & Flash File Operations Handler (Base Class)
 *
 * Abstract base class for storage operations. Platform-specific behavior
 * (buffer allocation, async writes, stream data routing) is delegated to
 * derived classes via protected virtual hooks:
 *
 *   StorageServerEsp32 — PSRAM double-buffered, dual-core FreeRTOS writer tasks
 *   StorageServerPico  — Single heap buffer, blocking inline writes
 *
 * The correct derived class is auto-included based on SFX_PLATFORM_ESP32,
 * and aliased as `StorageServer` for transparent consumer usage.
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

class StorageServerBase : public BusServer {
public:
    StorageServerBase() = default;
    virtual ~StorageServerBase() = default;

    const char* handlerName() const override { return "StorageServer"; }

    /// Cancel any active upload (called on SHUTDOWN to clean up state)
    void cancelActiveUpload();

    /**
     * @brief Check for upload inactivity timeout
     *
     * Call this from the main loop. If an upload is active and no
     * upload packet has arrived for UPLOAD_TIMEOUT_MS, the upload
     * is automatically cancelled and the partial file deleted.
     * This protects against client crashes, USB disconnects, or
     * CLI freezes leaving the storage mutex locked forever.
     */
    void checkUploadTimeout();

    /// True while a file upload is in progress (any mode).
    /// Use in the main loop to skip vTaskDelay for maximum throughput.
    bool isUploadActive() const { return _uploadActive; }

    /**
     * @brief True when the server is in raw stream receive mode.
     *
     * When true, the main loop MUST call processStreamData() instead of
     * server.loop() so that raw bytes go to the platform-specific buffer,
     * not the COBS parser (CommandRouter). After exactly file_size bytes
     * are received, this returns false and normal COBS processing resumes.
     */
    bool isStreamReceiving() const { return _streamReceiving; }

    /**
     * @brief Process raw stream data from serial (UPLOAD_STREAM mode)
     *
     * Reads available bytes from the Serial stream, feeds them to the
     * running MD5 hash, and delegates to the platform-specific
     * onStreamDataReceived() for storage writes.
     *
     * Called from the main loop when isStreamReceiving() is true.
     * Reads exactly _streamBytesRemaining bytes, then exits stream mode.
     *
     * @param serial  Reference to the Serial stream (UART0)
     */
    void processStreamData(Stream& serial);

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
    // Platform Hooks — Pure Virtual (derived classes MUST override)
    // ================================================================

    /// Allocate write buffers for chunked upload mode.
    /// Must set _uploadWriteBuf and _uploadBufCapacity.
    virtual bool allocateUploadBuffers() = 0;

    /// Free write buffers for chunked upload mode.
    virtual void freeUploadBuffers() = 0;

    /// Allocate buffers for stream upload mode.
    /// Must set _streamStaging (ring buffer on ESP32, staging only on Pico).
    virtual bool allocateStreamBuffers() = 0;

    /// Free buffers for stream upload mode.
    virtual void freeStreamBuffers() = 0;

    /// Handle write buffer full during chunked upload data reception.
    /// ESP32: async submit to writer task; Pico: blocking flush.
    /// Returns false on error (base class will NACK and abort).
    virtual bool onUploadBufferFull() = 0;

    /// Write stream data to platform-specific destination.
    /// ESP32: PSRAM ring buffer (Core 1 drains to SD).
    /// Pico: staging buffer + inline SD flush when full.
    virtual void onStreamDataReceived(const uint8_t* data, size_t len) = 0;

    /// Signal that all stream bytes have been received from serial.
    /// ESP32: set drain-complete flag for writer task.
    /// Pico: log completion stats.
    virtual void onStreamReceiveComplete() = 0;

    /// Finalize stream upload — wait for writer drain, flush remaining data.
    /// Sets errMsg on failure. Returns false on error.
    virtual bool onStreamEnd(const char*& errMsg) = 0;

    /// Buffer capacity to display in upload-begin log.
    /// ESP32: ring buffer capacity; Pico: staging buffer size.
    virtual size_t streamBufferCapacityForLog() const = 0;

    // ================================================================
    // Platform Hooks — Virtual with Defaults
    // ================================================================

    /// Check for async writer errors before processing upload data.
    /// ESP32: checks _writerError atomic flag.
    /// Default (Pico): always returns true (no async writer).
    virtual bool checkAsyncWriterHealth() { return true; }

    /// Platform-specific init after _uploadActive is set.
    /// ESP32: resets _writerError flag. Pico: no-op.
    /// @param isStream true if this is a stream upload, false for chunked
    virtual void onUploadActivated(bool isStream) { (void)isStream; }

    /// Start stream writer task (stream mode).
    /// ESP32: launches FreeRTOS task on Core 1.
    /// Default (Pico): returns true (no writer task needed).
    virtual bool onStreamStart() { return true; }

    /// Finalize chunked upload — wait for async writer completion.
    /// ESP32: waits for writer task, checks error flag.
    /// Default (Pico): returns true (no async writer).
    virtual bool onChunkedEnd(const char*& errMsg) { (void)errMsg; return true; }

    /// Cleanup stream mode on cancel or error.
    /// ESP32: stops stream writer task. Pico: no-op.
    virtual void onStreamCleanup() {}

    /// Cleanup chunked mode on cancel or error.
    /// ESP32: waits for writer task. Pico: no-op.
    virtual void onChunkedCleanup() {}

    // ================================================================
    // Shared State (accessible to derived classes)
    // ================================================================

    // Active fill buffer (Core 0 writes into this) — dynamically allocated
    uint8_t* _uploadWriteBuf    = nullptr;
    size_t   _uploadWriteBufLen = 0;
    size_t   _uploadBufCapacity = 0;  // Actual allocated size

    // File handle for active upload
    LFSFile  _uploadFile;

    // Staging buffer for SD writes — allocated per-upload
    uint8_t* _streamStaging        = nullptr;
    size_t   _streamStagingLen     = 0;       // Current fill level
    uint32_t _streamBytesWrittenToSD = 0;     // Total bytes flushed to SD

    // Stream state flags
    bool     _streamReceiving  = false;       // True = bypass COBS, read raw
    bool     _streamWriteError = false;       // SD write error during stream

    /// Flush buffered upload data to file (blocking on current core)
    bool flushUploadBuffer();

private:
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

    // --- Upload state ---
    bool     _uploadActive       = false;
    HubFxStorage::StorageTarget _uploadTarget = HubFxStorage::TARGET_SD;
    HubFxStorage::UploadMode    _uploadMode   = HubFxStorage::UPLOAD_SYNC;
    char     _uploadPath[128]    = {};
    uint32_t _uploadExpectedSize = 0;
    uint32_t _uploadBytesWritten = 0;
    uint16_t _uploadExpectedSeq  = 0;
    uint16_t _uploadCrcErrors    = 0;
    MD5Builder _uploadMd5;

    /// Inactivity timeout for uploads (ms).
    static constexpr uint32_t UPLOAD_TIMEOUT_MS = 30000;  // 30 seconds
    uint32_t _uploadLastActivity_ms = 0;

    /// Stream data inactivity timeout (ms).
    /// When in raw stream mode and no serial data arrives for this duration,
    /// exit stream mode and mark the upload as errored.  This MUST be shorter
    /// than the client's UPLOAD_END wait timeout so that the UPLOAD_END COBS
    /// packet is processed normally (not consumed as raw stream data).
    static constexpr uint32_t STREAM_DATA_TIMEOUT_MS = 5000;  // 5 seconds

    /// Maximum upload file size per target (safety cap).
    static constexpr uint32_t MAX_UPLOAD_SIZE_FLASH = 2  * 1024 * 1024;   // 2 MB
    static constexpr uint32_t MAX_UPLOAD_SIZE_SD    = 256 * 1024 * 1024;   // 256 MB

    // --- Stream remaining bytes ---
    uint32_t _streamBytesRemaining = 0;
};

// ============================================================================
// Platform-specific derived class (auto-selected by build target)
// ============================================================================
#if SFX_PLATFORM_ESP32
#include "storage_server_esp32.h"
#else
#include "storage_server_pico.h"
#endif

#endif // STORAGE_SERVER_H
