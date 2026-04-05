/*
 * ESP32 SD Policies — SPI and SDIO Bus Modes
 *
 * Do not include this file directly — include <storage/sd_card.h>.
 * Auto-included when SFX_PLATFORM_ESP32 is defined.
 *
 * Three policies for ESP32:
 *   EspSpiSdPolicy        — SPI via SD.h library
 *   EspSdio1BitPolicy     — 1-bit SDIO via SD_MMC.h library
 *   EspSdio4BitPolicy     — 4-bit SDIO via SD_MMC.h library
 *
 * All ESP32 policies share common file operations via the EspSdFileOps
 * base class, which operates through an fs::FS* pointer set during mount().
 * Only mount/unmount/cardType/fillStorageInfo differ between bus modes.
 */

#ifndef ESP32_SD_POLICIES_H
#define ESP32_SD_POLICIES_H

#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <SD_MMC.h>


// ============================================================================
// Common ESP32 File Operations Base
// ============================================================================

/**
 * @brief Shared file operation primitives for all ESP32 SD policies.
 *
 * All ESP32 bus modes (SPI, SDIO 1-bit, SDIO 4-bit) use the same
 * fs::FS interface for file operations. This base class implements
 * those operations once, dispatching through the _fs pointer that
 * is set by each policy's mount() method.
 */
class EspSdFileOps {
protected:
    fs::FS* _fs = nullptr;

public:
    using FileHandle = fs::File;

    // ========================================================================
    // Filesystem Primitives
    // ========================================================================

    FileHandle openDir(const char* path) {
        return _fs->open(path);
    }

    FileHandle openReadFile(const char* path) {
        return _fs->open(path, FILE_READ);
    }

    FileHandle openWriteFile(const char* path, bool truncate) {
        return _fs->open(path, truncate ? FILE_WRITE : FILE_APPEND, true);
    }

    static FileHandle nextFile(FileHandle& dir) {
        return dir.openNextFile();
    }

    bool exists(const char* path) { return _fs->exists(path); }
    bool removeFile(const char* path) { return _fs->remove(path); }
    bool makeDir(const char* path) { return _fs->mkdir(path); }
    bool removeDir(const char* path) { return _fs->rmdir(path); }

    // ========================================================================
    // File Handle Inspection
    // ========================================================================

    static bool isValid(const FileHandle& f) { return (bool)f; }
    static bool isDirectory(FileHandle& f) { return f.isDirectory(); }
    static uint32_t fileSize(FileHandle& f) { return (uint32_t)f.size(); }
    static void closeFile(FileHandle& f) { f.close(); }

    /**
     * @brief Extract basename from ESP32 file handle.
     *
     * ESP32's File.name() may return a full path (e.g., "/sounds/engine.wav").
     * This method extracts just the filename portion.
     */
    static void extractName(FileHandle& f, char* buf, size_t len) {
        const char* name = f.name();
        const char* slash = strrchr(name, '/');
        const char* baseName = slash ? slash + 1 : name;
        strncpy(buf, baseName, len - 1);
        buf[len - 1] = '\0';
    }

    // ========================================================================
    // Raw Access
    // ========================================================================

    /// Direct fs::FS access (caller must hold SdCardModule lock)
    fs::FS& rawFS() { return *_fs; }
};


// ============================================================================
// ESP32 SPI SD Policy
// ============================================================================

/**
 * @brief ESP32 SPI SD card policy using the Arduino SD.h library.
 *
 * Config pins: cs, sck, mosi, miso, speed_mhz.
 * Uses SPI bus for card communication (~2-4 MB/s typical).
 */
struct EspSpiSdPolicy : public EspSdFileOps {

    struct Config {
        uint8_t cs_pin;
        uint8_t sck_pin;
        uint8_t mosi_pin;
        uint8_t miso_pin;
        uint8_t speed_mhz = 25;
    };

    static constexpr SdBusMode BUS_MODE = SdBusMode::SPI;

    bool mount(const Config& cfg) {
        SPI.begin(cfg.sck_pin, cfg.miso_pin, cfg.mosi_pin, cfg.cs_pin);
        bool ok = SD.begin(cfg.cs_pin, SPI, (uint32_t)cfg.speed_mhz * 1000000U);
        if (ok) _fs = &SD;
        return ok;
    }

    void unmount() {
        SD.end();
        _fs = nullptr;
    }

    SdCardType cardType() {
        sdcard_type_t ct = SD.cardType();
        switch (ct) {
            case CARD_NONE:  return SdCardType::NONE;
            case CARD_MMC:   return SdCardType::MMC;
            case CARD_SD:    return SdCardType::SD;
            case CARD_SDHC:  return SdCardType::SDHC;
            default:         return SdCardType::UNKNOWN;
        }
    }

    void fillStorageInfo(StorageInfo& info) {
        info.cardSize_MB   = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
        info.totalSpace_MB = (uint32_t)(SD.totalBytes() / (1024ULL * 1024ULL));
        info.usedSpace_MB  = (uint32_t)(SD.usedBytes() / (1024ULL * 1024ULL));
        info.freeSpace_MB  = info.totalSpace_MB > info.usedSpace_MB
                             ? info.totalSpace_MB - info.usedSpace_MB : 0;
        info.fatType = 0;
        info.clusterSize_bytes = 0;
    }
};


// ============================================================================
// ESP32 SDIO SD Policy (1-bit or 4-bit)
// ============================================================================

/**
 * @brief ESP32 SDIO SD card policy using the SD_MMC library.
 *
 * Template parameter OneBit selects 1-bit (true) or 4-bit (false) mode.
 * SDIO provides significantly higher throughput than SPI:
 *   1-bit: ~10-12 MB/s
 *   4-bit: ~20-25 MB/s
 *
 * Config pins: clk, cmd, d0 (required); d1, d2, d3 (4-bit only).
 * Pass -1 for any pin to use platform defaults.
 */
template <bool OneBit>
struct EspSdioSdPolicy : public EspSdFileOps {

    struct Config {
        int8_t clk = -1;          ///< SDIO clock pin (-1 = default)
        int8_t cmd = -1;          ///< SDIO command pin (-1 = default)
        int8_t d0  = -1;          ///< SDIO data 0 pin (-1 = default)
        int8_t d1  = -1;          ///< 4-bit only (ignored when OneBit=true)
        int8_t d2  = -1;          ///< 4-bit only
        int8_t d3  = -1;          ///< 4-bit only
        bool formatIfFailed = true;   ///< Format card if mount fails
        uint8_t maxOpenFiles = 5;     ///< Maximum simultaneous open files
        uint8_t speed_mhz = 0;       ///< Ignored for SDIO (API compat with SPI)
    };

    static constexpr SdBusMode BUS_MODE = OneBit ? SdBusMode::SDIO_1BIT
                                                  : SdBusMode::SDIO_4BIT;

    bool mount(const Config& cfg) {
        // Set custom pins if any specified (all -1 = use platform defaults)
        if (cfg.clk >= 0 || cfg.cmd >= 0 || cfg.d0 >= 0) {
            SD_MMC.setPins(cfg.clk, cfg.cmd, cfg.d0, cfg.d1, cfg.d2, cfg.d3);
        }

        bool ok = SD_MMC.begin("/sdcard", OneBit, cfg.formatIfFailed,
                                SDMMC_FREQ_HIGHSPEED, cfg.maxOpenFiles);
        if (ok) _fs = &SD_MMC;
        return ok;
    }

    void unmount() {
        SD_MMC.end();
        _fs = nullptr;
    }

    SdCardType cardType() {
        sdcard_type_t ct = SD_MMC.cardType();
        switch (ct) {
            case CARD_NONE:  return SdCardType::NONE;
            case CARD_MMC:   return SdCardType::MMC;
            case CARD_SD:    return SdCardType::SD;
            case CARD_SDHC:  return SdCardType::SDHC;
            default:         return SdCardType::UNKNOWN;
        }
    }

    void fillStorageInfo(StorageInfo& info) {
        info.cardSize_MB   = (uint32_t)(SD_MMC.cardSize() / (1024ULL * 1024ULL));
        info.totalSpace_MB = (uint32_t)(SD_MMC.totalBytes() / (1024ULL * 1024ULL));
        info.usedSpace_MB  = (uint32_t)(SD_MMC.usedBytes() / (1024ULL * 1024ULL));
        info.freeSpace_MB  = info.totalSpace_MB > info.usedSpace_MB
                             ? info.totalSpace_MB - info.usedSpace_MB : 0;
        info.fatType = 0;
        info.clusterSize_bytes = 0;
    }
};

/// @brief 1-bit SDIO policy (~10-12 MB/s)
using EspSdio1BitPolicy = EspSdioSdPolicy<true>;

/// @brief 4-bit SDIO policy (~20-25 MB/s, highest throughput)
using EspSdio4BitPolicy = EspSdioSdPolicy<false>;


#endif // ESP32_SD_POLICIES_H
