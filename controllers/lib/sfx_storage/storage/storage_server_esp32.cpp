/*
 * StorageServerEsp32 — ESP32-S3 Dual-Core Storage Implementation
 *
 * Platform-specific overrides for StorageServerBase:
 *
 *   Buffer allocation:
 *     - PSRAM double-buffer (512 KB × 2) for chunked uploads
 *     - PSRAM ring buffer (1 MB) + staging (128 KB) for stream uploads
 *     - Falls back to internal RAM (64 KB × 2) if PSRAM unavailable
 *
 *   Async write offloading:
 *     - Persistent writer task (Core 1): chunked uploads with buffer swap
 *     - Per-upload stream writer task (Core 1): ring buffer → staging → SD
 *
 * See storage_server.cpp for the platform-agnostic protocol handlers.
 */

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32

#include "storage_server.h"
#include <platform/diag_log.h>
#include <esp_heap_caps.h>
#include <algorithm>  // std::swap

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// Buffer Allocation (PSRAM double-buffered)
// ============================================================================

bool StorageServerEsp32::allocateUploadBuffers() {
    if (_uploadWriteBuf) return true;  // Already allocated

    // Try PSRAM first (OPI, 8 MB on N8R8 module)
    _uploadWriteBuf  = (uint8_t*)heap_caps_malloc(UPLOAD_WRITE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    _uploadWriteBuf2 = (uint8_t*)heap_caps_malloc(UPLOAD_WRITE_BUF_SIZE, MALLOC_CAP_SPIRAM);

    if (_uploadWriteBuf && _uploadWriteBuf2) {
        _uploadBufCapacity = UPLOAD_WRITE_BUF_SIZE;
        STORAGE_LOG("Allocated PSRAM upload buffers: %u KB x 2 (free PSRAM: %u KB)",
                    (unsigned)(UPLOAD_WRITE_BUF_SIZE / 1024),
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        // PSRAM alloc failed — fall back to smaller internal RAM buffers
        free(_uploadWriteBuf);
        free(_uploadWriteBuf2);
        _uploadWriteBuf  = (uint8_t*)malloc(UPLOAD_WRITE_BUF_FALLBACK);
        _uploadWriteBuf2 = (uint8_t*)malloc(UPLOAD_WRITE_BUF_FALLBACK);
        if (!_uploadWriteBuf || !_uploadWriteBuf2) {
            freeUploadBuffers();
            STORAGE_LOG("Failed to allocate upload buffers (tried %u KB PSRAM, %u KB internal)",
                        (unsigned)(UPLOAD_WRITE_BUF_SIZE / 1024),
                        (unsigned)(UPLOAD_WRITE_BUF_FALLBACK / 1024));
            return false;
        }
        _uploadBufCapacity = UPLOAD_WRITE_BUF_FALLBACK;
        STORAGE_LOG("PSRAM unavailable — using internal RAM upload buffers: %u KB x 2",
                    (unsigned)(UPLOAD_WRITE_BUF_FALLBACK / 1024));
    }

    _uploadWriteBufLen = 0;
    return true;
}

void StorageServerEsp32::freeUploadBuffers() {
    free(_uploadWriteBuf);
    _uploadWriteBuf = nullptr;
    free(_uploadWriteBuf2);
    _uploadWriteBuf2 = nullptr;
    _uploadWriteBufLen = 0;
    _uploadBufCapacity = 0;
}


// ============================================================================
// Stream Buffer Allocation (Ring Buffer + Staging from PSRAM)
// ============================================================================

bool StorageServerEsp32::allocateStreamBuffers() {
    // Allocate 1 MB ring buffer from PSRAM
    if (!_streamRingBuf.allocate()) {
        STORAGE_LOG("Failed to allocate stream ring buffer (%u KB PSRAM)",
                    (unsigned)(StreamRingBuffer::CAPACITY / 1024));
        return false;
    }

    // Allocate staging buffer for SD writes (128 KB from PSRAM)
    _streamStaging = (uint8_t*)heap_caps_malloc(STREAM_WRITE_CHUNK, MALLOC_CAP_SPIRAM);
    if (!_streamStaging) {
        STORAGE_LOG("Failed to allocate stream staging buffer (%u KB PSRAM)",
                    (unsigned)(STREAM_WRITE_CHUNK / 1024));
        _streamRingBuf.release();
        return false;
    }

    STORAGE_LOG("Stream buffers allocated: ring=%uKB staging=%uKB (free PSRAM: %uKB)",
                (unsigned)(StreamRingBuffer::CAPACITY / 1024),
                (unsigned)(STREAM_WRITE_CHUNK / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

void StorageServerEsp32::freeStreamBuffers() {
    _streamRingBuf.release();
    _streamDrainComplete.store(false, std::memory_order_relaxed);
    if (_streamStaging) {
        free(_streamStaging);
        _streamStaging = nullptr;
    }
    _streamStagingLen = 0;
    _streamReceiving = false;
}


// ============================================================================
// Upload Data Handling (Async Writer Submission)
// ============================================================================

bool StorageServerEsp32::onUploadBufferFull() {
    if (_writerTask) {
        // Async path: swap buffers and submit to writer task
        return submitWriteBuffer();
    }
    // Fallback: blocking write (no writer task started)
    return flushUploadBuffer();
}

bool StorageServerEsp32::checkAsyncWriterHealth() {
    return !_writerError.load(std::memory_order_acquire);
}


// ============================================================================
// Upload Lifecycle Hooks
// ============================================================================

void StorageServerEsp32::onUploadActivated(bool isStream) {
    _writerError.store(false, std::memory_order_relaxed);
    if (isStream) {
        _streamDrainComplete.store(false, std::memory_order_release);
    }
}

bool StorageServerEsp32::onStreamStart() {
    return startStreamWriterTask();
}

bool StorageServerEsp32::onStreamEnd(const char*& errMsg) {
    // Wait for stream writer task to finish draining ring buffer
    if (_streamWriterDone) {
        if (xSemaphoreTake(_streamWriterDone, pdMS_TO_TICKS(30000)) != pdTRUE) {
            STORAGE_LOG("UPLOAD_END stream: writer drain timeout");
            errMsg = "Stream write timeout";
            return false;
        }
    }
    if (_writerError.load(std::memory_order_acquire)) {
        errMsg = "Stream write error";
        return false;
    }
    stopStreamWriterTask();

    STORAGE_LOG("UPLOAD_END stream: writer drained %lu bytes to SD (%lu KB/s peak)",
                (unsigned long)_streamBytesWrittenToSD,
                (unsigned long)_streamSdWriteRate_KBps.load(std::memory_order_relaxed));
    return true;
}

bool StorageServerEsp32::onChunkedEnd(const char*& errMsg) {
    // Wait for any in-flight async write to complete
    if (_writerTask && !waitWriterDone()) {
        errMsg = "Async write failed";
        return false;
    }
    if (_writerError.load(std::memory_order_acquire)) {
        errMsg = "Async write error";
        return false;
    }
    return true;
}

void StorageServerEsp32::onStreamCleanup() {
    stopStreamWriterTask();
}

void StorageServerEsp32::onChunkedCleanup() {
    if (_writerTask) {
        waitWriterDone();
    }
}


// ============================================================================
// Stream Data Processing (Ring Buffer Write)
// ============================================================================

void StorageServerEsp32::onStreamDataReceived(const uint8_t* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        size_t w = _streamRingBuf.write(data + written, len - written);
        written += w;
        if (w == 0) {
            // Ring buffer full — spin-wait for Core 1 to drain
            esp_rom_delay_us(10);
        }
    }
}

void StorageServerEsp32::onStreamReceiveComplete() {
    _streamDrainComplete.store(true, std::memory_order_release);
    STORAGE_LOG("Stream receive complete: %lu bytes in ring buffer, "
                "waiting for writer to drain",
                (unsigned long)(_streamBytesWrittenToSD + _streamRingBuf.used()));
}

size_t StorageServerEsp32::streamBufferCapacityForLog() const {
    return StreamRingBuffer::CAPACITY;
}


// ============================================================================
// Chunked Writer Task (Persistent, Dual-Core Double-Buffered)
// ============================================================================

void StorageServerEsp32::startWriterTask(uint32_t stackSize, UBaseType_t priority) {
    if (_writerTask) return;  // Already started

    _writerDataReady = xSemaphoreCreateBinary();
    _writerDone      = xSemaphoreCreateBinary();
    _writerBufLen.store(0, std::memory_order_relaxed);
    _writerError.store(false, std::memory_order_relaxed);
    _writerActive.store(true, std::memory_order_release);

    // Signal "done" initially so first submitWriteBuffer() doesn't wait
    xSemaphoreGive(_writerDone);

    xTaskCreatePinnedToCore(
        writerTaskFunc,   // Task function
        "StorageWriter",  // Name
        stackSize,        // Stack
        this,             // Parameter (this pointer)
        priority,         // Priority (below audio, above idle)
        &_writerTask,     // Handle
        1                 // Pin to Core 1
    );

    STORAGE_LOG("Writer task started on Core 1 (stack=%lu, prio=%u)",
                (unsigned long)stackSize, (unsigned)priority);
}

void StorageServerEsp32::writerTaskFunc(void* param) {
    StorageServerEsp32* self = static_cast<StorageServerEsp32*>(param);

    while (self->_writerActive.load(std::memory_order_acquire)) {
        // Wait for submit signal from Core 0
        if (xSemaphoreTake(self->_writerDataReady, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;  // Timeout — check _writerActive and loop
        }

        size_t writeLen = self->_writerBufLen.load(std::memory_order_acquire);
        if (writeLen > 0) {
            size_t written = self->_uploadFile.write(self->_uploadWriteBuf2, writeLen);
            if (written != writeLen) {
                STORAGE_LOG("Async write failed: expected %u wrote %u",
                            (unsigned)writeLen, (unsigned)written);
                self->_writerError.store(true, std::memory_order_release);
            }
            self->_writerBufLen.store(0, std::memory_order_release);
        }

        // Signal Core 0 that write is complete (buf2 is free)
        xSemaphoreGive(self->_writerDone);
    }

    // Task exits when _writerActive is cleared
    vTaskDelete(nullptr);
}

bool StorageServerEsp32::submitWriteBuffer() {
    // Wait for previous async write to complete (buf2 must be free)
    if (xSemaphoreTake(_writerDone, pdMS_TO_TICKS(5000)) != pdTRUE) {
        STORAGE_LOG("submitWriteBuffer: timeout waiting for writer");
        return false;
    }

    // Check for error on previous write
    if (_writerError.load(std::memory_order_acquire)) {
        return false;
    }

    // Pointer swap — zero copy, Core 1 writes from what was the fill buffer
    _writerBufLen.store(_uploadWriteBufLen, std::memory_order_release);
    std::swap(_uploadWriteBuf, _uploadWriteBuf2);
    _uploadWriteBufLen = 0;

    // Signal writer task that buf2 has data
    xSemaphoreGive(_writerDataReady);

    return true;
}

bool StorageServerEsp32::waitWriterDone() {
    if (!_writerTask) return true;

    // Wait for the writer to finish current buffer
    if (xSemaphoreTake(_writerDone, pdMS_TO_TICKS(10000)) != pdTRUE) {
        STORAGE_LOG("waitWriterDone: timeout");
        return false;
    }

    // Re-give the semaphore so it's in the "done" state
    xSemaphoreGive(_writerDone);

    return !_writerError.load(std::memory_order_acquire);
}


// ============================================================================
// Stream Writer Task (Per-Upload, Ring Buffer Drainer)
// ============================================================================

void StorageServerEsp32::streamWriterTaskFunc(void* param) {
    StorageServerEsp32* self = static_cast<StorageServerEsp32*>(param);

    STORAGE_LOG("Stream writer task started on Core %d", xPortGetCoreID());

    while (self->_streamWriterActive.load(std::memory_order_acquire)) {
        size_t avail = self->_streamRingBuf.used();
        bool drainComplete = self->_streamDrainComplete.load(std::memory_order_acquire);

        // Write when we have a full chunk OR when drain is signaled and any data remains
        if (avail >= self->STREAM_WRITE_CHUNK || (drainComplete && avail > 0)) {
            size_t toRead = (avail < self->STREAM_WRITE_CHUNK) ? avail : self->STREAM_WRITE_CHUNK;
            self->_streamRingBuf.read(self->_streamStaging, toRead);

            // Measure SD write time for DiagLog
            uint32_t t0 = millis();
            size_t written = self->_uploadFile.write(self->_streamStaging, toRead);
            uint32_t dt = millis() - t0;

            if (written != toRead) {
                STORAGE_LOG("Stream write failed: expected %u wrote %u",
                            (unsigned)toRead, (unsigned)written);
                self->_writerError.store(true, std::memory_order_release);
                // Signal done so handleUploadEnd doesn't hang
                xSemaphoreGive(self->_streamWriterDone);
                break;  // Exit task loop
            }

            self->_streamBytesWrittenToSD += written;

            // Compute and store SD write rate (KB/s)
            if (dt > 0) {
                uint32_t rate = (uint32_t)((uint64_t)written * 1000 / (dt * 1024));
                self->_streamSdWriteRate_KBps.store(rate, std::memory_order_relaxed);
                STORAGE_LOG("SD_WRITE: %uKB in %lums (%lu KB/s) ring=%uKB",
                            (unsigned)(toRead / 1024), (unsigned long)dt,
                            (unsigned long)rate,
                            (unsigned)(self->_streamRingBuf.used() / 1024));
            }

            // If drain is complete and ring is now empty, signal done
            if (drainComplete && self->_streamRingBuf.used() == 0) {
                STORAGE_LOG("Stream writer: drain complete (%lu bytes total)",
                            (unsigned long)self->_streamBytesWrittenToSD);
                xSemaphoreGive(self->_streamWriterDone);
                break;  // Done — exit task loop
            }
        } else if (drainComplete && avail == 0) {
            // Drain requested but ring is already empty — signal done
            STORAGE_LOG("Stream writer: drain complete (0 bytes remaining)");
            xSemaphoreGive(self->_streamWriterDone);
            break;  // Done — exit task loop
        } else {
            // No data to write — brief yield to avoid busy-spinning
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    STORAGE_LOG("Stream writer task exiting");
    // Don't self-delete — stopStreamWriterTask() handles task deletion.
    // Suspend self until deleted by Core 0.
    vTaskSuspend(nullptr);
}

bool StorageServerEsp32::startStreamWriterTask() {
    if (_streamWriterTask) {
        STORAGE_LOG("Stream writer task already running");
        return false;
    }

    _streamWriterDone = xSemaphoreCreateBinary();
    if (!_streamWriterDone) {
        STORAGE_LOG("Failed to create stream writer semaphore");
        return false;
    }

    _writerError.store(false, std::memory_order_relaxed);
    _streamWriterActive.store(true, std::memory_order_release);

    BaseType_t ret = xTaskCreatePinnedToCore(
        streamWriterTaskFunc,  // Task function
        "StreamWriter",       // Name
        8192,                  // Stack (8 KB — only needs staging ptr + locals)
        this,                  // Parameter
        2,                     // Priority (below audio, above idle)
        &_streamWriterTask,    // Handle
        1                      // Pin to Core 1
    );

    if (ret != pdPASS) {
        STORAGE_LOG("Failed to create stream writer task");
        vSemaphoreDelete(_streamWriterDone);
        _streamWriterDone = nullptr;
        _streamWriterActive.store(false, std::memory_order_relaxed);
        return false;
    }

    STORAGE_LOG("Stream writer task created on Core 1");
    return true;
}

void StorageServerEsp32::stopStreamWriterTask() {
    if (!_streamWriterTask) return;

    // Signal the task to exit
    _streamWriterActive.store(false, std::memory_order_release);
    // Also set drain complete so the task wakes up and sees the exit flag
    _streamDrainComplete.store(true, std::memory_order_release);

    // Give the task time to finish its current SD write and suspend
    vTaskDelay(pdMS_TO_TICKS(200));

    // Delete the task (it suspends itself after exiting the loop)
    vTaskDelete(_streamWriterTask);
    _streamWriterTask = nullptr;

    if (_streamWriterDone) {
        vSemaphoreDelete(_streamWriterDone);
        _streamWriterDone = nullptr;
    }

    STORAGE_LOG("Stream writer task stopped");
}

#endif  // SFX_PLATFORM_ESP32
