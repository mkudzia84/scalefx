/*
 * EspIdfSdio4BitPolicy — implementation.
 *
 * Mounts the SD card via ESP-IDF's `esp_vfs_fat_sdmmc_mount`, which
 * does:
 *   1. Initialises the SDMMC host (`sdmmc_host_init`)
 *   2. Probes the card, selects bus width, runs initialisation cmds
 *   3. Mounts FATFS on top via VFS at "/sdcard"
 *
 * All file ops thereafter go through `<stdio.h>` / `<dirent.h>` via
 * `NativeFile` against paths under "/sdcard".  POSIX → VFS → FATFS →
 * the IDF SDMMC driver — three layers of well-tested IDF code that
 * has none of the surprises the Arduino-ESP32 `SD_MMC` shim has.
 *
 * Threading
 * ─────────
 *   - Mount / unmount run from a single task (Core 0 in HubFX bring-up).
 *   - Read / write / seek operations are safe across tasks AND cores:
 *     FF_FS_REENTRANT (default sdkconfig in Arduino-ESP32) installs a
 *     FreeRTOS mutex per mount; concurrent fopen/fread/fwrite from
 *     Core 0 (protocol handler) and Core 1 (audio producer / page-
 *     cache reader) interleave correctly.
 *   - The outer `SdCardModule::lock()` survives only as a coarse gate
 *     for mount/unmount AND upload-exclusivity — see Rule 28.
 *
 * Recovery
 * ────────
 *   `SD_INIT` (host-side remount command) calls SdCardModuleT::retryInit,
 *   which calls unmount() + begin() — `esp_vfs_fat_sdcard_unmount` + a
 *   fresh mount.  No process-restart needed.
 */

// sd_card.h MUST come first — it defines SdBusMode + SdCardType + StorageInfo
// AND transitively includes our own header (esp_idf_sd_policy.h) via the
// platform-policy switch at its bottom.  Including esp_idf_sd_policy.h
// directly first would parse the header in isolation, before the enums
// are defined, and break on `SdBusMode::SDIO_4BIT`.
#include "../sd_card.h"

#if SFX_PLATFORM_ESP32

#include "esp_idf_sd_policy.h"

#include <cstring>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <esp_log.h>

// ──────────────────────────────────────────────────────────────────────
//  Mount / unmount
// ──────────────────────────────────────────────────────────────────────

bool EspIdfSdio4BitPolicy::mount(const Config& cfg) {
    if (_mounted) return true;

    // SDMMC host — Slot 1 (the only one that supports 4-bit SDIO + custom
    // pins on ESP32-S3).  SDMMC_HOST_DEFAULT() macro yields a fully-
    // populated host config; we tweak frequency afterwards if requested.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    if (cfg.speed_mhz > 0) {
        // SDMMC_FREQ_DEFAULT = 20000 kHz, SDMMC_FREQ_HIGHSPEED = 40000 kHz.
        host.max_freq_khz = (int)cfg.speed_mhz * 1000;
    }

    // Slot pin assignment — 4-bit mode requires all D0..D3 in addition
    // to CLK + CMD.  GPIO matrix routing applies (HubFX rev uses
    // CLK=39, CMD=38, D0=40, D1=41, D2=42, D3=45 — JTAG-reuse pins).
    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;
    slot_cfg.clk   = (gpio_num_t)cfg.clk;
    slot_cfg.cmd   = (gpio_num_t)cfg.cmd;
    slot_cfg.d0    = (gpio_num_t)cfg.d0;
    slot_cfg.d1    = (gpio_num_t)cfg.d1;
    slot_cfg.d2    = (gpio_num_t)cfg.d2;
    slot_cfg.d3    = (gpio_num_t)cfg.d3;
    // Enable internal pull-ups on the data lines.  HubFX has external
    // pull-ups on CMD + DATA but the IDF flag is cheap insurance and
    // matches what `SD_MMC.begin` did under the hood.
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Mount config — formatIfFailed reformats a corrupt card on first
    // boot (matching SD_MMC.begin(formatIfFailed)).  max_files caps the
    // VFS file-descriptor pool — keep it at 5 by default; the storage
    // server holds one for upload, the audio mixer one for the streaming
    // source, the page cache one per concurrent load, plus headroom.
    // allocation_unit_size = 0 lets FATFS pick the default for the card.
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = cfg.formatIfFailed;
    mount_cfg.max_files              = cfg.maxOpenFiles ? cfg.maxOpenFiles : 5;
    mount_cfg.allocation_unit_size   = 0;

    sdmmc_card_t* card = nullptr;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint,
                                            &host,
                                            &slot_cfg,
                                            &mount_cfg,
                                            &card);
    if (err != ESP_OK) {
        ESP_LOGE("sd_idf",
                 "esp_vfs_fat_sdmmc_mount(%s) failed: %s (0x%x)",
                 kMountPoint, esp_err_to_name(err), (unsigned)err);
        _card    = nullptr;
        _mounted = false;
        return false;
    }

    _card    = card;     // implicit cast to void*
    _mounted = true;
    return true;
}

void EspIdfSdio4BitPolicy::unmount() {
    if (!_mounted) return;
    esp_vfs_fat_sdcard_unmount(kMountPoint, static_cast<sdmmc_card_t*>(_card));
    _card    = nullptr;
    _mounted = false;
}


// ──────────────────────────────────────────────────────────────────────
//  Card / storage queries
// ──────────────────────────────────────────────────────────────────────

SdCardType EspIdfSdio4BitPolicy::cardType() {
    if (!_mounted || !_card) return SdCardType::NONE;
    auto* card = static_cast<sdmmc_card_t*>(_card);

    // `card->ocr` carries the OCR register from card init; the CCS bit
    // (bit 30) tells us SDSC vs SDHC/SDXC.  MMC is detected by an
    // `is_mmc` flag the IDF driver sets.
    if (card->is_mmc) return SdCardType::MMC;
    constexpr uint32_t SD_OCR_SDHC_CAP = (1u << 30);
    if (card->ocr & SD_OCR_SDHC_CAP) return SdCardType::SDHC;
    return SdCardType::SD;
}

void EspIdfSdio4BitPolicy::fillStorageInfo(StorageInfo& info) {
    info.cardSize_MB       = 0;
    info.totalSpace_MB     = 0;
    info.usedSpace_MB      = 0;
    info.freeSpace_MB      = 0;
    info.fatType           = 0;
    info.clusterSize_bytes = 0;

    if (!_mounted || !_card) return;
    auto* card = static_cast<sdmmc_card_t*>(_card);

    // Card size from CSD.
    const uint64_t cardBytes = (uint64_t)card->csd.capacity *
                               (uint64_t)card->csd.sector_size;
    info.cardSize_MB = (uint32_t)(cardBytes / (1024ULL * 1024ULL));

    // Total / used / free from FATFS — query via f_getfree on the mount
    // drive.  esp_vfs_fat_sdmmc_mount registers the card as drive "0:".
    FATFS*   fs        = nullptr;
    DWORD    fre_clust = 0;
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK && fs) {
        const uint32_t sectorSize  = card->csd.sector_size;
        const uint64_t totalSect   = (uint64_t)(fs->n_fatent - 2) * fs->csize;
        const uint64_t freeSect    = (uint64_t)fre_clust * fs->csize;
        const uint64_t totalBytes  = totalSect * sectorSize;
        const uint64_t freeBytes   = freeSect  * sectorSize;
        info.totalSpace_MB     = (uint32_t)(totalBytes / (1024ULL * 1024ULL));
        info.freeSpace_MB      = (uint32_t)(freeBytes  / (1024ULL * 1024ULL));
        info.usedSpace_MB      = info.totalSpace_MB > info.freeSpace_MB
                                 ? info.totalSpace_MB - info.freeSpace_MB : 0;
        info.fatType           = (uint8_t)fs->fs_type;        // FS_FAT12 / FAT16 / FAT32 / EXFAT
        info.clusterSize_bytes = (uint32_t)fs->csize * sectorSize;
    }
}


// ──────────────────────────────────────────────────────────────────────
//  Filesystem primitives (POSIX → "/sdcard/..")
// ──────────────────────────────────────────────────────────────────────

namespace {
// Join "/sdcard" + user path into out (size 192) — same convention as
// NativeFile's joinPath but exposed here because the non-file ops
// (remove / exists / mkdir / rmdir) need an explicit path string.
void joinSd(char* out, size_t outSize, const char* path) {
    if (!path) path = "";
    const bool needsSlash = (path[0] != '/');
    snprintf(out, outSize, "/sdcard%s%s",
             needsSlash ? "/" : "", path);
}
}  // namespace

bool EspIdfSdio4BitPolicy::exists(const char* path) {
    char full[192];
    joinSd(full, sizeof(full), path);
    struct stat st;
    return stat(full, &st) == 0;
}

bool EspIdfSdio4BitPolicy::removeFile(const char* path) {
    char full[192];
    joinSd(full, sizeof(full), path);
    return remove(full) == 0;
}

bool EspIdfSdio4BitPolicy::makeDir(const char* path) {
    char full[192];
    joinSd(full, sizeof(full), path);
    // 0775 — matches the permission bits the IDF VFS defaults to; FATFS
    // ignores permission bits anyway.
    return mkdir(full, 0775) == 0;
}

bool EspIdfSdio4BitPolicy::removeDir(const char* path) {
    char full[192];
    joinSd(full, sizeof(full), path);
    return rmdir(full) == 0;
}

void EspIdfSdio4BitPolicy::extractName(FileHandle& f, char* buf, size_t len) {
    const char* n = f.name();
    if (!n) { buf[0] = '\0'; return; }
    strncpy(buf, n, len - 1);
    buf[len - 1] = '\0';
}

#endif  // SFX_PLATFORM_ESP32
