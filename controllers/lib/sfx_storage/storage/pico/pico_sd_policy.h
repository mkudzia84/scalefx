/*
 * Pico SPI SD Policy — SdFat Backend for RP2040/RP2350
 *
 * Do not include this file directly — include <storage/sd_card.h>.
 * Auto-included when SFX_PLATFORM_PICO is defined and SdFat is available.
 *
 * Uses the SdFat library for all file operations.
 * File handle type: File32.
 */

#ifndef PICO_SD_POLICY_H
#define PICO_SD_POLICY_H

#include <SPI.h>
#include <SdFat.h>


struct PicoSpiSdPolicy {

    // ========================================================================
    // Policy Types
    // ========================================================================

    struct Config {
        uint8_t cs_pin;
        uint8_t sck_pin;
        uint8_t mosi_pin;
        uint8_t miso_pin;
        uint8_t speed_mhz = 25;
    };

    using FileHandle = File32;
    static constexpr SdBusMode BUS_MODE = SdBusMode::SPI;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    bool mount(const Config& cfg) {
        SPI.setRX(cfg.miso_pin);
        SPI.setTX(cfg.mosi_pin);
        SPI.setSCK(cfg.sck_pin);
        return _sd.begin(cfg.cs_pin, SD_SCK_MHZ(cfg.speed_mhz));
    }

    void unmount() {
        // SdFat doesn't have an explicit unmount; closing all files suffices
    }

    // ========================================================================
    // Card Queries
    // ========================================================================

    SdCardType cardType() {
        uint8_t ct = _sd.card()->type();
        if (ct == SD_CARD_TYPE_SD1 || ct == SD_CARD_TYPE_SD2) return SdCardType::SD;
        if (ct == SD_CARD_TYPE_SDHC) return SdCardType::SDHC;
        return SdCardType::UNKNOWN;
    }

    void fillStorageInfo(StorageInfo& info) {
        info.cardSize_MB = (uint32_t)(_sd.card()->sectorCount() / 2048);
        info.fatType = _sd.fatType();
        info.clusterSize_bytes = _sd.bytesPerCluster();

        uint32_t clusters = _sd.clusterCount();
        uint32_t freeClusters = _sd.freeClusterCount();
        uint64_t bpc = _sd.bytesPerCluster();
        info.totalSpace_MB = (uint32_t)((uint64_t)clusters * bpc / 1048576ULL);
        info.freeSpace_MB  = (uint32_t)((uint64_t)freeClusters * bpc / 1048576ULL);
        info.usedSpace_MB  = info.totalSpace_MB > info.freeSpace_MB
                             ? info.totalSpace_MB - info.freeSpace_MB : 0;
    }

    // ========================================================================
    // Filesystem Primitives
    // ========================================================================

    FileHandle openDir(const char* path) {
        return _sd.open(path, O_RDONLY);
    }

    FileHandle openReadFile(const char* path) {
        return _sd.open(path, O_RDONLY);
    }

    FileHandle openWriteFile(const char* path, bool truncate) {
        oflag_t flags = O_WRONLY | O_CREAT;
        if (truncate) flags |= O_TRUNC;
        return _sd.open(path, flags);
    }

    static FileHandle nextFile(FileHandle& dir) {
        return dir.openNextFile();
    }

    bool exists(const char* path) { return _sd.exists(path); }
    bool removeFile(const char* path) { return _sd.remove(path); }
    bool makeDir(const char* path) { return _sd.mkdir(path); }
    bool removeDir(const char* path) { return _sd.rmdir(path); }

    // ========================================================================
    // File Handle Inspection
    // ========================================================================

    static bool isValid(const FileHandle& f) { return (bool)f; }
    static bool isDirectory(FileHandle& f) { return f.isDirectory(); }
    static uint32_t fileSize(FileHandle& f) { return (uint32_t)f.size(); }
    static void closeFile(FileHandle& f) { f.close(); }

    static void extractName(FileHandle& f, char* buf, size_t len) {
        f.getName(buf, len);
    }

    // ========================================================================
    // Raw Access
    // ========================================================================

    /// Direct SdFat access (caller must hold SdCardModule lock)
    SdFat32& rawSD() { return _sd; }

private:
    SdFat32 _sd;   // FAT-only variant — _sd.open() returns File32
};


#endif // PICO_SD_POLICY_H
