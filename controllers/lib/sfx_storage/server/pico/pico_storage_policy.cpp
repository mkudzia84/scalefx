/*
 * PicoStoragePolicy — RP2040/RP2350 Single-Core Storage Implementation
 *
 * Simple blocking writes with single heap buffers.
 * See pico_storage_policy.h for the policy interface.
 *
 * Only compiled when SFX_HAS_STORAGE_SERVER is defined (HubFX).
 * Controllers that only use FlashModule for config (LightFX, GearControl)
 * can include sfx_storage without pulling in StorageServer.
 */

#include <platform/sfx_platform.h>

#if !SFX_PLATFORM_ESP32 && defined(SFX_HAS_STORAGE_SERVER)

#include <server/storage_server.h>
#include <platform/diag_log.h>

#define STORAGE_LOG(fmt, ...) SFX_LOG_INFO("[Storage] " fmt, ##__VA_ARGS__)


// ============================================================================
// Buffer Allocation (single heap buffers)
// ============================================================================

bool PicoStoragePolicy::allocateUploadBuffers() {
    if (_state->uploadWriteBuf) return true;  // Already allocated

    _state->uploadWriteBuf = (uint8_t*)malloc(UPLOAD_WRITE_BUF_SIZE);
    if (!_state->uploadWriteBuf) {
        STORAGE_LOG("Failed to allocate upload buffer (%u bytes)", (unsigned)UPLOAD_WRITE_BUF_SIZE);
        return false;
    }

    _state->uploadBufCapacity = UPLOAD_WRITE_BUF_SIZE;
    _state->uploadWriteBufLen = 0;
    STORAGE_LOG("Allocated upload buffer: %u KB", (unsigned)(UPLOAD_WRITE_BUF_SIZE / 1024));
    return true;
}

void PicoStoragePolicy::freeUploadBuffers() {
    free(_state->uploadWriteBuf);
    _state->uploadWriteBuf = nullptr;
    _state->uploadWriteBufLen = 0;
    _state->uploadBufCapacity = 0;
}


// ============================================================================
// Upload Data Handling (blocking write)
// ============================================================================

bool PicoStoragePolicy::onUploadBufferFull() {
    return _state->flushUploadBuffer();
}

#endif  // !SFX_PLATFORM_ESP32 && SFX_HAS_STORAGE_SERVER
