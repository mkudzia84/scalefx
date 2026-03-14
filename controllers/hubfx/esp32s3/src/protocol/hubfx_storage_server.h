/*
 * HubFX Storage Server — SD Card & Flash File Operations Handler
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
 * Config commands (0x90-0x92) are not yet implemented.
 *
 * This is an ESP32-S3 controller-local class (not shared library)
 * because it depends on platform-specific storage modules.
 */

#ifndef HUBFX_STORAGE_SERVER_H
#define HUBFX_STORAGE_SERVER_H

#include <serial/serial.h>
#include <serial/hubfx/hubfx.h>
#include <serial/core/stream.h>
#include <storage/flash.h>
#include <storage/sd_card.h>
#include <MD5Builder.h>

class HubFxStorageServer : public BusServer {
public:
    HubFxStorageServer() = default;

    const char* handlerName() const override { return "HubFxStorageServer"; }

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
protected:
    CommandHandleResult handleModulePacket(uint8_t type,
                                           const uint8_t* payload,
                                           size_t len) override;

    uint8_t moduleRangeLow()  const override { return 0x93; }
    uint8_t moduleRangeHigh() const override { return 0xA9; }

    const char* getModuleErrorMessage(uint8_t code) override {
        return HubFxError::getMessage(code);
    }

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

    /// Check if storage target is initialized, send NACK if not
    bool checkStorageReady(HubFxStorage::StorageTarget target);

    /// Lock/unlock the appropriate storage module
    void lockStorage(HubFxStorage::StorageTarget target);
    void unlockStorage(HubFxStorage::StorageTarget target);

    /// Get target name for logging
    static const char* targetName(HubFxStorage::StorageTarget target);

    /// Map FlashError/SdError to HubFxError (codes are identical)
    uint8_t mapStorageError(uint8_t err);

    /**
     * @brief Extract path and optional target from payload
     *
     * Wire format: [pathLen:u8][path:str][target:u8?]
     * If target byte is omitted, defaults to TARGET_SD.
     *
     * Validates:
     *   - Path length and buffer bounds
     *   - No embedded null bytes in path data
     *   - Path format (starts with '/', no '..' traversal)
     *   - Target enum bounds (0=SD, 1=Flash)
     *
     * @param payload Packet payload
     * @param len     Payload length
     * @param path    Output path buffer
     * @param pathBufSize Size of path buffer
     * @param target  Output target (TARGET_SD or TARGET_FLASH)
     * @return SerialError::OK on success, or specific error code
     */
    uint8_t extractPathAndTarget(const uint8_t* payload, size_t len,
                              char* path, size_t pathBufSize,
                              HubFxStorage::StorageTarget& target);

    /**
     * @brief Validate a file path for safety
     *
     * Rejects paths that:
     *   - Don't start with '/'
     *   - Contain '..' path traversal components
     *
     * @param path Null-terminated path string
     * @return true if path is valid
     */
    static bool isValidPath(const char* path);

    /// Flush buffered upload data to file
    bool flushUploadBuffer();

    // --- Upload state ---
    bool     _uploadActive       = false;
    HubFxStorage::StorageTarget _uploadTarget = HubFxStorage::TARGET_SD;
    HubFxStorage::UploadMode    _uploadMode   = HubFxStorage::UPLOAD_SYNC;
    char     _uploadPath[128]    = {};
    uint32_t _uploadExpectedSize = 0;
    uint32_t _uploadBytesWritten = 0;
    uint16_t _uploadExpectedSeq  = 0;
    uint16_t _uploadCrcErrors    = 0;  // CRC errors counted in burst mode
    LFSFile  _uploadFile;
    MD5Builder _uploadMd5;             // Running MD5 hash of uploaded data

    // --- Upload write buffer (accumulate chunks, flush in 4KB blocks) ---
    static constexpr size_t UPLOAD_WRITE_BUF_SIZE = 4096;
    uint8_t  _uploadWriteBuf[UPLOAD_WRITE_BUF_SIZE] = {};
    size_t   _uploadWriteBufLen = 0;

    /// Inactivity timeout for uploads (ms).
    /// If no UPLOAD_DATA/END/CANCEL arrives within this window,
    /// the upload is auto-cancelled to prevent stuck mutex/partial files.
    static constexpr uint32_t UPLOAD_TIMEOUT_MS = 30000;  // 30 seconds
    uint32_t _uploadLastActivity_ms = 0;

    /// Maximum upload file size per target (safety cap).
    /// These are sanity limits independent of free-space checks.
    static constexpr uint32_t MAX_UPLOAD_SIZE_FLASH = 2  * 1024 * 1024;   // 2 MB
    static constexpr uint32_t MAX_UPLOAD_SIZE_SD    = 256 * 1024 * 1024;   // 256 MB
};

#endif // HUBFX_STORAGE_SERVER_H
