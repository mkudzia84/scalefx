/*
 * Storage Server — Policy-Based Template Implementation
 *
 * Platform-agnostic protocol handlers for file operations via FlashModule
 * and SdCardModule singletons. File list and download use StreamWriter
 * for chunked transfer.
 *
 * Platform-specific behavior (buffer allocation, async writes) is resolved
 * at compile time via policy composition:
 * TPolicy provides platform-specific implementations, accessed through
 * the _policy member (no virtual dispatch, no CRTP self-casting).
 *
 * Buffer sizing rationale (6 Mbps serial):
 *   - Raw byte rate: 750 KB/s, effective ~580 KB/s with UART/COBS overhead
 *   - Stream mode bypasses COBS for data plane - near wire-rate throughput
 *   - Each COBS chunk (sync): 2044 data + 4 header = 2048 payload, ~2060 bytes on wire
 *   - UART RX buffer: 128 KB - holds ~220ms of data at line rate
 *   - Stream segments: 512 KB SD / 128 KB flash; 64 KB fill buffer on ESP32
 *
 * Included from storage_service.h -- do not include directly.
 */

#ifndef STORAGE_SERVICE_IPP
#define STORAGE_SERVICE_IPP

#include <serial/diag_log.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


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

        // Upload commands (0xA0-0xA3)
        case StoragePacket::FILE_UPLOAD_BEGIN:
            handleUploadBegin(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_DATA:
            handleUploadData(payload, len);
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_END:
            handleUploadEnd();
            return CommandHandleResult::Handled;

        case StoragePacket::FILE_UPLOAD_CANCEL:
            handleUploadCancel();
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

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_LIST %s:%s", targetName(target), path);

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

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    STORAGE_LOG("FILE_TREE %s:%s", targetName(target), path);

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

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target, &flags);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    // Guard: reject if upload is active on the same target (would deadlock on mutex)
    if (_uploadActive && _uploadTarget == target) {
        STORAGE_LOG("FILE_DELETE rejected: upload active on %s", targetName(target));
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    // Legacy default (no flags byte) = recursive, matching pre-v2 server behavior.
    // Explicit flags byte = client controls recursion via DeleteFlags::RECURSIVE bit.
    bool recursive = flagsPresent ? ((flags & StorageWire::DeleteFlags::RECURSIVE) != 0) : true;

    STORAGE_LOG("FILE_DELETE %s:%s flags=0x%02X recursive=%d",
                targetName(target), path, flags, recursive ? 1 : 0);

    // Check if target is a file or directory
    FileEntry entry;
    uint8_t infoErr;
    if (target == StorageWire::TARGET_FLASH)
        infoErr = FlashModule::instance().getFileInfo(path, entry);
    else
        infoErr = SdCardModule::instance().getFileInfo(path, entry);

    if (infoErr != 0) {
        sendNack(mapStorageError(infoErr));
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
        STORAGE_LOG("deleted %s:%s%s", targetName(target), path,
                    entry.isDirectory ? (recursive ? " (recursive)" : " (empty)") : "");
        sendAck();
    } else {
        sendNack(mapStorageError(err));
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

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target, &flags);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
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
        STORAGE_LOG("mkdir %s:%s%s", targetName(target), path,
                    createParents ? " -p" : "");
        sendAck();
    } else {
        sendNack(mapStorageError(err));
    }
}


// ============================================================================
// File Info (0x9D)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleFileInfo(const uint8_t* payload, size_t len) {
    char path[128];
    StorageWire::StorageTarget target = StorageWire::TARGET_SD;

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
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
        sendNack(mapStorageError(err));
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

    uint8_t pathErr = extractPathAndTarget(payload, len, path, sizeof(path), target);
    if (pathErr != SerialError::OK) { sendNack(pathErr); return; }
    if (!checkStorageReady(target)) return;

    if (_uploadActive && _uploadTarget == target) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    lockStorage(target);

    LFSFile file;
    uint8_t err;
    if (target == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().openRead(path, file);
    else
        err = _policy.sdOpenRead(path, file);

    if (err != 0) {
        unlockStorage(target);
        sendNack(mapStorageError(err));
        return;
    }

    STORAGE_LOG("FILE_DOWNLOAD %s:%s size=%lu", targetName(target), path,
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
// File Upload Begin (0xA0)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleUploadBegin(const uint8_t* payload, size_t len) {
    // Wire: [size:u32LE][pathLen:u8][path:str][target:u8?]
    if (len < 6) {  // 4 (size) + 1 (pathLen) + 1 (min path char)
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    if (_uploadActive) {
        sendNack(StorageError::UPLOAD_IN_PROGRESS);
        return;
    }

    uint32_t fileSize = SfxWire::getU32LE(payload);
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
    _uploadTarget = StorageWire::TARGET_SD;
    _uploadMode   = StorageWire::UPLOAD_SYNC;
    size_t afterPath = 5 + pathLen;
    if (len > afterPath) {
        uint8_t rawTarget = payload[afterPath];
        if (rawTarget > StorageWire::TARGET_FLASH) {
            sendNack(SerialError::INVALID_PARAM, "Invalid storage target");
            return;
        }
        _uploadTarget = static_cast<StorageWire::StorageTarget>(rawTarget);
        if (len > afterPath + 1) {
            uint8_t rawMode = payload[afterPath + 1];
            if (rawMode != StorageWire::UPLOAD_SYNC &&
                rawMode != StorageWire::UPLOAD_STREAM) {
                sendNack(SerialError::INVALID_PARAM, "Invalid upload mode");
                return;
            }
            _uploadMode = static_cast<StorageWire::UploadMode>(rawMode);
        }
    }

    if (!checkStorageReady(_uploadTarget)) return;

    // Check absolute maximum file size per target
    uint32_t maxSize = (_uploadTarget == StorageWire::TARGET_FLASH)
                     ? MAX_UPLOAD_SIZE_FLASH : MAX_UPLOAD_SIZE_SD;
    if (fileSize > maxSize) {
        sendNack(StorageError::FILE_TOO_LARGE, "Exceeds maximum file size");
        return;
    }

    // Check available space
    if (_uploadTarget == StorageWire::TARGET_FLASH) {
        FlashStorageInfo info;
        FlashModule::instance().getStorageInfo(info);
        if (fileSize > info.freeBytes) {
            sendNack(StorageError::FILE_TOO_LARGE);
            return;
        }
    } else {
        StorageInfo info;
        SdCardModule::instance().getStorageInfo(info);
        uint64_t freeBytes = (uint64_t)info.freeSpace_MB * 1024ULL * 1024ULL;
        if (fileSize > freeBytes) {
            sendNack(StorageError::FILE_TOO_LARGE);
            return;
        }
    }

    // Open file for writing (truncate if exists)
    lockStorage(_uploadTarget);
    uint8_t err;
    if (_uploadTarget == StorageWire::TARGET_FLASH)
        err = FlashModule::instance().openWrite(_uploadPath, _shared.uploadFile, true);
    else
        err = _policy.sdOpenWrite(_uploadPath, _shared.uploadFile, true);

    if (err != 0) {
        unlockStorage(_uploadTarget);
        sendNack(mapStorageError(err));
        return;
    }

    // Allocate upload write buffers (platform-specific: PSRAM on ESP32, heap on Pico)
    if (!_policy.allocateUploadBuffers()) {
        _shared.uploadFile.close();
        unlockStorage(_uploadTarget);
        sendNack(StorageError::FILE_IO_ERROR, "Buffer alloc failed");
        return;
    }

    _uploadActive       = true;
    _uploadExpectedSize = fileSize;
    _uploadBytesWritten = 0;
    _uploadExpectedSeq  = 0;
    _shared.uploadWriteBufLen  = 0;
    _uploadLastActivity_ms = millis();
    _uploadMd5.begin();

    // Stream mode state
    _streamActive          = false;
    _streamSegmentIndex    = 0;
    _streamSegBytesRemaining = 0;
    _streamSegmentSize     = 0;
    _streamSegmentCount    = 0;

    // Platform hook: reset per-upload counters, etc.
    _policy.onUploadActivated();

    const char* modeStr = (_uploadMode == StorageWire::UPLOAD_STREAM) ? "stream" : "sync";

    STORAGE_LOG("UPLOAD_BEGIN %s:%s size=%lu mode=%s buf=%uKB",
                targetName(_uploadTarget), _uploadPath,
                (unsigned long)fileSize, modeStr,
                (unsigned)(_shared.uploadBufCapacity / 1024));

    // Suppress STATUS_UPDATE for the duration of the upload
    notifyTransferStart();

    // Fire upload-start hook so the firmware can make this upload exclusive
    // (suspend audio tasks, engine, USB host poll, etc.). Fires for BOTH
    // sync and batch/stream modes — upload is always exclusive on HubFX.
    if (_onUploadStart) {
        _onUploadStart();
        _uploadSuspended = true;
    }

    if (_uploadMode == StorageWire::UPLOAD_STREAM) {
        // Pre-allocate file on SD for contiguous cluster allocation
        if (_uploadTarget == StorageWire::TARGET_SD && fileSize > 0) {
            _shared.uploadFile.seek(fileSize - 1);
            uint8_t zero = 0;
            _shared.uploadFile.write(&zero, 1);
            _shared.uploadFile.seek(0);
        }

        // Compute segment parameters. LittleFS (flash) writes are ~5-10x
        // slower than SD_MMC — shrink the segment so the client's ACK deadline
        // isn't exceeded during a single flash-erase-and-program cycle.
        _streamSegmentSize = (_uploadTarget == StorageWire::TARGET_FLASH)
                             ? STREAM_SEGMENT_SIZE_FLASH
                             : STREAM_SEGMENT_SIZE;
        uint16_t segCount = (uint16_t)((fileSize + _streamSegmentSize - 1) / _streamSegmentSize);
        _streamSegBytesRemaining = (fileSize < _streamSegmentSize) ? fileSize : _streamSegmentSize;
        _streamSegmentIndex = 0;
        _streamSegmentCount = segCount;
        _streamActive = true;

        // Initialize stream diagnostics
        _streamStartTime_ms    = millis();
        _streamSegStartTime_ms = millis();
        _streamEndTime_ms      = 0;
        _streamLastLogTime_ms  = millis();
        _streamLastLogBytes    = 0;
        _streamLastCallTime_ms = millis();
        _streamMaxGap_ms       = 0;
        _streamIterCount       = 0;

        STORAGE_LOG("STREAM active: %u segments of %uKB, uart_rx=%dB",
                    segCount, (unsigned)(_streamSegmentSize / 1024),
                    serial()->available());

        // ACK payload: [segment_size:u32LE][segment_count:u16LE]
        uint8_t ackPayload[6];
        SfxWire::putU32LE(&ackPayload[0], _streamSegmentSize);
        SfxWire::putU16LE(&ackPayload[4], segCount);
        sendRawPacket(CorePacket::ACK, currentTag(), ackPayload, 6);
    } else {
        sendAck();
    }
}


// ============================================================================
// File Upload Data (0xA1)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleUploadData(const uint8_t* payload, size_t len) {
    // Wire: [seqNum:u16LE][crc16:u16LE][data:N]
    if (!_uploadActive) {
        sendNack(StorageError::NO_UPLOAD_ACTIVE);
        return;
    }

    if (len < 5) {  // 2 (seq) + 2 (crc) + 1 (min data)
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Policy health check — a no-op for single-core policies, reserved
    // for a future async writer to signal a deferred error.
    if (!_policy.checkAsyncWriterHealth()) {
        STORAGE_LOG("UPLOAD_DATA: policy signalled error -- aborting upload");
        cleanupUpload(true);
        sendNack(StorageError::FILE_IO_ERROR);
        return;
    }

    uint16_t seqNum   = SfxWire::getU16LE(payload);
    uint16_t rxCrc16  = SfxWire::getU16LE(&payload[2]);
    const uint8_t* data = &payload[4];
    size_t dataLen    = len - 4;

    // Verify sequence number (sync mode only — stream mode has no seq)
    if (seqNum != _uploadExpectedSeq) {
        STORAGE_LOG("UPLOAD_DATA seq mismatch: got %u expected %u",
                    seqNum, _uploadExpectedSeq);
        sendNack(SerialError::INVALID_PARAM, "Sequence mismatch");
        return;
    }

    // Verify CRC-16
    uint16_t calcCrc = StreamProtocol::crc16(data, dataLen);
    if (calcCrc != rxCrc16) {
        STORAGE_LOG("UPLOAD_DATA CRC error seg %u: rx=0x%04X calc=0x%04X",
                    seqNum, rxCrc16, calcCrc);
        sendNack(SerialError::CRC_ERROR);
        return;
    }

    // Update activity timestamp (for inactivity timeout)
    _uploadLastActivity_ms = millis();

    // Would exceed expected file size?
    if (_uploadBytesWritten + dataLen > _uploadExpectedSize) {
        STORAGE_LOG("UPLOAD_DATA overflow: written=%lu + chunk=%u > expected=%lu",
                    (unsigned long)_uploadBytesWritten, (unsigned)dataLen,
                    (unsigned long)_uploadExpectedSize);
        cleanupUpload(true);
        sendNack(StorageError::FILE_TOO_LARGE);
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
        size_t space = _shared.uploadBufCapacity - _shared.uploadWriteBufLen;
        size_t toCopy = (remaining < space) ? remaining : space;
        memcpy(&_shared.uploadWriteBuf[_shared.uploadWriteBufLen], src, toCopy);
        _shared.uploadWriteBufLen += toCopy;
        src += toCopy;
        remaining -= toCopy;

        if (_shared.uploadWriteBufLen >= _shared.uploadBufCapacity) {
            if (!_policy.onUploadBufferFull()) {
                cleanupUpload(true);
                sendNack(StorageError::FILE_IO_ERROR);
                return;
            }
        }
    }

    // Sync mode: ACK each chunk
    sendAck();
}


// ============================================================================
// File Upload End (0xA2)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleUploadEnd() {
    if (!_uploadActive) {
        sendNack(StorageError::NO_UPLOAD_ACTIVE);
        return;
    }

    uint32_t tEndReceived = millis();

    // Log gap between stream completion and UPLOAD_END reception
    if (_uploadMode == StorageWire::UPLOAD_STREAM && _streamEndTime_ms > 0) {
        uint32_t gapSinceStreamEnd = tEndReceived - _streamEndTime_ms;
        STORAGE_LOG("UPLOAD_END received %lums after stream end (rx=%lu/%lu)",
                    (unsigned long)gapSinceStreamEnd,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize);
    }

    // Verify total bytes received
    if (_uploadBytesWritten != _uploadExpectedSize) {
        STORAGE_LOG("UPLOAD_END size mismatch: written=%lu expected=%lu",
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize);
        cleanupUpload(true);
        sendNack(StorageError::FILE_IO_ERROR, "Size mismatch");
        return;
    }

    // Platform hook for end-of-upload finalization (async writer drain on
    // policies that have one; no-op for the inline single-core path).
    uint32_t t0 = millis();
    const char* errMsg = nullptr;
    if (!_policy.onChunkedEnd(errMsg)) {
        STORAGE_LOG("UPLOAD_END drain failed after %lums: %s",
                    (unsigned long)(millis() - t0),
                    errMsg ? errMsg : "unknown");
        cleanupUpload(true);
        sendNack(StorageError::FILE_IO_ERROR, errMsg ? errMsg : "Async write failed");
        return;
    }
    uint32_t t1 = millis();

    // Flush remaining write buffer to file (blocking — final partial block)
    if (_shared.uploadWriteBufLen > 0) {
        if (!_shared.flushUploadBuffer()) {
            cleanupUpload(true);
            sendNack(StorageError::FILE_IO_ERROR, "Final flush failed");
            return;
        }
    }
    uint32_t t2 = millis();

    // Force data to storage media before closing
    _shared.uploadFile.flush();
    _shared.uploadFile.close();
    unlockStorage(_uploadTarget);
    uint32_t t3 = millis();

    // Compute final MD5 digest
    _uploadMd5.calculate();
    uint8_t md5Bytes[16];
    _uploadMd5.getBytes(md5Bytes);
    uint32_t t4 = millis();

    const char* modeStr = (_uploadMode == StorageWire::UPLOAD_STREAM) ? "stream" : "sync";

    auto ws = _policy.writerStats();
    uint32_t avgLat = (ws.writeCount > 0)
        ? (ws.totalStallTime_ms / ws.writeCount) : 0;
    STORAGE_LOG("UPLOAD_END %s:%s %lu bytes OK (md5=%s mode=%s) "
                "finalize=%lums flush=%lums close=%lums md5=%lums total=%lums",
                targetName(_uploadTarget), _uploadPath,
                (unsigned long)_uploadBytesWritten,
                _uploadMd5.toString().c_str(), modeStr,
                (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                (unsigned long)(t3 - t2), (unsigned long)(t4 - t3),
                (unsigned long)(t4 - tEndReceived));
    STORAGE_LOG("SD write stats: %luKB in %lu writes, "
                "avg_lat=%lums max_lat=%lums total_io=%lums",
                (unsigned long)(ws.bytesWritten / 1024),
                (unsigned long)ws.writeCount,
                (unsigned long)avgLat,
                (unsigned long)ws.maxWriteLatency_ms,
                (unsigned long)ws.totalStallTime_ms);

    _uploadActive = false;
    _streamActive = false;
    _policy.freeUploadBuffers();

    // Resume resources suspended for the duration of the upload
    if (_uploadSuspended) {
        if (_onUploadEnd) _onUploadEnd();
        _uploadSuspended = false;
    }

    // Re-enable STATUS_UPDATE after transfer completes
    notifyTransferEnd();

    // ACK payload: [md5:16B]
    sendRawPacket(CorePacket::ACK, currentTag(), md5Bytes, 16);
}


// ============================================================================
// File Upload Cancel (0xA3)
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleUploadCancel() {
    if (!_uploadActive) {
        sendNack(StorageError::NO_UPLOAD_ACTIVE);
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

// --- Stream Upload Helpers ---

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::processStream() {
    if (!_streamActive || !_uploadActive) return;

    // Track loop gap — detect slow main loop iterations
    uint32_t now = millis();
    uint32_t gap = now - _streamLastCallTime_ms;
    _streamLastCallTime_ms = now;
    if (gap > _streamMaxGap_ms) _streamMaxGap_ms = gap;

    // Warn on excessive gap (UART RX buffer could overflow)
    if (gap > 50) {
        STORAGE_LOG("Stream loop gap: %lums (max=%lu) uart=%d",
                    (unsigned long)gap, (unsigned long)_streamMaxGap_ms,
                    serial()->available());
    }

    _streamIterCount++;

    // Policy health check (reserved for a future async writer — no-op today)
    if (!_policy.checkAsyncWriterHealth()) {
        STORAGE_LOG("Stream: policy signalled error - aborting "
                    "(seg=%u/%u rx=%lu/%lu)",
                    _streamSegmentIndex, _streamSegmentCount,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize);
        _streamActive = false;
        // Drain incoming raw bytes so COBS parser doesn't see garbage
        while (serial()->available()) serial()->read();
        cleanupUpload(true);
        return;
    }

    int avail = serial()->available();
    if (avail <= 0) return;

    _uploadLastActivity_ms = now;

    // Accumulate into the fill buffer. Policies see consistent buffer-full
    // writes (typically 64 KB) instead of many small writes driven by UART
    // arrival bursts — critical when the policy writes directly to flash
    // or SD (small LittleFS writes are 10-20x slower than batched ones).
    size_t space = _shared.uploadBufCapacity - _shared.uploadWriteBufLen;
    size_t toRead = (size_t)avail;
    if (toRead > _streamSegBytesRemaining) toRead = _streamSegBytesRemaining;
    if (toRead > space)                    toRead = space;

    size_t got = serial()->readBytes(
        &_shared.uploadWriteBuf[_shared.uploadWriteBufLen], toRead);
    if (got == 0) return;

    // Feed running MD5 hash (fast, stays on Core 0)
    _uploadMd5.add(&_shared.uploadWriteBuf[_shared.uploadWriteBufLen], got);
    _uploadBytesWritten      += got;
    _streamSegBytesRemaining -= got;
    _shared.uploadWriteBufLen += got;

    // Flush when buffer full, or when the segment boundary has been reached
    // (policies need the in-flight bytes on disk before we ACK).
    bool needFlush = (_shared.uploadWriteBufLen >= _shared.uploadBufCapacity)
                  || (_streamSegBytesRemaining == 0 && _shared.uploadWriteBufLen > 0);

    if (needFlush && !_policy.onUploadBufferFull()) {
        STORAGE_LOG("Stream: buffer flush failed - aborting "
                    "(seg=%u/%u rx=%lu/%lu fill=%u%%)",
                    _streamSegmentIndex, _streamSegmentCount,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize,
                    _policy.bufferFillPercent());
        _streamActive = false;
        while (serial()->available()) serial()->read();
        cleanupUpload(true);
        return;
    }

    // Periodic progress logging (every ~2 seconds)
    if (now - _streamLastLogTime_ms >= 2000) {
        uint32_t bytesInPeriod = _uploadBytesWritten - _streamLastLogBytes;
        uint32_t elapsed_ms = now - _streamLastLogTime_ms;
        uint32_t kbps = (elapsed_ms > 0) ? (bytesInPeriod / elapsed_ms) : 0;
        auto ws = _policy.writerStats();
        uint32_t sdKBps = (elapsed_ms > 0 && ws.writeCount > 0)
            ? (ws.bytesWritten / (now - _streamStartTime_ms)) : 0;
        uint32_t avgLat = (ws.writeCount > 0)
            ? (ws.totalStallTime_ms / ws.writeCount) : 0;
        STORAGE_LOG("Stream seg=%u/%u rx=%lu/%lu (%u%%) "
                    "rate=%luKB/s fill=%u%% uart=%d maxgap=%lums iter=%lu "
                    "sd_written=%luKB sd_rate=%luKB/s sd_writes=%lu "
                    "sd_avglat=%lums sd_maxlat=%lums",
                    _streamSegmentIndex, _streamSegmentCount,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize,
                    (unsigned)(_uploadBytesWritten * 100 / _uploadExpectedSize),
                    (unsigned long)kbps,
                    _policy.bufferFillPercent(),
                    serial()->available(),
                    (unsigned long)_streamMaxGap_ms,
                    (unsigned long)_streamIterCount,
                    (unsigned long)(ws.bytesWritten / 1024),
                    (unsigned long)sdKBps,
                    (unsigned long)ws.writeCount,
                    (unsigned long)avgLat,
                    (unsigned long)ws.maxWriteLatency_ms);
        _streamLastLogTime_ms = now;
        _streamLastLogBytes = _uploadBytesWritten;
    }

    // Segment complete?
    if (_streamSegBytesRemaining == 0) {
        uint32_t segElapsed_ms = now - _streamSegStartTime_ms;
        uint32_t segKBps = (segElapsed_ms > 0)
            ? (_streamSegmentSize / segElapsed_ms) : 0;
        STORAGE_LOG("Segment %u/%u done: %lums %luKB/s "
                    "fill=%u%% maxgap=%lums iter=%lu",
                    _streamSegmentIndex, _streamSegmentCount,
                    (unsigned long)segElapsed_ms, (unsigned long)segKBps,
                    _policy.bufferFillPercent(),
                    (unsigned long)_streamMaxGap_ms,
                    (unsigned long)_streamIterCount);

        sendStreamSegmentAck();
        _streamSegmentIndex++;

        // Reset per-segment diagnostics
        _streamSegStartTime_ms = millis();
        _streamMaxGap_ms = 0;
        _streamIterCount = 0;

        if (_uploadBytesWritten >= _uploadExpectedSize) {
            // All data received - exit stream mode, wait for UPLOAD_END (COBS)
            _streamActive = false;
            _streamEndTime_ms = millis();
            uint32_t totalElapsed_ms = _streamEndTime_ms - _streamStartTime_ms;
            uint32_t totalKBps = (totalElapsed_ms > 0)
                ? (_uploadBytesWritten / totalElapsed_ms) : 0;
            STORAGE_LOG("Stream complete: %lu bytes in %u segments, "
                        "%lums (%luKB/s) — waiting for UPLOAD_END",
                        (unsigned long)_uploadBytesWritten,
                        _streamSegmentIndex,
                        (unsigned long)totalElapsed_ms,
                        (unsigned long)totalKBps);
        } else {
            // More segments - compute next segment size
            uint32_t remaining = _uploadExpectedSize - _uploadBytesWritten;
            _streamSegBytesRemaining = (remaining < _streamSegmentSize)
                                    ? remaining : _streamSegmentSize;
        }
    }
}

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::sendStreamSegmentAck() {
    // Wire: [segment_idx:u16LE][bytes_received:u32LE][fill_pct:u8]
    uint8_t payload[7];
    SfxWire::putU16LE(&payload[0], _streamSegmentIndex);
    SfxWire::putU32LE(&payload[2], _uploadBytesWritten);
    payload[6] = _policy.bufferFillPercent();
    sendRawPacket(StoragePacket::FILE_UPLOAD_PROGRESS, SfxWire::TAG_ASYNC,
                  payload, sizeof(payload));
}

// --- Upload Cleanup ---

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::cleanupUpload(bool deletePartial) {
    if (!_uploadActive) return;

    _streamActive = false;

    // Drain any raw bytes remaining in the UART buffer (stream mode leftovers)
    if (serial()) {
        while (serial()->available()) serial()->read();
    }

    _policy.onChunkedCleanup();
    // Discard any buffered data (don't flush on cleanup/cancel)
    _shared.uploadWriteBufLen = 0;
    _policy.freeUploadBuffers();

    _shared.uploadFile.close();

    if (deletePartial) {
        // We already hold the storage lock from handleUploadBegin(),
        // so use policy-level remove directly instead of removeFile() which re-locks.
        if (_uploadTarget == StorageWire::TARGET_FLASH) {
            FlashModule::instance().getFS().remove(_uploadPath);
        } else {
            SdCardModule::instance().policy().removeFile(_uploadPath);
        }
        STORAGE_LOG("Deleted partial upload: %s:%s", targetName(_uploadTarget), _uploadPath);
    }

    unlockStorage(_uploadTarget);
    _uploadActive = false;

    // Resume resources suspended for the duration of the upload
    if (_uploadSuspended) {
        if (_onUploadEnd) _onUploadEnd();
        _uploadSuspended = false;
    }

    // Re-enable STATUS_UPDATE after transfer cleanup
    notifyTransferEnd();
}

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::cancelActiveUpload() {
    if (_uploadActive) {
        STORAGE_LOG("Cancelling active upload on shutdown: %s", _uploadPath);
        cleanupUpload(true);
    }
}



// ============================================================================
// Upload Timeout
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::checkUploadTimeout() {
    if (!_uploadActive) return;

    uint32_t elapsed = millis() - _uploadLastActivity_ms;
    uint32_t timeout = _streamActive ? STREAM_INACTIVITY_MS : UPLOAD_TIMEOUT_MS;
    if (elapsed >= timeout) {
        STORAGE_LOG("Upload inactivity timeout (%lums, limit=%lums) "
                    "cancelling %s:%s (rx=%lu/%lu bytes, stream=%s "
                    "seg=%u/%u fill=%u%%)",
                    (unsigned long)elapsed,
                    (unsigned long)timeout,
                    targetName(_uploadTarget), _uploadPath,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize,
                    _streamActive ? "yes" : "no",
                    _streamSegmentIndex, _streamSegmentCount,
                    _policy.bufferFillPercent());
        cleanupUpload(true);
    }
}


// ============================================================================
// Helpers
// ============================================================================

// --- Transfer lifecycle notification helpers ---

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

template <typename TPolicy>
uint8_t StorageServicePolicy<TPolicy>::mapStorageError(uint8_t err) {
    // FlashError and SdError share identical codes (0-6)
    switch (err) {
        case FlashError::OK:              return SerialError::OK;
        case FlashError::NOT_INITIALIZED: return SerialError::NOT_INITIALIZED;
        case FlashError::NOT_FOUND:       return StorageError::FILE_NOT_FOUND;
        case FlashError::IO_ERROR:        return StorageError::FILE_IO_ERROR;
        case FlashError::IS_DIRECTORY:    return StorageError::FILE_IO_ERROR;
        case FlashError::ALREADY_EXISTS:  return StorageError::FILE_ALREADY_EXISTS;
        case FlashError::LIMIT_EXCEEDED:  return SerialError::UNKNOWN;
        default:                          return SerialError::UNKNOWN;
    }
}


// ============================================================================
// SD Card Init (0x93) -- Remount SD card
// ============================================================================

template <typename TPolicy>
void StorageServicePolicy<TPolicy>::handleSdInit(const uint8_t* payload, size_t len) {
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
// Storage Helpers
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

template <typename TPolicy>
const char* StorageServicePolicy<TPolicy>::targetName(StorageWire::StorageTarget target) {
    return (target == StorageWire::TARGET_FLASH) ? "flash" : "sd";
}

template <typename TPolicy>
uint8_t StorageServicePolicy<TPolicy>::extractPathAndTarget(
        const uint8_t* payload, size_t len,
        char* path, size_t pathBufSize, StorageWire::StorageTarget& target,
        uint8_t* flagsOut) {

    if (flagsOut) *flagsOut = 0;

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
        if (rawTarget > StorageWire::TARGET_FLASH) return SerialError::INVALID_PARAM;
        target = static_cast<StorageWire::StorageTarget>(rawTarget);
    } else {
        target = StorageWire::TARGET_SD;  // default
    }

    // Optional flags byte after the target (append-only extension — Rule 11)
    if (flagsOut && len > (size_t)(2 + pathLen)) {
        *flagsOut = payload[2 + pathLen];
    }

    return SerialError::OK;
}


// ============================================================================
// Path Validation
// ============================================================================

template <typename TPolicy>
bool StorageServicePolicy<TPolicy>::isValidPath(const char* path) {
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

#undef STORAGE_LOG

#endif // STORAGE_SERVICE_IPP
