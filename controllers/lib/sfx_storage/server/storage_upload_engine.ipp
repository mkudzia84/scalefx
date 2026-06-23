/*
 * UploadEngine<TPolicy> implementation.
 *
 * Bodies moved VERBATIM from the former StorageServicePolicy upload handlers.
 * The only mechanical changes: the platform policy + shared buffers are now
 * reached through the owning policy (`_svc._policy` / `_svc._shared`); the
 * stateless path helpers are the free `sfx_storage::*` functions; every wire /
 * lock / transfer-notify call goes through the thin wrappers below so the hot
 * paths stay textually unchanged.  All upload LOGIC (sequence/CRC checks,
 * segment math, MD5, flush ordering, self-heal) is unchanged.
 *
 * #included from storage_service.h AFTER StorageServicePolicy is complete.
 */

#ifndef SFX_STORAGE_UPLOAD_ENGINE_IPP
#define SFX_STORAGE_UPLOAD_ENGINE_IPP

#include <serial/diag_log.h>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include "storage_path_util.h"       // sfx_storage::targetName / mapStorageError / isValidPath

// ── Thin wrappers onto the owning policy's shared surface ───────────────

template <typename TPolicy>
int UploadEngine<TPolicy>::sendAck() { return _svc.sendAck(); }

template <typename TPolicy>
int UploadEngine<TPolicy>::sendNack(uint8_t errorCode, const char* reason) {
    return _svc.sendNack(errorCode, reason);
}

template <typename TPolicy>
int UploadEngine<TPolicy>::sendRawPacket(uint8_t t, uint8_t tag, const uint8_t* p, size_t l) {
    return _svc.sendRawPacket(t, tag, p, l);
}

template <typename TPolicy>
uint8_t UploadEngine<TPolicy>::currentTag() const { return _svc.currentTag(); }

template <typename TPolicy>
sfx::Stream* UploadEngine<TPolicy>::serial() const { return _svc.serial(); }

template <typename TPolicy>
bool UploadEngine<TPolicy>::checkStorageReady(StorageWire::StorageTarget target) {
    return _svc.checkStorageReady(target);
}

template <typename TPolicy>
void UploadEngine<TPolicy>::lockStorage(StorageWire::StorageTarget target) {
    _svc.lockStorage(target);
}

template <typename TPolicy>
void UploadEngine<TPolicy>::unlockStorage(StorageWire::StorageTarget target) {
    _svc.unlockStorage(target);
}

template <typename TPolicy>
void UploadEngine<TPolicy>::notifyTransferStart() { _svc.notifyTransferStart(); }

template <typename TPolicy>
void UploadEngine<TPolicy>::notifyTransferEnd() { _svc.notifyTransferEnd(); }


// ============================================================================
// File Upload Begin (0xA0)
// ============================================================================

template <typename TPolicy>
void UploadEngine<TPolicy>::handleUploadBegin(const uint8_t* payload, size_t len) {
    // Wire: [size:u32LE][pathLen:u8][path:str][target:u8?]
    if (len < 6) {  // 4 (size) + 1 (pathLen) + 1 (min path char)
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Stale-upload recovery (hardening, 2026-05-31): the wire is single-master,
    // so a fresh UPLOAD_BEGIN while one is already active means the previous
    // client abandoned mid-transfer (timed-out chunk, dropped ACK, Studio crash)
    // and has reconnected to retry.  Rejecting with UPLOAD_IN_PROGRESS used to
    // wedge that retry until the inactivity timeout fired (formerly 30 s) — the
    // device looked dead.  Instead, forcibly clean up the stale transfer (closes
    // the file, frees buffers, unlocks storage, fires onUploadEnd) and honour the
    // new BEGIN immediately.  cleanupUpload() is self-contained and resets all
    // upload state to idle, so the open/lock below starts from a clean slate.
    if (_uploadActive.load(std::memory_order_relaxed)) {
        STORAGE_LOG("UPLOAD_BEGIN while active — recovering stale upload "
                    "%s:%s (rx=%lu/%lu, idle=%lums)",
                    sfx_storage::targetName(_uploadTarget), _uploadPath,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize,
                    (unsigned long)(SFX_MILLIS() - _uploadLastActivity_ms));
        captureUploadDiag(StorageWire::REASON_STALE_RESET);
        cleanupUpload(true);
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
    if (!sfx_storage::isValidPath(_uploadPath)) {
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
        err = FlashModule::instance().openWrite(_uploadPath, _svc._shared.uploadFile, true);
    else
        err = _svc._policy.sdOpenWrite(_uploadPath, _svc._shared.uploadFile, true);

    if (err != 0) {
        unlockStorage(_uploadTarget);
        sendNack(sfx_storage::mapStorageError(err));
        return;
    }

    // Allocate upload write buffers (platform-specific: PSRAM on ESP32, heap on Pico)
    if (!_svc._policy.allocateUploadBuffers()) {
        _svc._shared.uploadFile.close();
        unlockStorage(_uploadTarget);
        sendNack(StorageError::FILE_IO_ERROR, "Buffer alloc failed");
        return;
    }

    _uploadActive.store(true, std::memory_order_release);
    _uploadExpectedSize = fileSize;
    _uploadBytesWritten = 0;
    _uploadExpectedSeq  = 0;
    _svc._shared.uploadWriteBufLen  = 0;
    _uploadLastActivity_ms = SFX_MILLIS();
    _uploadMd5.begin();

    // Stream mode state
    _streamActive.store(false, std::memory_order_release);
    _streamSegmentIndex    = 0;
    _streamSegBytesRemaining = 0;
    _streamSegmentSize     = 0;
    _streamSegmentCount    = 0;

    // Platform hook: reset per-upload counters, etc.
    _svc._policy.onUploadActivated();

    const char* modeStr = (_uploadMode == StorageWire::UPLOAD_STREAM) ? "stream" : "sync";

    STORAGE_LOG("UPLOAD_BEGIN %s:%s size=%lu mode=%s buf=%uKB",
                sfx_storage::targetName(_uploadTarget), _uploadPath,
                (unsigned long)fileSize, modeStr,
                (unsigned)(_svc._shared.uploadBufCapacity / 1024));

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
        // Pre-allocate file on SD for contiguous cluster allocation.  A failed
        // pre-allocation is a genuine card-I/O fault, not just a lost
        // optimization — surface it now rather than letting the upload limp on
        // to a confusing failure at the first real segment flush.
        if (_uploadTarget == StorageWire::TARGET_SD && fileSize > 0) {
            uint8_t zero = 0;
            bool preallocOk = _svc._shared.uploadFile.seek(fileSize - 1)
                           && (_svc._shared.uploadFile.write(&zero, 1) == 1)
                           && _svc._shared.uploadFile.seek(0);
            if (!preallocOk) {
                STORAGE_LOG("UPLOAD_BEGIN: SD pre-allocation failed for %s (size=%lu)",
                            _uploadPath, (unsigned long)fileSize);
                cleanupUpload(true);
                sendNack(StorageError::FILE_IO_ERROR, "Pre-allocation failed");
                return;
            }
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
        // A zero-byte file has no segments to stream (segCount==0) — never enter
        // raw-stream mode, or processStream() would spin on `avail<=0` and the
        // upload would only terminate via the 5 s inactivity self-heal (which
        // deletes the partial file and fails the client's UPLOAD_END).  Leaving
        // _streamActive false keeps the wire in COBS mode so the immediately
        // following UPLOAD_END is processed and completes the empty file.
        _streamActive.store(fileSize > 0, std::memory_order_release);

        // Initialize stream diagnostics
        _streamStartTime_ms    = SFX_MILLIS();
        _streamSegStartTime_ms = SFX_MILLIS();
        _streamEndTime_ms      = 0;
        _streamLastLogTime_ms  = SFX_MILLIS();
        _streamLastLogBytes    = 0;
        _streamLastCallTime_ms = SFX_MILLIS();
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
void UploadEngine<TPolicy>::handleUploadData(const uint8_t* payload, size_t len) {
    // Wire: [seqNum:u16LE][crc16:u16LE][data:N]
    if (!_uploadActive.load(std::memory_order_relaxed)) {
        sendNack(StorageError::NO_UPLOAD_ACTIVE);
        return;
    }

    if (len < 5) {  // 2 (seq) + 2 (crc) + 1 (min data)
        sendNack(SerialError::MISSING_PARAMETER);
        return;
    }

    // Policy health check — a no-op for single-core policies, reserved
    // for a future async writer to signal a deferred error.
    if (!_svc._policy.checkAsyncWriterHealth()) {
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
    _uploadLastActivity_ms = SFX_MILLIS();

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
        size_t space = _svc._shared.uploadBufCapacity - _svc._shared.uploadWriteBufLen;
        size_t toCopy = (remaining < space) ? remaining : space;
        memcpy(&_svc._shared.uploadWriteBuf[_svc._shared.uploadWriteBufLen], src, toCopy);
        _svc._shared.uploadWriteBufLen += toCopy;
        src += toCopy;
        remaining -= toCopy;

        if (_svc._shared.uploadWriteBufLen >= _svc._shared.uploadBufCapacity) {
            if (!_svc._policy.onUploadBufferFull()) {
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
void UploadEngine<TPolicy>::handleUploadEnd() {
    if (!_uploadActive.load(std::memory_order_relaxed)) {
        sendNack(StorageError::NO_UPLOAD_ACTIVE);
        return;
    }

    uint32_t tEndReceived = SFX_MILLIS();

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
    uint32_t t0 = SFX_MILLIS();
    const char* errMsg = nullptr;
    if (!_svc._policy.onChunkedEnd(errMsg)) {
        STORAGE_LOG("UPLOAD_END drain failed after %lums: %s",
                    (unsigned long)(SFX_MILLIS() - t0),
                    errMsg ? errMsg : "unknown");
        cleanupUpload(true);
        sendNack(StorageError::FILE_IO_ERROR, errMsg ? errMsg : "Async write failed");
        return;
    }
    uint32_t t1 = SFX_MILLIS();

    // Flush remaining write buffer to file (blocking — final partial block)
    if (_svc._shared.uploadWriteBufLen > 0) {
        if (!_svc._shared.flushUploadBuffer()) {
            cleanupUpload(true);
            sendNack(StorageError::FILE_IO_ERROR, "Final flush failed");
            return;
        }
    }
    uint32_t t2 = SFX_MILLIS();

    // Force data to storage media before closing
    _svc._shared.uploadFile.flush();
    _svc._shared.uploadFile.close();
    unlockStorage(_uploadTarget);
    uint32_t t3 = SFX_MILLIS();

    // Compute final MD5 digest
    _uploadMd5.calculate();
    uint8_t md5Bytes[16];
    _uploadMd5.getBytes(md5Bytes);
    uint32_t t4 = SFX_MILLIS();

    const char* modeStr = (_uploadMode == StorageWire::UPLOAD_STREAM) ? "stream" : "sync";

    auto ws = _svc._policy.writerStats();
    uint32_t avgLat = (ws.writeCount > 0)
        ? (ws.totalStallTime_ms / ws.writeCount) : 0;
    STORAGE_LOG("UPLOAD_END %s:%s %lu bytes OK (md5=%s mode=%s) "
                "finalize=%lums flush=%lums close=%lums md5=%lums total=%lums",
                sfx_storage::targetName(_uploadTarget), _uploadPath,
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

    _uploadActive.store(false, std::memory_order_release);
    _streamActive.store(false, std::memory_order_release);
    // Snapshot the healthy-completion stats too, so a DIAG query right after a
    // successful upload still shows the SD write profile (max latency, etc.)
    // — captured before freeUploadBuffers()/the next BEGIN clears them.
    captureUploadDiag(StorageWire::REASON_COMPLETED);
    _svc._policy.freeUploadBuffers();

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
void UploadEngine<TPolicy>::handleUploadCancel() {
    if (!_uploadActive.load(std::memory_order_relaxed)) {
        sendNack(StorageError::NO_UPLOAD_ACTIVE);
        return;
    }

    STORAGE_LOG("UPLOAD_CANCEL %s:%s (received %lu/%lu bytes)",
                sfx_storage::targetName(_uploadTarget), _uploadPath,
                (unsigned long)_uploadBytesWritten,
                (unsigned long)_uploadExpectedSize);

    captureUploadDiag(StorageWire::REASON_CLIENT_CANCEL);
    cleanupUpload(true);
    sendAck();
}


// ============================================================================
// Stream Upload pump (batch mode)
// ============================================================================

template <typename TPolicy>
void UploadEngine<TPolicy>::processStream() {
    if (!_streamActive.load(std::memory_order_relaxed) ||
        !_uploadActive.load(std::memory_order_relaxed)) return;

    // Track loop gap — detect slow main loop iterations
    uint32_t now = SFX_MILLIS();
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
    if (!_svc._policy.checkAsyncWriterHealth()) {
        STORAGE_LOG("Stream: policy signalled error - aborting "
                    "(seg=%u/%u rx=%lu/%lu)",
                    _streamSegmentIndex, _streamSegmentCount,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize);
        _streamActive.store(false, std::memory_order_release);
        captureUploadDiag(StorageWire::REASON_HEALTH);
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
    size_t space = _svc._shared.uploadBufCapacity - _svc._shared.uploadWriteBufLen;
    size_t toRead = (size_t)avail;
    if (toRead > _streamSegBytesRemaining) toRead = _streamSegBytesRemaining;
    if (toRead > space)                    toRead = space;

    size_t got = serial()->readBytes(
        &_svc._shared.uploadWriteBuf[_svc._shared.uploadWriteBufLen], toRead);
    if (got == 0) return;

    // Feed running MD5 hash (fast, stays on Core 0)
    _uploadMd5.add(&_svc._shared.uploadWriteBuf[_svc._shared.uploadWriteBufLen], got);
    _uploadBytesWritten      += got;
    _streamSegBytesRemaining -= got;
    _svc._shared.uploadWriteBufLen += got;

    // Flush when buffer full, or when the segment boundary has been reached
    // (policies need the in-flight bytes on disk before we ACK).
    bool needFlush = (_svc._shared.uploadWriteBufLen >= _svc._shared.uploadBufCapacity)
                  || (_streamSegBytesRemaining == 0 && _svc._shared.uploadWriteBufLen > 0);

    if (needFlush && !_svc._policy.onUploadBufferFull()) {
        STORAGE_LOG("Stream: buffer flush failed - aborting "
                    "(seg=%u/%u rx=%lu/%lu fill=%u%%)",
                    _streamSegmentIndex, _streamSegmentCount,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize,
                    _svc._policy.bufferFillPercent());
        _streamActive.store(false, std::memory_order_release);
        captureUploadDiag(StorageWire::REASON_FLUSH_FAIL);
        while (serial()->available()) serial()->read();
        cleanupUpload(true);
        return;
    }

    // Hardening (2026-05-31): the inactivity timer measures *the client going
    // silent*, NOT how long our own SD/flash flush took.  onUploadBufferFull()
    // above is a SYNCHRONOUS write that can stall multiple seconds when the SD
    // card does internal GC / wear-levelling (typically tens of MB into a large
    // file).  Because _uploadLastActivity_ms was stamped at the TOP of this call
    // (before the read+flush), a flush longer than STREAM_INACTIVITY_MS would
    // make the very next checkUploadTimeout() abort our OWN healthy upload — the
    // client, still streaming, then waits out its 15 s segment-ACK deadline and
    // reports "stream timeout" (observed: 86 MB upload dying at a random ~30 MB).
    // Re-stamp AFTER the flush so the timer only counts genuine client silence.
    if (needFlush) _uploadLastActivity_ms = SFX_MILLIS();

    // Periodic progress logging (every ~2 seconds)
    if (now - _streamLastLogTime_ms >= 2000) {
        uint32_t bytesInPeriod = _uploadBytesWritten - _streamLastLogBytes;
        uint32_t elapsed_ms = now - _streamLastLogTime_ms;
        uint32_t kbps = (elapsed_ms > 0) ? (bytesInPeriod / elapsed_ms) : 0;
        auto ws = _svc._policy.writerStats();
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
                    _svc._policy.bufferFillPercent(),
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
                    _svc._policy.bufferFillPercent(),
                    (unsigned long)_streamMaxGap_ms,
                    (unsigned long)_streamIterCount);

        sendStreamSegmentAck();
        _streamSegmentIndex++;

        // Reset per-segment diagnostics
        _streamSegStartTime_ms = SFX_MILLIS();
        _streamMaxGap_ms = 0;
        _streamIterCount = 0;

        if (_uploadBytesWritten >= _uploadExpectedSize) {
            // All data received - exit stream mode, wait for UPLOAD_END (COBS)
            _streamActive.store(false, std::memory_order_release);
            _streamEndTime_ms = SFX_MILLIS();
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
void UploadEngine<TPolicy>::sendStreamSegmentAck() {
    // Wire: [segment_idx:u16LE][bytes_received:u32LE][fill_pct:u8]
    uint8_t payload[7];
    SfxWire::putU16LE(&payload[0], _streamSegmentIndex);
    SfxWire::putU32LE(&payload[2], _uploadBytesWritten);
    payload[6] = _svc._policy.bufferFillPercent();
    sendRawPacket(StoragePacket::FILE_UPLOAD_PROGRESS, SfxWire::TAG_ASYNC,
                  payload, sizeof(payload));
}


// ============================================================================
// Upload Cleanup
// ============================================================================

template <typename TPolicy>
void UploadEngine<TPolicy>::cleanupUpload(bool deletePartial) {
    if (!_uploadActive.load(std::memory_order_relaxed)) return;

    _streamActive.store(false, std::memory_order_release);

    // Drain any raw bytes remaining in the UART buffer (stream mode leftovers)
    if (serial()) {
        while (serial()->available()) serial()->read();
    }

    _svc._policy.onChunkedCleanup();
    // Discard any buffered data (don't flush on cleanup/cancel)
    _svc._shared.uploadWriteBufLen = 0;
    _svc._policy.freeUploadBuffers();

    _svc._shared.uploadFile.close();

    if (deletePartial) {
        // We already hold the storage lock from handleUploadBegin(),
        // so use policy-level remove directly instead of removeFile() which re-locks.
        if (_uploadTarget == StorageWire::TARGET_FLASH) {
            FlashModule::instance().removeFileNoLock(_uploadPath);
        } else {
            SdCardModule::instance().policy().removeFile(_uploadPath);
        }
        STORAGE_LOG("Deleted partial upload: %s:%s",
                    sfx_storage::targetName(_uploadTarget), _uploadPath);
    }

    unlockStorage(_uploadTarget);
    _uploadActive.store(false, std::memory_order_release);

    // Resume resources suspended for the duration of the upload
    if (_uploadSuspended) {
        if (_onUploadEnd) _onUploadEnd();
        _uploadSuspended = false;
    }

    // Re-enable STATUS_UPDATE after transfer cleanup
    notifyTransferEnd();
}


// ============================================================================
// Upload Diagnostics (FILE_UPLOAD_DIAG)
// ============================================================================

template <typename TPolicy>
void UploadEngine<TPolicy>::captureUploadDiag(uint8_t reason) {
    // Snapshot stream progress + the policy's SD writer stats BEFORE
    // cleanupUpload() runs (the next BEGIN's onUploadActivated() zeroes them).
    // This is the only way a client that timed out waiting for a segment ACK
    // can see the SD write latency that caused it — the stream phase can't emit
    // COBS log packets over the wire.
    auto ws = _svc._policy.writerStats();
    _diag.bytesRecv       = _uploadBytesWritten;
    _diag.expectedSize    = _uploadExpectedSize;
    _diag.segIndex        = _streamSegmentIndex;
    _diag.segCount        = _streamSegmentCount;
    _diag.fillPct         = _svc._policy.bufferFillPercent();
    _diag.sdWriteCount    = ws.writeCount;
    _diag.sdBytesWritten  = ws.bytesWritten;
    _diag.sdMaxLat_ms     = ws.maxWriteLatency_ms;
    _diag.sdTotalStall_ms = ws.totalStallTime_ms;
    _diag.maxLoopGap_ms   = _streamMaxGap_ms;
    _diag.flags           = (_uploadActive.load(std::memory_order_relaxed) ? 0x01 : 0x00)
                          | (_streamActive.load(std::memory_order_relaxed) ? 0x02 : 0x00);
    _diag.reason          = reason;
}

template <typename TPolicy>
void UploadEngine<TPolicy>::handleUploadDiagReq() {
    // If an upload is in progress (shouldn't normally be — the wire is in raw
    // mode then, so this only reaches us between/around transfers), refresh the
    // live numbers first so the snapshot is current.
    if (_uploadActive.load(std::memory_order_relaxed)) {
        captureUploadDiag(StorageWire::REASON_ACTIVE);
    }

    uint8_t buf[35];
    SfxWire::putU32LE(&buf[0],  _diag.bytesRecv);
    SfxWire::putU32LE(&buf[4],  _diag.expectedSize);
    SfxWire::putU16LE(&buf[8],  _diag.segIndex);
    SfxWire::putU16LE(&buf[10], _diag.segCount);
    buf[12] = _diag.fillPct;
    SfxWire::putU32LE(&buf[13], _diag.sdWriteCount);
    SfxWire::putU32LE(&buf[17], _diag.sdBytesWritten);
    SfxWire::putU32LE(&buf[21], _diag.sdMaxLat_ms);
    SfxWire::putU32LE(&buf[25], _diag.sdTotalStall_ms);
    SfxWire::putU32LE(&buf[29], _diag.maxLoopGap_ms);
    buf[33] = _diag.flags;
    buf[34] = _diag.reason;

    sendRawPacket(StoragePacket::FILE_UPLOAD_DIAG_RESP, currentTag(), buf, sizeof(buf));
}

template <typename TPolicy>
void UploadEngine<TPolicy>::cancelActiveUpload() {
    if (_uploadActive.load(std::memory_order_relaxed)) {
        STORAGE_LOG("Cancelling active upload on shutdown: %s", _uploadPath);
        cleanupUpload(true);
    }
}


// ============================================================================
// Upload Timeout
// ============================================================================

template <typename TPolicy>
void UploadEngine<TPolicy>::checkUploadTimeout() {
    if (!_uploadActive.load(std::memory_order_relaxed)) return;

    uint32_t elapsed = SFX_MILLIS() - _uploadLastActivity_ms;
    uint32_t timeout = _streamActive.load(std::memory_order_relaxed) ? STREAM_INACTIVITY_MS : UPLOAD_TIMEOUT_MS;
    if (elapsed >= timeout) {
        STORAGE_LOG("Upload inactivity timeout (%lums, limit=%lums) "
                    "cancelling %s:%s (rx=%lu/%lu bytes, stream=%s "
                    "seg=%u/%u fill=%u%%)",
                    (unsigned long)elapsed,
                    (unsigned long)timeout,
                    sfx_storage::targetName(_uploadTarget), _uploadPath,
                    (unsigned long)_uploadBytesWritten,
                    (unsigned long)_uploadExpectedSize,
                    _streamActive.load(std::memory_order_relaxed) ? "yes" : "no",
                    _streamSegmentIndex, _streamSegmentCount,
                    _svc._policy.bufferFillPercent());
        captureUploadDiag(StorageWire::REASON_INACTIVITY);
        cleanupUpload(true);
    }
}

#endif  // SFX_STORAGE_UPLOAD_ENGINE_IPP
