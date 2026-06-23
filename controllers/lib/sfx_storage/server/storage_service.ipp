/*
 * Storage Server — Policy-Based Template Implementation (file-ops half)
 *
 * Platform-agnostic protocol handlers for filesystem QUERIES + MUTATIONS via
 * FlashModule and SdCardModule singletons.  File list, tree and download use
 * StreamWriter for chunked transfer.  The exclusive UPLOAD state machine lives
 * in storage_upload_engine.ipp; the stateless path/error helpers live in
 * storage_path_util.h.
 *
 * Platform-specific behavior (buffer allocation, async writes) is resolved
 * at compile time via policy composition: TPolicy provides platform-specific
 * implementations, accessed through the _policy member (no virtual dispatch,
 * no CRTP self-casting).
 *
 * Included from storage_service.h -- do not include directly.  (STORAGE_LOG is
 * defined in storage_service.h so both this unit and the upload-engine unit
 * share it.)
 */

#ifndef STORAGE_SERVICE_IPP
#define STORAGE_SERVICE_IPP

#include <serial/diag_log.h>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include "storage_path_util.h"       // sfx_storage::targetName / mapStorageError / extractPathAndTarget


// ============================================================================
// Packet Dispatch
// ============================================================================

template <typename TPolicy>
CommandHandleResult StorageServicePolicy<TPolicy>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {

    switch (type) {
        // SD card commands (0x93-0x95)
        case StoragePacket::SD_INIT:
            handleSdInit(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::SD_STATUS_REQ:
            handleSdStatus();
            return CommandHandleResult::Handled;

        case StoragePacket::FLASH_STATUS_REQ:
            handleFlashStatus();
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_LIST:
            handleFileList(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_TREE:
            handleFileTree(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_DELETE:
            handleFileDelete(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_MKDIR:
            handleFileMkdir(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_INFO:
            handleFileInfo(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_DOWNLOAD:
            handleFileDownload(payload, len);
            return CommandHandleResult::Handled;

        // Upload commands (0xA0-0xA3) — delegated to the UploadEngine
        case StoragePacket::FILE_UPLOAD_BEGIN:
            _upload.handleUploadBegin(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_DATA:
            _upload.handleUploadData(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_END:
            _upload.handleUploadEnd();
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_CANCEL:
            _upload.handleUploadCancel();
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_DIAG_REQ:
            _upload.handleUploadDiagReq();
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}


// ============================================================================
// Flash Status (0x99)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFlashStatus() {
    // An in-flight upload holds the storage lock for its entire duration
    // (storage_upload_engine.ipp).  Re-locking it here would self-deadlock
    // the loop task that dispatched this status query mid-upload.
    if (_upload.isActive() && _upload.target() == StorageWire::TARGET_FLASH) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    FlashModule& flash = FlashModule::instance();

    FlashStorageInfo info;
    if (!flash.isInitialized()) {
        // Send response with initialized=false
        uint8_t resp[13] = {0};
        resp[0] = 0;  // not initialized
        sendRawPacket(StoragePacket::FLASH_STATUS_REQ, currentTag(), resp, 13);
        return;
    }

    flash.getStorageInfo(info);

    uint8_t resp[13];
    resp[0] = info.initialized ? 1 : 0;
    SfxWire::putU32LE(&resp[1], info.totalBytes);
    SfxWire::putU32LE(&resp[5], info.usedBytes);
    SfxWire::putU32LE(&resp[9], info.freeBytes);

    STORAGE_LOG("flash status: init=%d total=%lu used=%lu free=%lu",
                info.initialized, info.totalBytes, info.usedBytes, info.freeBytes);

    // FLASH_STATUS_REQ doubles as response type (same 0x99)
    sendRawPacket(StoragePacket::FLASH_STATUS_REQ, currentTag(), resp, sizeof(resp));
}


// ============================================================================
// File List (0x9A) -- Streamed Response
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileList(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;

    uint8_t pathErr = sfx_storage::extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_upload.isActive() && _upload.target() == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_LIST %s:%s", sfx_storage::targetName(target), path);

    notifyTransferStart();

    StreamWriter stream(*_ctx, currentTag());
    stream.begin(0);

    auto listCb = [&stream](const FileEntry& entry) -> bool {
        stream.printf("%c\t%s\t%lu\n",
                      entry.isDirectory ? 'd' : 'f',
                      entry.name,
                      (unsigned long)entry.size);
        return true;
    };

    uint8_t err;
    if (target == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().listDirectory(path, listCb);
    else
        err = SdCardModule::instance().listDirectory(path, listCb);

    if (err == SdError::LIMIT_EXCEEDED) {
        stream.printf("TRUNCATED: entry limit reached (%d)\n", MAX_TREE_ENTRIES);
    } else if (err != 0) {
        stream.printf("ERROR: %d\n", err);
    }

    stream.end();
    notifyTransferEnd();
}


// ============================================================================
// File Tree (0xA9) -- Recursive Directory Listing (Streamed)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileTree(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;

    uint8_t pathErr = sfx_storage::extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_upload.isActive() && _upload.target() == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_TREE %s:%s", sfx_storage::targetName(target), path);

    notifyTransferStart();

    StreamWriter stream(*_ctx, currentTag());
    stream.begin(0);

    auto treeCb = [&stream](const FileEntry& entry, int depth) -> bool {
        stream.printf("%d\t%c\t%s\t%lu\n",
                      depth,
                      entry.isDirectory ? 'd' : 'f',
                      entry.name,
                      (unsigned long)entry.size);
        return true;
    };

    uint8_t err;
    if (target == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().listTree(path, treeCb);
    else
        err = SdCardModule::instance().listTree(path, treeCb);

    if (err == SdError::LIMIT_EXCEEDED) {
        stream.printf("TRUNCATED: depth or entry limit reached (max_depth=%d, max_entries=%d)\n",
                      MAX_TREE_DEPTH, MAX_TREE_ENTRIES);
    } else if (err != 0) {
        stream.printf("ERROR: %d\n", err);
    }

    stream.end();
    notifyTransferEnd();
}


// ============================================================================
// File Delete (0x9B)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileDelete(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;
    uint8_t flags = 0;
    bool flagsPresent = false;

    // Detect whether a flags byte was supplied so we can preserve the legacy
    // "no flags = recursive" default for pre-flags clients.
    {
        if (len >= 1) {
            uint8_t pathLen = payload[0];
            if (len > (size_t)(2 + pathLen)) flagsPresent = true;
        }
    }

    uint8_t pathErr = sfx_storage::extractPathAndTarget(payload, len, path, sizeof(path), target, &flags);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    // Guard: reject if upload is active on the same target (would deadlock on mutex)
    if (_upload.isActive() && _upload.target() == target) {
        STORAGE_LOG("FILE_DELETE rejected: upload active on %s", sfx_storage::targetName(target));
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    // Legacy default (no flags byte) = recursive, matching pre-v2 server behavior.
    // Explicit flags byte = client controls recursion via DeleteFlags::RECURSIVE bit.
    bool recursive = flagsPresent ? ((flags & StorageWire::DeleteFlags::RECURSIVE) != 0) : true;

    STORAGE_LOG("FILE_DELETE %s:%s flags=0x%02X recursive=%d",
                sfx_storage::targetName(target), path, flags, recursive ? 1 : 0);

    // Check if target is a file or directory
    FileEntry entry;
    uint8_t infoErr;
    if (target == StorageWire::TARGET_FLASH)
        infoErr = FlashModule::instance().getFileInfo(path, entry);
    else
        infoErr = SdCardModule::instance().getFileInfo(path, entry);

    if (infoErr != 0) {
        sendNack(sfx_storage::mapStorageError(infoErr));
        return;
    }

    uint8_t err;
    if (entry.isDirectory) {
        if (target == StorageWire::TARGET_FLASH)
            err = FlashModule::instance().removeDirectory(path, recursive);
        else
            err = SdCardModule::instance().removeDirectory(path, recursive);
    } else {
        if (target == StorageWire::TARGET_FLASH)
            err = FlashModule::instance().removeFile(path);
        else
            err = SdCardModule::instance().removeFile(path);
    }

    if (err == 0) {
        STORAGE_LOG("deleted %s:%s%s", sfx_storage::targetName(target), path,
                    entry.isDirectory ? (recursive ? " (recursive)" : " (empty)") : "");
        sendAck();
    } else {
        sendNack(sfx_storage::mapStorageError(err));
    }
}


// ============================================================================
// File Mkdir (0x9C)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileMkdir(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;
    uint8_t flags = 0;

    uint8_t pathErr = sfx_storage::extractPathAndTarget(payload, len, path, sizeof(path), target, &flags);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_upload.isActive() && _upload.target() == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    bool createParents = (flags & StorageWire::MkdirFlags::PARENTS) != 0;

    uint8_t err;
    if (target == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().makeDirectory(path, createParents);
    else
        err = SdCardModule::instance().makeDirectory(path, createParents);

    if (err == 0) {
        STORAGE_LOG("mkdir %s:%s%s", sfx_storage::targetName(target), path,
                    createParents ? " -p" : "");
        sendAck();
    } else {
        sendNack(sfx_storage::mapStorageError(err));
    }
}


// ============================================================================
// File Info (0x9D)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileInfo(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;

    uint8_t pathErr = sfx_storage::extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_upload.isActive() && _upload.target() == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    FileEntry entry;
    uint8_t err;
    if (target == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().getFileInfo(path, entry);
    else
        err = SdCardModule::instance().getFileInfo(path, entry);

    // FILE_INFO_RESP: [exists:u8][isDir:u8][size:u32LE]
    uint8_t resp[6];
    if (err == FlashError::NOT_FOUND) {  // Same value as SdError::NOT_FOUND
        resp[0] = 0; resp[1] = 0;
        SfxWire::putU32LE(&resp[2], 0);
    } else if (err == 0) {
        resp[0] = 1;
        resp[1] = entry.isDirectory ? 1 : 0;
        SfxWire::putU32LE(&resp[2], entry.size);
    } else {
        sendNack(sfx_storage::mapStorageError(err));
        return;
    }

    sendRawPacket(StoragePacket::FILE_INFO_RESP, currentTag(), resp, sizeof(resp));
}


// ============================================================================
// File Download (0x9F) -- Streamed Response
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileDownload(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;

    uint8_t pathErr = sfx_storage::extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_upload.isActive() && _upload.target() == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    lockStorage(target);

    StorageFile file;
    uint8_t err;
    if (target == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().openRead(path, file);
    else
        err = _policy.sdOpenRead(path, file);

    if (err != 0) {
        unlockStorage(target);
        sendNack(sfx_storage::mapStorageError(err));
        return;
    }

    STORAGE_LOG("FILE_DOWNLOAD %s:%s size=%lu", sfx_storage::targetName(target), path,
                (unsigned long)file.size());

    notifyTransferStart();

    StreamWriter stream(*_ctx, currentTag());
    stream.begin(file.size());

    uint8_t buf[256];
    while (file.available()) {
        int n = file.read(buf, sizeof(buf));
        if (n <= 0) break;
        stream.write(buf, n);
    }

    file.close();
    unlockStorage(target);

    stream.end();
    notifyTransferEnd();
}


// ============================================================================
// SD Card Init (0x93) -- Remount SD card
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleSdInit(const uint8_t* payload, size_t len) {
    // An in-flight upload holds the storage lock; a remount mid-upload would
    // re-lock it and self-deadlock the dispatching loop task.  Reject
    // unconditionally (a remount of either target is unsafe while uploading).
    if (_upload.isActive()) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    SdCardModule& sd = SdCardModule::instance();

    // SD_INIT payload: [speed_mhz:u8] -- ignored for SDIO mode
    uint8_t speed = (len >= 1) ? payload[0] : 0;

    STORAGE_LOG("SD_INIT: remounting (speed=%u)", speed);

    // Update config speed before retry (meaningful for SPI, ignored for SDIO)
    if (speed > 0) sd.config().speed_mhz = speed;

    // retryInit() calls unmount() first, then re-mounts with stored config
    bool ok = sd.retryInit();
    if (ok) {
        StorageInfo info;
        sd.getStorageInfo(info);

        static const char* typeNames[] = {"NONE", "MMC", "SD", "SDHC", "UNKNOWN"};
        uint8_t ct = (uint8_t)info.cardType;
        const char* typeName = ct <= 4 ? typeNames[ct] : "?";

        STORAGE_LOG("SD_INIT OK: %s %lu MB (total=%lu free=%lu used=%lu)",
                    typeName,
                    (unsigned long)info.cardSize_MB,
                    (unsigned long)info.totalSpace_MB,
                    (unsigned long)info.freeSpace_MB,
                    (unsigned long)info.usedSpace_MB);
        sendAck();
    } else {
        STORAGE_LOG("SD_INIT failed");
        sendNack(StorageError::SD_NOT_INITIALIZED, "SD init failed");
    }
}


// ============================================================================
// SD Card Status (0x94)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleSdStatus() {
    // An in-flight SD upload holds the storage lock for its whole duration;
    // re-locking here would self-deadlock the dispatching loop task.
    if (_upload.isActive() && _upload.target() == StorageWire::TARGET_SD) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    SdCardModule& sd = SdCardModule::instance();

    // SD_STATUS_RESP extended:
    //   [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE]
    //   [freeSpace_MB:u32LE][fatType:u8]
    //   [cardType:u8][busMode:u8][usedSpace_MB:u32LE]
    // = 20 bytes total
    uint8_t resp[20] = {0};

    if (!sd.isInitialized()) {
        resp[0] = 0;
        sendRawPacket(StoragePacket::SD_STATUS_RESP, currentTag(), resp, sizeof(resp));
        return;
    }

    StorageInfo info;
    sd.getStorageInfo(info);

    resp[0] = 1;
    SfxWire::putU32LE(&resp[1], info.cardSize_MB);
    SfxWire::putU32LE(&resp[5], info.totalSpace_MB);
    SfxWire::putU32LE(&resp[9], info.freeSpace_MB);
    resp[13] = info.fatType;
    // Extended fields
    resp[14] = (uint8_t)info.cardType;
    resp[15] = (uint8_t)info.busMode;
    SfxWire::putU32LE(&resp[16], info.usedSpace_MB);

    static const char* typeNames[] = {"NONE", "MMC", "SD", "SDHC", "UNKNOWN"};
    uint8_t ct = (uint8_t)info.cardType;
    const char* typeName = ct <= 4 ? typeNames[ct] : "?";
    static const char* busNames[] = {"SPI", "SDIO-1bit", "SDIO-4bit"};
    uint8_t bm = (uint8_t)info.busMode;
    const char* busName = bm <= 2 ? busNames[bm] : "?";

    STORAGE_LOG("SD status: %s %s card=%luMB total=%luMB free=%luMB used=%luMB",
                typeName, busName,
                (unsigned long)info.cardSize_MB,
                (unsigned long)info.totalSpace_MB,
                (unsigned long)info.freeSpace_MB,
                (unsigned long)info.usedSpace_MB);

    sendRawPacket(StoragePacket::SD_STATUS_RESP, currentTag(), resp, sizeof(resp));
}


// ============================================================================
// Transfer lifecycle notification helpers
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::notifyTransferStart() {
    if (!_transferNotified) {
        _transferNotified = true;
        // Auto-suppress STATUS_UPDATE broadcasts for the duration of
        // the transfer (Rule 28).  Users that want additional behaviour
        // on top can still register `onTransferStart(...)`.
        if (_ctx) _ctx->setTransferActive(true);
        if (_onTransferStart) _onTransferStart();
    }
}

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::notifyTransferEnd() {
    if (_transferNotified) {
        _transferNotified = false;
        if (_ctx) _ctx->setTransferActive(false);
        if (_onTransferEnd) _onTransferEnd();
    }
}


// ============================================================================
// Storage Helpers (shared by file-ops + UploadEngine)
// ============================================================================

template <typename TPolicy>
bool StorageServicePolicy<TPolicy>::checkStorageReady(StorageWire::StorageTarget target) {
    if (target == StorageWire::TARGET_FLASH) {
        if (!FlashModule::instance().isInitialized()) {
            sendNack(SerialError::NOT_INITIALIZED);
            return false;
        }
    } else {
        if constexpr (!TPolicy::SdSupported) {
            sendNack(SerialError::NOT_SUPPORTED);
            return false;
        } else {
            if (!SdCardModule::instance().isInitialized()) {
                sendNack(StorageError::SD_NOT_INITIALIZED);
                return false;
            }
        }
    }
    return true;
}

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::lockStorage(StorageWire::StorageTarget target) {
    if (target == StorageWire::TARGET_FLASH)
        FlashModule::instance().lock();
    else
        SdCardModule::instance().lock();
}

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::unlockStorage(StorageWire::StorageTarget target) {
    if (target == StorageWire::TARGET_FLASH)
        FlashModule::instance().unlock();
    else
        SdCardModule::instance().unlock();
}

#endif // STORAGE_SERVICE_IPP
