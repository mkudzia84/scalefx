/*
 * bring_up.h — one-call hardware probe for the storage stack.
 *
 *   bringUpStorage(sdCfg)
 *     1. Initialises `FlashModule::instance()` (LittleFS).
 *     2. Initialises `SdCardModule::instance()` with the supplied
 *        pin config (4-bit SDIO on ESP32, SPI on Pico).
 *     3. Logs success / failure for both backends — same lines every
 *        board would otherwise hand-write.
 *
 *   Returns a `BringUpResult` so the caller can branch on partial
 *   failure (Flash up but SD absent, etc.), but most boards just
 *   ignore the return value — both backends self-recover later via
 *   `SD_INIT` + `FLASH_STATUS_REQ` from the host.
 *
 *   The `StorageServicePolicy<...>` policy already routes the wire
 *   commands — this header just centralises the hardware probe so it
 *   isn't duplicated across every controller sketch.
 *
 *   Build gate: `SFX_HAS_STORAGE` (same as `flash.cpp` / `sd_card.h`).
 */

#ifndef SFX_STORAGE_BRING_UP_H
#define SFX_STORAGE_BRING_UP_H

#if defined(SFX_HAS_STORAGE)

#include <serial/diag_log.h>

#include "flash.h"
#include "sd_card.h"

struct BringUpResult {
    bool flashOk = false;
    bool sdOk    = false;
};

/// Probe LittleFS + SD with the supplied pin map.  Logs through
/// `SFX_LOG_*` so the result is visible on the COBS wire's
/// `LOG_MESSAGE` channel without a per-board log line.
inline BringUpResult bringUpStorage(const SdCardModule::Config& sdCfg) {
    BringUpResult r;

    // ── LittleFS flash ───────────────────────────────────────────────
    FlashModule& flash = FlashModule::instance();
    if (flash.begin()) {
        FlashStorageInfo info;
        flash.getStorageInfo(info);
        SFX_LOG_INFO("Flash ready: %lu/%lu bytes used",
                     (unsigned long)info.usedBytes,
                     (unsigned long)info.totalBytes);
        r.flashOk = true;
    } else {
        SFX_LOG_ERROR("Flash init failed");
    }

    // ── SD card ──────────────────────────────────────────────────────
    SdCardModule& sd = SdCardModule::instance();
    if (sd.begin(sdCfg)) {
        StorageInfo info;
        sd.getStorageInfo(info);
        SFX_LOG_INFO("SD ready: %lu MB (free %lu MB)",
                     (unsigned long)info.cardSize_MB,
                     (unsigned long)info.freeSpace_MB);
        r.sdOk = true;
    } else {
        SFX_LOG_WARN("SD card not available (no card inserted?)");
    }

    return r;
}

#endif  // SFX_HAS_STORAGE
#endif  // SFX_STORAGE_BRING_UP_H
