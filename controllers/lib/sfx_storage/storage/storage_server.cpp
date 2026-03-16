/*
 * Storage Server — Base Class Implementation
 *
 * Platform-agnostic protocol handlers for file operations via FlashModule
 * and SdCardModule singletons. File list and download use StreamWriter
 * for chunked transfer.
 *
 * Platform-specific behavior (buffer allocation, async writes, stream
 * data routing) is delegated to derived classes via virtual hooks.
 * See storage_server_esp32.cpp and storage_server_pico.cpp.
 *
 * Buffer sizing rationale (6 Mbps serial):
 *   - Raw byte rate: 750 KB/s, effective ~580 KB/s with UART/COBS overhead
 *   - Each chunk: 2044 data + 4 header = 2048 payload, ~2060 bytes on wire
 *   - Max ~280 chunks/sec at full wire speed
 *   - UART RX buffer: 128 KB — holds ~64 chunks (~220ms of data at line rate)
 */

#include "storage_server.h"
#include <platform/diag_log.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// Packet Dispatch
// ============================================================================

CommandHandleResult StorageServerBase::handleModulePacket(
        uint8_t type, const uint8_t* payload, size_t len) {

    switch (type) {
        // SD card commands (0x93-0x95)
        case HubFxPacket::SD_INIT:
            handleSdInit(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::SD_STATUS_REQ:
            handleSdStatus();
            return CommandHandleResult::Handled;

        case HubFxPacket::FLASH_STATUS_REQ:
            handleFlashStatus();
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_LIST:
            handleFileList(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_TREE:
            handleFileTree(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_DELETE:
            handleFileDelete(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_MKDIR:
            handleFileMkdir(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_INFO:
            handleFileInfo(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_DOWNLOAD:
            handleFileDownload(payload, len);
            return CommandHandleResult::Handled;

        // Upload commands (0xA0-0xA3)
        case HubFxPacket::FILE_UPLOAD_BEGIN:
            handleUploadBegin(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_UPLOAD_DATA:
            handleUploadData(payload, len);
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_UPLOAD_END:
            handleUploadEnd();
            return CommandHandleResult::Handled;

        case HubFxPacket::FILE_UPLOAD_CANCEL:
            handleUploadCancel();
            return CommandHandleResult::Handled;

        default:
            return CommandHandleResult::NotMyCommand;
    }
}


// ============================================================================
// Flash Status (0x99)
// ============================================================================

void StorageServerBase::handleFlashStatus() {
    FlashModule& flash = FlashModule::instance();

    FlashStorageInfo info;
    if (!flash.isInitialized()) {
        // Send response with initialized=false
        uint8_t resp[13] = {0};
        resp[0] = 0;  // not initialized
        sendRawPacket(HubFxPacket::FLASH_STATUS_REQ, currentTag(), resp, 13);
        return;
    }

    flash.getStorageInfo(info);

    uint8_t resp[13];
    resp[0] = info.initialized ? 1 : 0;
    CoreProtocol::putU32LE(&resp[1], info.totalBytes);
    CoreProtocol::putU32LE(&resp[5], info.usedBytes);
    CoreProtocol::putU32LE(&resp[9], info.freeBytes);

    STORAGE_LOG("flash status: init=%d total=%lu used=%lu free=%lu",
                info.initialized, info.totalBytes, info.usedBytes, info.freeBytes);

    // FLASH_STATUS_REQ doubles as response type (same 0x99)
    sendRawPacket(HubFxPacket::FLASH_STATUS_REQ, currentTag(), resp, sizeof(resp));
}


// ============================================================================
// File List (0x9A) — Streamed Response
// ============================================================================

void StorageServerBase::handleFileList(const uint8_t* payload, size_t len) {
    char path[128];
    HubFxStorage::StorageTarget target = HubFxStorage::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_LIST %s:%s", targetName(target), path);

    StreamWriter stream(*this, currentTag());
    stream.begin(0);

    auto listCb = [&stream](const FileEntry& entry) -> bool {
        stream.printf("%c\t%s\t%lu\n",
                      entry.isDirectory ? 'd' : 'f',
                      entry.name,
                      (unsigned long)entry.size);
        return true;
    };

    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH)
        err = FlashModule::instance().listDirectory(path, listCb);
    else
        err = SdCardModule::instance().listDirectory(path, listCb);

    if (err == SdError::LIMIT_EXCEEDED) {
        stream.printf("TRUNCATED: entry limit reached (%d)\n", MAX_TREE_ENTRIES);
    } else if (err != 0) {
        stream.printf("ERROR: %d\n", err);
    }

    stream.end();
}


// ============================================================================
// File Tree (0xA9) — Recursive Directory Listing (Streamed)
// ============================================================================

void StorageServerBase::handleFileTree(const uint8_t* payload, size_t len) {
    char path[128];
    HubFxStorage::StorageTarget target = HubFxStorage::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_TREE %s:%s", targetName(target), path);

    StreamWriter stream(*this, currentTag());
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
    if (target == HubFxStorage::TARGET_FLASH)
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
}


// ============================================================================
// File Delete (0x9B)
// ============================================================================

void StorageServerBase::handleFileDelete(const uint8_t* payload, size_t len) {
    char path[128];
    HubFxStorage::StorageTarget target = HubFxStorage::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    // Guard: reject if upload is active on the same target (would deadlock on mutex)
    if (_uploadActive && _uploadTarget == target) {
        STORAGE_LOG("FILE_DELETE rejected: upload active on %s", targetName(target));
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_DELETE %s:%s", targetName(target), path);

    // Check if target is a file or directory
    FileEntry entry;
    uint8_t infoErr;
    if (target == HubFxStorage::TARGET_FLASH)
        infoErr = FlashModule::instance().getFileInfo(path, entry);
    else
        infoErr = SdCardModule::instance().getFileInfo(path, entry);

    if (infoErr != 0) {
        sendNack(mapStorageError(infoErr));
        return;
    }

    uint8_t err;
    if (entry.isDirectory) {
        // Recursive directory removal
        if (target == HubFxStorage::TARGET_FLASH)
            err = FlashModule::instance().removeDirectory(path);
        else
            err = SdCardModule::instance().removeDirectory(path);
    } else {
        if (target == HubFxStorage::TARGET_FLASH)
            err = FlashModule::instance().removeFile(path);
        else
            err = SdCardModule::instance().removeFile(path);
    }

    if (err == 0) {
        STORAGE_LOG("deleted %s:%s%s", targetName(target), path,
                    entry.isDirectory ? " (recursive)" : "");
        sendAck();
    } else {
        sendNack(mapStorageError(err));
    }
}


// ============================================================================
// File Mkdir (0x9C)
// ============================================================================

void StorageServerBase::handleFileMkdir(const uint8_t* payload, size_t len) {
    char path[128];
    HubFxStorage::StorageTarget target = HubFxStorage::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH)
        err = FlashModule::instance().makeDirectory(path);
    else
        err = SdCardModule::instance().makeDirectory(path);

    if (err == 0) {
        STORAGE_LOG("mkdir %s:%s", targetName(target), path);
        sendAck();
    } else {
        sendNack(mapStorageError(err));
    }
}


// ============================================================================
// File Info (0x9D)
// ============================================================================

void StorageServerBase::handleFileInfo(const uint8_t* payload, size_t len) {
    char path[128];
    HubFxStorage::StorageTarget target = HubFxStorage::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    FileEntry entry;
    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH)
        err = FlashModule::instance().getFileInfo(path, entry);
    else
        err = SdCardModule::instance().getFileInfo(path, entry);

    // FILE_INFO_RESP: [exists:u8][isDir:u8][size:u32LE]
    uint8_t resp[6];
    if (err == FlashError::NOT_FOUND) {  // Same value as SdError::NOT_FOUND
        resp[0] = 0; resp[1] = 0;
        CoreProtocol::putU32LE(&resp[2], 0);
    } else if (err == 0) {
        resp[0] = 1;
        resp[1] = entry.isDirectory ? 1 : 0;
        CoreProtocol::putU32LE(&resp[2], entry.size);
    } else {
        sendNack(mapStorageError(err));
        return;
    }

    sendRawPacket(HubFxPacket::FILE_INFO_RESP, currentTag(), resp, sizeof(resp));
}


// ============================================================================
// File Download (0x9F) — Streamed Response
// ============================================================================

void StorageServerBase::handleFileDownload(const uint8_t* payload, size_t len) {
    char path[128];
    HubFxStorage::StorageTarget target = HubFxStorage::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    lockStorage(target);

    LFSFile file;
    uint8_t err;
    if (target == HubFxStorage::TARGET_FLASH)
        err = FlashModule::instance().openRead(path, file);
    else
        err = SdCardModule::instance().openRead(path, file);

    if (err != 0) {
        unlockStorage(target);
        sendNack(mapStorageError(err));
        return;
    }

    STORAGE_LOG("FILE_DOWNLOAD %s:%s size=%lu", targetName(target), path,
                (unsigned long)file.size());

    StreamWriter stream(*this, currentTag());
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
}


// ============================================================================
// File Upload Begin (0xA0)
// ============================================================================

void StorageServerBase::handleUploadBegin(const uint8_t* payload, size_t len) {
    // Wire: [size:u32LE][pathLen:u8][path:str][target:u8?]
    if (len < 6) {  // 4 (size) + 1 (pathLen) + 1 (min path char)
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (_uploadActive) {
        sendNack(HubFxError::UPLOAD_IN_PROGRESS);
        return;
    }

    uint32_t fileSize = CoreProtocol::getU32LE(payload);
    uint8_t pathLen = payload[4];

    if (pathLen == 0 || (size_t)(5 + pathLen) > len) {
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    if (pathLen >= sizeof(_uploadPath)) {
        sendNack(SerialError::INVALID_PARAM);
        return;
    }

    memcpy(_uploadPath, &payload[5], pathLen);
    _uploadPath[pathLen] = '\0';

    // Reject embedded null bytes in path data
    if (memchr(&payload[5], 0, pathLen) != nullptr) {
        sendNack(SerialError::INVALID_PARAM, "Path contains null byte");
        return;
    }

    // Validate path format (no traversal, must start with '/')
    if (!isValidPath(_uploadPath)) {
        sendNack(SerialError::INVALID_PARAM, "Invalid path");
        return;
    }

    // Optional target byte after path, optional mode byte after target
    _uploadTarget = HubFxStorage::TARGET_SD;
    _uploadMode   = HubFxStorage::UPLOAD_SYNC;
    size_t afterPath = 5 + pathLen;
    if (len > afterPath) {
        uint8_t rawTarget = payload[afterPath];
        if (rawTarget > HubFxStorage::TARGET_FLASH) {
            sendNack(SerialError::INVALID_PARAM, "Invalid storage target");
            return;
        }
        _uploadTarget = static_cast<HubFxStorage::StorageTarget>(rawTarget);
        if (len > afterPath + 1) {
            uint8_t rawMode = payload[afterPath + 1];
            if (rawMode == 2 || rawMode > HubFxStorage::UPLOAD_STREAM) {
                sendNack(SerialError::INVALID_PARAM, "Invalid upload mode");
                return;
            }
            _uploadMode = static_cast<HubFxStorage::UploadMode>(rawMode);
        }
    }

    if (!checkStorageReady(_uploadTarget)) return;

    // Check absolute maximum file size per target
    uint32_t maxSize = (_uploadTarget == HubFxStorage::TARGET_FLASH)
                     ? MAX_UPLOAD_SIZE_FLASH : MAX_UPLOAD_SIZE_SD;
    if (fileSize > maxSize) {
        sendNack(HubFxError::FILE_TOO_LARGE, "Exceeds maximum file size");
        return;
    }

    // Check available space
    if (_uploadTarget == HubFxStorage::TARGET_FLASH) {
        FlashStorageInfo info;
        FlashModule::instance().getStorageInfo(info);
        if (fileSize > info.freeBytes) {
            sendNack(HubFxError::FILE_TOO_LARGE);
            return;
        }
    } else {
        StorageInfo info;
        SdCardModule::instance().getStorageInfo(info);
        uint64_t freeBytes = (uint64_t)info.freeSpace_MB * 1024ULL * 1024ULL;
        if (fileSize > freeBytes) {
            sendNack(HubFxError::FILE_TOO_LARGE);
            return;
        }
    }

    // Open file for writing (truncate if exists)
    lockStorage(_uploadTarget);
    uint8_t err;
    if (_uploadTarget == HubFxStorage::TARGET_FLASH)
        err = FlashModule::instance().openWrite(_uploadPath, _uploadFile, true);
    else
        err = SdCardModule::instance().openWrite(_uploadPath, _uploadFile, true);

    if (err != 0) {
        unlockStorage(_uploadTarget);
        sendNack(mapStorageError(err));
        return;
    }

    // Allocate upload write buffers (platform-specific: PSRAM on ESP32, heap on Pico)
    if (_uploadMode == HubFxStorage::UPLOAD_STREAM) {
        if (!allocateStreamBuffers()) {
            _uploadFile.close();
            unlockStorage(_uploadTarget);
            sendNack(HubFxError::FILE_IO_ERROR, "Stream buffer alloc failed");
            return;
        }
    } else {
        if (!allocateUploadBuffers()) {
            _uploadFile.close();
            unlockStorage(_uploadTarget);
            sendNack(HubFxError::FILE_IO_ERROR, "Buffer alloc failed");
            return;
        }
    }

    _uploadActive       = true;
    _uploadExpectedSize = fileSize;
    _uploadBytesWritten = 0;
    _uploadExpectedSeq  = 0;
    _uploadCrcErrors    = 0;
    _uploadWriteBufLen  = 0;
    _uploadLastActivity_ms = millis();
    _uploadMd5.begin();

    if (_uploadMode == HubFxStorage::UPLOAD_STREAM) {
        // Enter raw stream receive mode — main loop will bypass COBS and
        // route Serial data to processStreamData() instead of server.loop()
        _streamBytesRemaining = fileSize;
        _streamBytesWrittenToSD = 0;
        _streamStagingLen = 0;
        _streamWriteError = false;

        // Platform hook: start stream writer task (ESP32) or no-op (Pico)
        if (!onStreamStart()) {
            _uploadFile.close();
            unlockStorage(_uploadTarget);
            freeStreamBuffers();
            _uploadActive = false;
            sendNack(HubFxError::FILE_IO_ERROR, "Stream writer task failed");
            return;
        }

        _streamReceiving = true;  // Must be set LAST — gates main loop branching
    }

    // Platform hook: reset writer error flags, etc.
    onUploadActivated(_uploadMode == HubFxStorage::UPLOAD_STREAM);

    const char* modeStr = "sync";
    if (_uploadMode == HubFxStorage::UPLOAD_BURST) modeStr = "burst";
    else if (_uploadMode == HubFxStorage::UPLOAD_STREAM) modeStr = "STREAM";

    STORAGE_LOG("UPLOAD_BEGIN %s:%s size=%lu mode=%s buf=%uKB",
                targetName(_uploadTarget), _uploadPath,
                (unsigned long)fileSize, modeStr,
                (unsigned)(_uploadMode == HubFxStorage::UPLOAD_STREAM
                    ? (streamBufferCapacityForLog() / 1024)
                    : (_uploadBufCapacity / 1024)));
    sendAck();
}


// ============================================================================
// File Upload Data (0xA1)
// ============================================================================

void StorageServerBase::handleUploadData(const uint8_t* payload, size_t len) {
    // Wire: [seqNum:u16LE][crc16:u16LE][data:N]
    if (!_uploadActive) {
        sendNack(HubFxError::NO_UPLOAD_ACTIVE);
        return;
    }

    if (len < 5) {  // 2 (seq) + 2 (crc) + 1 (min data)
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Check if background writer reported an error on previous buffer
    if (!checkAsyncWriterHealth()) {
        STORAGE_LOG("UPLOAD_DATA: async writer error — aborting upload");
        cleanupUpload(true);
        sendNack(HubFxError::FILE_IO_ERROR);
        return;
    }

    uint16_t seqNum   = CoreProtocol::getU16LE(payload);
    uint16_t rxCrc16  = CoreProtocol::getU16LE(&payload[2]);
    const uint8_t* data = &payload[4];
    size_t dataLen    = len - 4;
    // Burst mode skips per-chunk ACK / resync on mismatch
    bool isBurst = (_uploadMode == HubFxStorage::UPLOAD_BURST);

    // Verify sequence number
    if (seqNum != _uploadExpectedSeq) {
        STORAGE_LOG("UPLOAD_DATA seq mismatch: got %u expected %u",
                    seqNum, _uploadExpectedSeq);
        if (isBurst) {
            _uploadCrcErrors++;
            _uploadExpectedSeq = seqNum + 1;
            return;
        }
        sendNack(SerialError::INVALID_PARAM, "Sequence mismatch");
        return;
    }

    // Verify CRC-16
    uint16_t calcCrc = StreamProtocol::crc16(data, dataLen);
    if (calcCrc != rxCrc16) {
        STORAGE_LOG("UPLOAD_DATA CRC error seg %u: rx=0x%04X calc=0x%04X",
                    seqNum, rxCrc16, calcCrc);
        if (isBurst) {
            _uploadCrcErrors++;
        } else {
            sendNack(SerialError::CRC_ERROR);
            return;
        }
    }

    // Update activity timestamp (for inactivity timeout)
    _uploadLastActivity_ms = millis();

    // Would exceed expected file size?
    if (_uploadBytesWritten + dataLen > _uploadExpectedSize) {
        STORAGE_LOG("UPLOAD_DATA overflow: written=%lu + chunk=%u > expected=%lu",
                    (unsigned long)_uploadBytesWritten, (unsigned)dataLen,
                    (unsigned long)_uploadExpectedSize);
        cleanupUpload(true);
        sendNack(HubFxError::FILE_TOO_LARGE);
        return;
    }

    // Feed data to running MD5 hash (fast, stays on Core 0)
    _uploadMd5.add(const_cast<uint8_t*>(data), dataLen);
    _uploadBytesWritten += dataLen;
    _uploadExpectedSeq++;

    // Copy into fill buffer, flushing when full
    size_t remaining = dataLen;
    const uint8_t* src = data;
    while (remaining > 0) {
        size_t space = _uploadBufCapacity - _uploadWriteBufLen;
        size_t toCopy = (remaining < space) ? remaining : space;
        memcpy(&_uploadWriteBuf[_uploadWriteBufLen], src, toCopy);
        _uploadWriteBufLen += toCopy;
        src += toCopy;
        remaining -= toCopy;

        if (_uploadWriteBufLen >= _uploadBufCapacity) {
            if (!onUploadBufferFull()) {
                cleanupUpload(true);
                sendNack(HubFxError::FILE_IO_ERROR);
                return;
            }
        }
    }

    if (!isBurst) {
        sendAck();
    }
}


// ============================================================================
// File Upload End (0xA2)
// ============================================================================

void StorageServerBase::handleUploadEnd() {
    if (!_uploadActive) {
        sendNack(HubFxError::NO_UPLOAD_ACTIVE);
        return;
    }

    // Verify total bytes received
    if (_uploadBytesWritten != _uploadExpectedSize) {
        STORAGE_LOG("UPLOAD_END size mismatch: written=%lu expected=%lu",
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize);
        cleanupUpload(true);
        sendNack(HubFxError::FILE_IO_ERROR, "Size mismatch");
        return;
    }

    if (_uploadMode == HubFxStorage::UPLOAD_STREAM) {
        // Check for write errors that occurred during streaming
        if (_streamWriteError) {
            cleanupUpload(true);
            sendNack(HubFxError::FILE_IO_ERROR, "Stream write error");
            return;
        }

        // Platform hook: wait for writer drain (ESP32) or flush remaining staging (Pico)
        const char* errMsg = nullptr;
        if (!onStreamEnd(errMsg)) {
            cleanupUpload(true);
            sendNack(HubFxError::FILE_IO_ERROR, errMsg ? errMsg : "Stream end error");
            return;
        }
    } else {
        // Chunked mode: platform hook for async writer completion
        const char* errMsg = nullptr;
        if (!onChunkedEnd(errMsg)) {
            cleanupUpload(true);
            sendNack(HubFxError::FILE_IO_ERROR, errMsg ? errMsg : "Async write failed");
            return;
        }
    }

    // Flush remaining write buffer to file (blocking — final partial block)
    // Stream mode has no write buffer (uses ring buffer, already drained above)
    if (_uploadMode != HubFxStorage::UPLOAD_STREAM && _uploadWriteBufLen > 0) {
        if (!flushUploadBuffer()) {
            cleanupUpload(true);
            sendNack(HubFxError::FILE_IO_ERROR, "Final flush failed");
            return;
        }
    }

    // Force data to storage media before closing
    _uploadFile.flush();

    // Close file and release lock
    _uploadFile.close();
    unlockStorage(_uploadTarget);

    // Compute final MD5 digest
    _uploadMd5.calculate();
    uint8_t md5Bytes[16];
    _uploadMd5.getBytes(md5Bytes);

    bool isBurst = (_uploadMode == HubFxStorage::UPLOAD_BURST);
    bool isStream = (_uploadMode == HubFxStorage::UPLOAD_STREAM);

    STORAGE_LOG("UPLOAD_END %s:%s %lu bytes OK (md5=%s%s%s)", targetName(_uploadTarget),
                _uploadPath, (unsigned long)_uploadBytesWritten,
                _uploadMd5.toString().c_str(),
                isBurst && _uploadCrcErrors > 0 ? " CRC_ERRORS" : "",
                isStream ? " STREAM" : "");

    _uploadActive = false;

    // Free the appropriate buffers for the upload mode
    if (isStream) {
        freeStreamBuffers();
    } else {
        freeUploadBuffers();
    }

    // ACK payload: [md5:16B] or [md5:16B][crcErrors:u16LE] (burst)
    if (isBurst && _uploadCrcErrors > 0) {
        uint8_t ackPayload[18];
        memcpy(ackPayload, md5Bytes, 16);
        CoreProtocol::putU16LE(&ackPayload[16], _uploadCrcErrors);
        sendRawPacket(CorePacket::ACK, _currentTag, ackPayload, 18);
    } else {
        sendRawPacket(CorePacket::ACK, _currentTag, md5Bytes, 16);
    }
}


// ============================================================================
// File Upload Cancel (0xA3)
// ============================================================================

void StorageServerBase::handleUploadCancel() {
    if (!_uploadActive) {
        sendNack(HubFxError::NO_UPLOAD_ACTIVE);
        return;
    }

    STORAGE_LOG("UPLOAD_CANCEL %s:%s (received %lu/%lu bytes)",
                targetName(_uploadTarget), _uploadPath,
                (unsigned long)_uploadBytesWritten,
                (unsigned long)_uploadExpectedSize);

    cleanupUpload(true);
    sendAck();
}


// ============================================================================
// Upload Helpers
// ============================================================================

void StorageServerBase::cleanupUpload(bool deletePartial) {
    if (!_uploadActive) return;

    if (_uploadMode == HubFxStorage::UPLOAD_STREAM) {
        _streamReceiving = false;
        onStreamCleanup();
        freeStreamBuffers();
    } else {
        onChunkedCleanup();
        // Discard any buffered data (don't flush on cleanup/cancel)
        _uploadWriteBufLen = 0;
        freeUploadBuffers();
    }

    _uploadFile.close();

    if (deletePartial) {
        // We already hold the storage lock from handleUploadBegin(),
        // so use getFS() directly instead of removeFile() which re-locks.
        if (_uploadTarget == HubFxStorage::TARGET_FLASH) {
            FlashModule::instance().getFS().remove(_uploadPath);
        } else {
            SdCardModule::instance().getFS().remove(_uploadPath);
        }
        STORAGE_LOG("Deleted partial upload: %s:%s", targetName(_uploadTarget), _uploadPath);
    }

    unlockStorage(_uploadTarget);
    _uploadActive = false;
}

void StorageServerBase::cancelActiveUpload() {
    if (_uploadActive) {
        STORAGE_LOG("Cancelling active upload on shutdown: %s", _uploadPath);
        cleanupUpload(true);
    }
}

bool StorageServerBase::flushUploadBuffer() {
    if (_uploadWriteBufLen == 0) return true;

    size_t written = _uploadFile.write(_uploadWriteBuf, _uploadWriteBufLen);
    if (written != _uploadWriteBufLen) {
        STORAGE_LOG("Write buffer flush failed: expected %u wrote %u",
                    (unsigned)_uploadWriteBufLen, (unsigned)written);
        _uploadWriteBufLen = 0;
        return false;
    }

    _uploadWriteBufLen = 0;
    return true;
}


// ============================================================================
// Raw Stream Data Processing (UPLOAD_STREAM mode, Core 0)
// ============================================================================

void StorageServerBase::processStreamData(Stream& serial) {
    uint8_t tmp[2048];

    while (_streamBytesRemaining > 0) {
        int avail = serial.available();
        if (avail <= 0) break;  // No data available — return, will be called again

        // Read exactly what we need (no more), clamped to temp buffer size
        size_t toRead = (size_t)avail;
        if (toRead > _streamBytesRemaining) toRead = _streamBytesRemaining;
        if (toRead > sizeof(tmp)) toRead = sizeof(tmp);

        size_t got = serial.readBytes(tmp, toRead);
        if (got == 0) break;  // Shouldn't happen, but be safe

        // Feed running MD5 hash
        _uploadMd5.add(tmp, got);

        // Platform-specific: ring buffer (ESP32) or staging + inline flush (Pico)
        onStreamDataReceived(tmp, got);

        _streamBytesRemaining -= got;
        _uploadBytesWritten += got;
        _uploadLastActivity_ms = millis();
    }

    // Check if all expected bytes have been received
    if (_streamBytesRemaining == 0 && _streamReceiving) {
        _streamReceiving = false;
        onStreamReceiveComplete();
    } else if (_streamBytesRemaining > 0 && _streamReceiving) {
        // Check for stream data stall — if no serial data arrives for
        // STREAM_DATA_TIMEOUT_MS, some bytes were likely lost (UART HW FIFO
        // overflow, USB-UART bridge issue, etc.).  Exit stream mode NOW so
        // subsequent COBS packets from the client (UPLOAD_END, UPLOAD_CANCEL)
        // are parsed as protocol — not consumed as raw file data.
        uint32_t elapsed = millis() - _uploadLastActivity_ms;
        if (elapsed >= STREAM_DATA_TIMEOUT_MS) {
            STORAGE_LOG("Stream data stall: %lu bytes still expected "
                        "(received %lu/%lu, gap %lus) — aborting upload",
                        (unsigned long)_streamBytesRemaining,
                        (unsigned long)_uploadBytesWritten,
                        (unsigned long)_uploadExpectedSize,
                        (unsigned long)(elapsed / 1000));
            _streamReceiving = false;
            // Let the writer task drain whatever data is in the ring buffer,
            // then clean up immediately (delete partial file, stop writer,
            // free buffers).  This is faster than waiting for the 30s general
            // upload timeout, and leaves the server ready for the next command.
            onStreamReceiveComplete();
            cleanupUpload(true);
        }
    }
}


// ============================================================================
// Upload Timeout
// ============================================================================

void StorageServerBase::checkUploadTimeout() {
    if (!_uploadActive) return;

    uint32_t elapsed = millis() - _uploadLastActivity_ms;
    if (elapsed >= UPLOAD_TIMEOUT_MS) {
        STORAGE_LOG("Upload inactivity timeout (%lus) — cancelling %s:%s "
                    "(received %lu/%lu bytes)",
                    (unsigned long)(elapsed / 1000),
                    targetName(_uploadTarget), _uploadPath,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize);
        cleanupUpload(true);
    }
}


// ============================================================================
// Helpers
// ============================================================================

uint8_t StorageServerBase::mapStorageError(uint8_t err) {
    // FlashError and SdError share identical codes (0-6)
    switch (err) {
        case FlashError::OK:              return SerialError::OK;
        case FlashError::NOT_INITIALIZED: return SerialError::NOT_INITIALIZED;
        case FlashError::NOT_FOUND:       return HubFxError::FILE_NOT_FOUND;
        case FlashError::IO_ERROR:        return HubFxError::FILE_IO_ERROR;
        case FlashError::IS_DIRECTORY:    return HubFxError::FILE_IO_ERROR;
        case FlashError::ALREADY_EXISTS:  return HubFxError::FILE_ALREADY_EXISTS;
        case FlashError::LIMIT_EXCEEDED:  return SerialError::UNKNOWN;
        default:                          return SerialError::UNKNOWN;
    }
}


// ============================================================================
// SD Card Init (0x93) — Remount SD card
// ============================================================================

void StorageServerBase::handleSdInit(const uint8_t* payload, size_t len) {
    SdCardModule& sd = SdCardModule::instance();

    // SD_INIT payload: [speed_mhz:u8] — ignored for SDIO mode
    uint8_t speed = (len >= 1) ? payload[0] : 0;

    STORAGE_LOG("SD_INIT: remounting (speed=%u)", speed);

    // retryInit() calls unmount() first, then re-mounts
    bool ok = sd.retryInit(speed);
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
        sendNack(HubFxError::SD_NOT_INITIALIZED, "SD init failed");
    }
}


// ============================================================================
// SD Card Status (0x94)
// ============================================================================

void StorageServerBase::handleSdStatus() {
    SdCardModule& sd = SdCardModule::instance();

    // SD_STATUS_RESP extended:
    //   [initialized:u8][cardSize_MB:u32LE][totalSpace_MB:u32LE]
    //   [freeSpace_MB:u32LE][fatType:u8]
    //   [cardType:u8][busMode:u8][usedSpace_MB:u32LE]
    // = 20 bytes total
    uint8_t resp[20] = {0};

    if (!sd.isInitialized()) {
        resp[0] = 0;
        sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), resp, sizeof(resp));
        return;
    }

    StorageInfo info;
    sd.getStorageInfo(info);

    resp[0] = 1;
    CoreProtocol::putU32LE(&resp[1], info.cardSize_MB);
    CoreProtocol::putU32LE(&resp[5], info.totalSpace_MB);
    CoreProtocol::putU32LE(&resp[9], info.freeSpace_MB);
    resp[13] = info.fatType;
    // Extended fields
    resp[14] = (uint8_t)info.cardType;
    resp[15] = (uint8_t)info.busMode;
    CoreProtocol::putU32LE(&resp[16], info.usedSpace_MB);

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

    sendRawPacket(HubFxPacket::SD_STATUS_RESP, currentTag(), resp, sizeof(resp));
}


// ============================================================================
// Storage Helpers
// ============================================================================

bool StorageServerBase::checkStorageReady(HubFxStorage::StorageTarget target) {
    if (target == HubFxStorage::TARGET_FLASH) {
        if (!FlashModule::instance().isInitialized()) {
            sendNack(SerialError::NOT_INITIALIZED);
            return false;
        }
    } else {
        if (!SdCardModule::instance().isInitialized()) {
            sendNack(HubFxError::SD_NOT_INITIALIZED);
            return false;
        }
    }
    return true;
}

void StorageServerBase::lockStorage(HubFxStorage::StorageTarget target) {
    if (target == HubFxStorage::TARGET_FLASH)
        FlashModule::instance().lock();
    else
        SdCardModule::instance().lock();
}

void StorageServerBase::unlockStorage(HubFxStorage::StorageTarget target) {
    if (target == HubFxStorage::TARGET_FLASH)
        FlashModule::instance().unlock();
    else
        SdCardModule::instance().unlock();
}

const char* StorageServerBase::targetName(HubFxStorage::StorageTarget target) {
    return (target == HubFxStorage::TARGET_FLASH) ? "flash" : "sd";
}

uint8_t StorageServerBase::extractPathAndTarget(
        const uint8_t* payload, size_t len,
        char* path, size_t pathBufSize, HubFxStorage::StorageTarget& target) {

    if (len < 1) return SerialError::MISSING_PARAMETER;

    uint8_t pathLen = payload[0];
    if (pathLen == 0 || (size_t)(1 + pathLen) > len) return SerialError::MISSING_PARAMETER;
    if (pathLen >= pathBufSize) return SerialError::PARAM_TOO_LONG;

    // Reject embedded null bytes in path data
    if (memchr(&payload[1], 0, pathLen) != nullptr) return SerialError::INVALID_PARAM;

    memcpy(path, &payload[1], pathLen);
    path[pathLen] = '\0';

    // Validate path format (no traversal, must start with '/')
    if (!isValidPath(path)) return SerialError::INVALID_PARAM;

    // Optional target byte after the path
    if (len > (size_t)(1 + pathLen)) {
        uint8_t rawTarget = payload[1 + pathLen];
        if (rawTarget > HubFxStorage::TARGET_FLASH) return SerialError::INVALID_PARAM;
        target = static_cast<HubFxStorage::StorageTarget>(rawTarget);
    } else {
        target = HubFxStorage::TARGET_SD;  // default
    }

    return SerialError::OK;
}


// ============================================================================
// Path Validation
// ============================================================================

bool StorageServerBase::isValidPath(const char* path) {
    if (!path || path[0] != '/') return false;

    // Reject path traversal: ".." as a path component
    const char* p = path;
    while ((p = strstr(p, "..")) != nullptr) {
        bool preceded = (p == path || *(p - 1) == '/');
        bool followed = (*(p + 2) == '\0' || *(p + 2) == '/');
        if (preceded && followed) return false;
        p += 2;
    }

    return true;
}
