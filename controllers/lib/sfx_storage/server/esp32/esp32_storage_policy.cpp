/*
 * Esp32StoragePolicy — ESP32-S3 Dual-Core Storage Implementation
 *
 * Platform-specific methods for StorageServerT<Esp32StoragePolicy>:
 *
 *   Buffer allocation (on-demand, per upload):
 *     - 64 KB PSRAM fill buffer (per-upload, feeds the ring buffer)
 *     - 2 MB SPSC ring buffer + 256 KB staging (allocated on first upload, freed after)
 *     - Falls back to internal RAM if PSRAM unavailable
 *
 *   Async write offloading via SPSC ring buffer:
 *     - Core 0: UPLOAD_DATA chunks → fill buffer → writeBulk to 2 MB ring
 *     - Core 1: Persistent writer task drains ring → 256 KB staging → SD card
 *     - Core 0 never blocks on SD I/O (only on ring-full, which is rare)
 *
 * See storage_server.ipp for the platform-agnostic protocol handlers.
 */

#include <platform/sfx_platform.h>

#if SFX_PLATFORM_ESP32 && defined(SFX_HAS_STORAGE_SERVER)

#include <server/storage_server.h>
#include <platform/diag_log.h>
#include <esp_heap_caps.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// Buffer Allocation (64 KB fill buffer only — ring is persistent)
// ============================================================================

bool Esp32StoragePolicy::allocateUploadBuffers() {
    if (_state->uploadWriteBuf) return true;  // Already allocated

    // Try PSRAM first for the fill buffer
    _state->uploadWriteBuf = (uint8_t*)heap_caps_malloc(UPLOAD_FILL_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (_state->uploadWriteBuf) {
        _state->uploadBufCapacity = UPLOAD_FILL_BUF_SIZE;
        STORAGE_LOG("Allocated PSRAM fill buffer: %u KB (free PSRAM: %u KB)",
                    (unsigned)(UPLOAD_FILL_BUF_SIZE / 1024),
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else {
        // PSRAM failed — fall back to internal RAM
        _state->uploadWriteBuf = (uint8_t*)malloc(UPLOAD_FILL_BUF_FALLBACK);
        if (!_state->uploadWriteBuf) {
            STORAGE_LOG("Failed to allocate fill buffer (%u KB PSRAM, %u KB internal)",
                        (unsigned)(UPLOAD_FILL_BUF_SIZE / 1024),
                        (unsigned)(UPLOAD_FILL_BUF_FALLBACK / 1024));
            return false;
        }
        _state->uploadBufCapacity = UPLOAD_FILL_BUF_FALLBACK;
        STORAGE_LOG("PSRAM unavailable — using internal RAM fill buffer: %u KB",
                    (unsigned)(UPLOAD_FILL_BUF_FALLBACK / 1024));
    }

    _state->uploadWriteBufLen = 0;
    return true;
}

void Esp32StoragePolicy::freeUploadBuffers() {
    free(_state->uploadWriteBuf);
    _state->uploadWriteBuf = nullptr;
    _state->uploadWriteBufLen = 0;
    _state->uploadBufCapacity = 0;
}


// ============================================================================
// Upload Data Handling (Ring Buffer Write)
// ============================================================================

bool Esp32StoragePolicy::onUploadBufferFull() {
    if (_writerTask && _ring.isAllocated()) {
        // Async path: write fill buffer contents into ring (non-blocking)
        size_t remaining = _state->uploadWriteBufLen;
        size_t offset = 0;
        uint32_t stallStart = 0;
        bool stalled = false;

        while (remaining > 0) {
            uint32_t written = _ring.writeBulk(
                _state->uploadWriteBuf + offset, remaining);
            offset    += written;
            remaining -= written;

            if (remaining > 0) {
                if (!stalled) {
                    stallStart = millis();
                    stalled = true;
                }
                // Ring full — yield briefly for writer task to drain
                vTaskDelay(pdMS_TO_TICKS(1));
                if (_writerError.load(std::memory_order_acquire)) {
                    STORAGE_LOG("Ring write aborted: writer error");
                    return false;
                }
            }
        }

        // Log significant stalls (>10ms means ring buffer was under pressure)
        if (stalled) {
            uint32_t stallDuration = millis() - stallStart;
            if (stallDuration >= 10) {
                uint32_t ringAvail = _ring.availableRead();
                uint8_t ringPct = (uint8_t)((uint64_t)ringAvail * 100 / UPLOAD_RING_CAPACITY);
                STORAGE_LOG("Ring stall: %lums (ring=%u%% avail=%luKB)",
                            (unsigned long)stallDuration,
                            ringPct,
                            (unsigned long)(ringAvail / 1024));
            }
        }

        _state->uploadWriteBufLen = 0;
        return true;
    }

    // Fallback: blocking write (no writer task or ring not available)
    return _state->flushUploadBuffer();
}

bool Esp32StoragePolicy::checkAsyncWriterHealth() {
    return !_writerError.load(std::memory_order_acquire);
}


// ============================================================================
// Upload Lifecycle Hooks
// ============================================================================

void Esp32StoragePolicy::onUploadActivated() {
    // Lazy-allocate ring + staging on first upload (freed after upload completes)
    allocateWriterBuffers();

    _writerError.store(false, std::memory_order_relaxed);
    _drainRequested.store(false, std::memory_order_relaxed);

    // Reset writer performance counters for new upload
    _writerBytesWritten.store(0, std::memory_order_relaxed);
    _writerWriteCount.store(0, std::memory_order_relaxed);
    _writerMaxLatency_ms.store(0, std::memory_order_relaxed);
    _writerTotalStall_ms.store(0, std::memory_order_relaxed);

    // Reset ring buffer for new upload
    if (_ring.isAllocated()) {
        _ring.reset();
    }

    // Clear any stale drain signal from previous upload
    if (_drainComplete) {
        xSemaphoreTake(_drainComplete, 0);
    }

    // Start writer task on-demand for this upload
    startWriterTask();
}

bool Esp32StoragePolicy::onChunkedEnd(const char*& errMsg) {
    if (!_writerTask || !_ring.isAllocated()) {
        // No async writer — nothing to drain
        return true;
    }

    // Flush any remaining data in the fill buffer to the ring
    if (_state->uploadWriteBufLen > 0) {
        size_t remaining = _state->uploadWriteBufLen;
        size_t offset = 0;

        while (remaining > 0) {
            uint32_t written = _ring.writeBulk(
                _state->uploadWriteBuf + offset, remaining);
            offset    += written;
            remaining -= written;

            if (remaining > 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                if (_writerError.load(std::memory_order_acquire)) {
                    errMsg = "Writer error during final flush to ring";
                    stopWriterTask();
                    return false;
                }
            }
        }
        _state->uploadWriteBufLen = 0;
    }

    // Signal writer to drain everything remaining in the ring
    _drainRequested.store(true, std::memory_order_release);

    // Wait for writer to finish draining (generous timeout for large files)
    if (xSemaphoreTake(_drainComplete, pdMS_TO_TICKS(30000)) != pdTRUE) {
        errMsg = "Drain timeout waiting for writer";
        STORAGE_LOG("onChunkedEnd: drain timeout (ring avail=%u)",
                    (unsigned)_ring.availableRead());
        stopWriterTask();
        return false;
    }

    _drainRequested.store(false, std::memory_order_relaxed);

    if (_writerError.load(std::memory_order_acquire)) {
        errMsg = "Writer error during drain";
        stopWriterTask();
        return false;
    }

    // Drain successful — stop writer task and free buffers (upload ending)
    stopWriterTask();
    freeWriterBuffers();
    return true;
}

void Esp32StoragePolicy::onChunkedCleanup() {
    // Drain and stop writer if still running
    if (_writerTask && _ring.isAllocated()) {
        // Signal drain so writer finishes its current SD write batch
        _drainRequested.store(true, std::memory_order_release);

        // Wait for writer to complete current batch (brief timeout — data will be discarded)
        if (_drainComplete) {
            xSemaphoreTake(_drainComplete, pdMS_TO_TICKS(5000));
        }

        _drainRequested.store(false, std::memory_order_relaxed);

        // Discard any remaining ring data (upload is being cancelled)
        _ring.reset();

        // Stop writer task (upload cancelled/cleaned up)
        stopWriterTask();
    }

    // Free writer buffers (ring + staging + semaphore) — always attempt,
    // even if writer was already stopped by an error path in onChunkedEnd()
    freeWriterBuffers();
}


// ============================================================================
// Buffer Diagnostics
// ============================================================================

uint8_t Esp32StoragePolicy::bufferFillPercent() const {
    if (_ring.isAllocated()) {
        return (uint8_t)_ring.fillPercent();
    }
    // Fallback: use fill buffer fill %
    return (_state->uploadBufCapacity > 0)
        ? (uint8_t)((_state->uploadWriteBufLen * 100) / _state->uploadBufCapacity)
        : 0;
}

Esp32StoragePolicy::WriterStats Esp32StoragePolicy::writerStats() const {
    return WriterStats{
        _writerBytesWritten.load(std::memory_order_relaxed),
        _writerWriteCount.load(std::memory_order_relaxed),
        _writerMaxLatency_ms.load(std::memory_order_relaxed),
        _writerTotalStall_ms.load(std::memory_order_relaxed)
    };
}


// ============================================================================
// Writer Buffers (On-demand PSRAM allocation — per upload lifecycle)
// ============================================================================

void Esp32StoragePolicy::allocateWriterBuffers() {
    if (_ring.isAllocated()) return;  // Already allocated

    // Allocate ring buffer from PSRAM (2 MB)
    if (!_ring.init()) {
        STORAGE_LOG("Failed to allocate ring buffer (%u MB PSRAM) -- uploads will use blocking writes",
                    (unsigned)(UPLOAD_RING_CAPACITY / (1024 * 1024)));
        return;
    }

    // Allocate staging buffer from PSRAM (256 KB)
    _writerStaging = (uint8_t*)heap_caps_malloc(WRITER_STAGING_SIZE, MALLOC_CAP_SPIRAM);
    if (!_writerStaging) {
        STORAGE_LOG("Failed to allocate staging buffer (%u KB PSRAM) -- uploads will use blocking writes",
                    (unsigned)(WRITER_STAGING_SIZE / 1024));
        _ring.shutdown();
        return;
    }

    _drainComplete = xSemaphoreCreateBinary();

    STORAGE_LOG("Writer buffers allocated: ring=%uMB staging=%uKB (PSRAM free=%uKB)",
                (unsigned)(UPLOAD_RING_CAPACITY / (1024 * 1024)),
                (unsigned)(WRITER_STAGING_SIZE / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void Esp32StoragePolicy::freeWriterBuffers() {
    bool anyFreed = false;

    if (_writerStaging) {
        heap_caps_free(_writerStaging);
        _writerStaging = nullptr;
        anyFreed = true;
    }

    if (_ring.isAllocated()) {
        _ring.shutdown();
        anyFreed = true;
    }

    if (_drainComplete) {
        vSemaphoreDelete(_drainComplete);
        _drainComplete = nullptr;
        anyFreed = true;
    }

    if (anyFreed) {
        STORAGE_LOG("Writer buffers freed (PSRAM free=%uKB)",
                    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}


// ============================================================================
// Writer Task (On-demand, Core 1, Ring Buffer -> SD Card)
// ============================================================================

void Esp32StoragePolicy::startWriterTask() {
    if (_writerTask) return;  // Already running

    if (!_ring.isAllocated() || !_writerStaging) {
        STORAGE_LOG("Cannot start writer task: buffers not allocated");
        return;
    }

    _writerError.store(false, std::memory_order_relaxed);
    _drainRequested.store(false, std::memory_order_relaxed);
    _writerActive.store(true, std::memory_order_release);
    _writerDone.store(false, std::memory_order_relaxed);

    xTaskCreatePinnedToCore(
        writerTaskFunc,           // Task function
        "StorageWriter",          // Name
        WRITER_TASK_STACK,        // Stack (8 KB)
        this,                     // Parameter
        WRITER_TASK_PRIORITY,     // Priority (same as audio producer)
        &_writerTask,             // Handle
        1                         // Pin to Core 1
    );

    STORAGE_LOG("Writer task started on Core 1 (prio=%u, ring=%uMB, staging=%uKB)",
                (unsigned)WRITER_TASK_PRIORITY,
                (unsigned)(UPLOAD_RING_CAPACITY / (1024 * 1024)),
                (unsigned)(WRITER_STAGING_SIZE / 1024));
}

void Esp32StoragePolicy::stopWriterTask() {
    if (!_writerTask) return;  // Already stopped (idempotent)

    _writerActive.store(false, std::memory_order_release);

    // Wait for task to acknowledge exit
    uint32_t t0 = millis();
    while (!_writerDone.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (millis() - t0 > 5000) {
            STORAGE_LOG("Writer task did not exit in 5s -- force-deleting");
            vTaskDelete(_writerTask);
            break;
        }
    }

    _writerTask = nullptr;
    STORAGE_LOG("Writer task stopped");
}

void Esp32StoragePolicy::writerTaskFunc(void* param) {
    Esp32StoragePolicy* self = static_cast<Esp32StoragePolicy*>(param);

    // --- Startup instrumentation ---
    uint32_t loopCount = 0;
    uint32_t idleCount = 0;
    uint32_t lastHeartbeat = millis();
    bool firstWrite = true;

    STORAGE_LOG("Writer task running on core %d (prio=%d, ring=%u%%, staging=%uKB)",
                xPortGetCoreID(), (int)uxTaskPriorityGet(nullptr),
                (uint8_t)self->_ring.fillPercent(),
                (unsigned)(WRITER_STAGING_SIZE / 1024));

    while (self->_writerActive.load(std::memory_order_acquire)) {
        loopCount++;
        uint32_t avail = self->_ring.availableRead();
        bool drain = self->_drainRequested.load(std::memory_order_acquire);

        if (avail > 0) {
            // --- First-data detection ---
            if (firstWrite) {
                STORAGE_LOG("Writer: first data -- ring=%u%% avail=%luKB after %lu loops (%lu idle)",
                            (uint8_t)self->_ring.fillPercent(),
                            (unsigned long)(avail / 1024),
                            (unsigned long)loopCount,
                            (unsigned long)idleCount);
                firstWrite = false;
            }

            // Read up to staging size from ring
            uint32_t toRead = (avail > WRITER_STAGING_SIZE)
                            ? WRITER_STAGING_SIZE : avail;
            uint32_t bytesRead = self->_ring.readBulk(self->_writerStaging, toRead);

            if (bytesRead > 0) {
                uint32_t t0 = millis();
                size_t written = self->_state->uploadFile.write(
                    self->_writerStaging, bytesRead);
                uint32_t latency = millis() - t0;

                if (written != bytesRead) {
                    STORAGE_LOG("Writer: SD write FAILED: expected %u wrote %u latency=%lums ring=%u%%",
                                (unsigned)bytesRead, (unsigned)written,
                                (unsigned long)latency,
                                (uint8_t)self->_ring.fillPercent());
                    self->_writerError.store(true, std::memory_order_release);
                    // Signal drain complete on error so Core 0 doesn't hang
                    if (self->_drainComplete)
                        xSemaphoreGive(self->_drainComplete);
                    // Wait for error to be acknowledged before looping
                    while (self->_writerActive.load(std::memory_order_acquire) &&
                           self->_writerError.load(std::memory_order_acquire)) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    continue;
                }

                // Update performance counters
                self->_writerBytesWritten.fetch_add(written, std::memory_order_relaxed);
                self->_writerWriteCount.fetch_add(1, std::memory_order_relaxed);
                self->_writerTotalStall_ms.fetch_add(latency, std::memory_order_relaxed);

                // Track max write latency (relaxed CAS loop)
                uint32_t prevMax = self->_writerMaxLatency_ms.load(std::memory_order_relaxed);
                while (latency > prevMax) {
                    if (self->_writerMaxLatency_ms.compare_exchange_weak(
                            prevMax, latency, std::memory_order_relaxed)) break;
                }

                // Log SD write spikes (>50ms is notable for SDMMC)
                if (latency > 50) {
                    STORAGE_LOG("Writer: SD spike %luKB in %lums (%luKB/s) ring=%u%%",
                                (unsigned long)(written / 1024),
                                (unsigned long)latency,
                                (unsigned long)(latency > 0 ? written / latency : 0),
                                (uint8_t)self->_ring.fillPercent());
                }
            }

            // Check if drain is complete (drain requested + ring now empty)
            if (drain && self->_ring.availableRead() == 0) {
                if (self->_drainComplete)
                    xSemaphoreGive(self->_drainComplete);
            }

        } else if (drain) {
            // Drain requested but ring already empty -- signal done
            if (self->_drainComplete)
                xSemaphoreGive(self->_drainComplete);

        } else {
            // No data and no drain -- yield CPU
            idleCount++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        // --- Periodic heartbeat (every 2s while actively writing) ---
        uint32_t now = millis();
        if (!firstWrite && now - lastHeartbeat >= 2000) {
            auto ws = self->writerStats();
            uint32_t avgLat = (ws.writeCount > 0)
                ? (ws.totalStallTime_ms / ws.writeCount) : 0;
            STORAGE_LOG("Writer: loops=%lu idle=%lu ring=%u%% "
                        "sd_written=%luKB writes=%lu avglat=%lums maxlat=%lums",
                        (unsigned long)loopCount, (unsigned long)idleCount,
                        (uint8_t)self->_ring.fillPercent(),
                        (unsigned long)(ws.bytesWritten / 1024),
                        (unsigned long)ws.writeCount,
                        (unsigned long)avgLat,
                        (unsigned long)ws.maxWriteLatency_ms);
            lastHeartbeat = now;
        }
    }

    // --- Exit instrumentation ---
    auto finalStats = self->writerStats();
    STORAGE_LOG("Writer task exiting: loops=%lu idle=%lu sd_written=%luKB writes=%lu maxlat=%lums",
                (unsigned long)loopCount, (unsigned long)idleCount,
                (unsigned long)(finalStats.bytesWritten / 1024),
                (unsigned long)finalStats.writeCount,
                (unsigned long)finalStats.maxWriteLatency_ms);

    self->_writerDone.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}


#endif  // SFX_PLATFORM_ESP32 && SFX_HAS_STORAGE_SERVER
