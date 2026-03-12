/*
 * SD Card Module — Thread-Safe File Operations
 *
 * Singleton class for SD card initialization and file operations.
 * Provides thread-safe SPI access via platform-abstracted mutex
 * for multi-core safety.
 *
 * All methods return SdError codes (0 = OK) and never write to Serial.
 * Protocol output is handled by StorageServer using StreamWriter.
 *
 * Usage:
 *   SdCardModule& sd = SdCardModule::instance();
 *   sd.begin(cs, sck, mosi, miso);
 *
 *   // Thread-safe listing
 *   sd.listDirectory("/", [](const FileEntry& e) {
 *       // ... process entry
 *       return true;  // continue
 *   });
 *
 *   // File I/O (caller manages lock + file lifetime)
 *   sd.lock();
 *   SdFile file;
 *   sd.openRead("/config.yaml", file);
 *   int n = file.read(buf, sizeof(buf));
 *   file.close();
 *   sd.unlock();
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <Arduino.h>
#include "platform/sfx_platform.h"
#include <functional>

#include "storage_types.h"

// ============================================================================
// Platform SD Backend Detection
// ============================================================================

#if SFX_PLATFORM_PICO
    #include <SPI.h>
    #if __has_include(<SdFat.h>)
        #include <SdFat.h>
        #define SFX_SD_BACKEND_SDFAT 1
    #else
        #define SFX_SD_BACKEND_SDFAT 0
    #endif
    #define SFX_SD_BACKEND_ESP 0
#elif SFX_PLATFORM_ESP32
    #include <SPI.h>
    #include <FS.h>
    #include <SD.h>
    #include <SD_MMC.h>
    #define SFX_SD_BACKEND_SDFAT 0
    #define SFX_SD_BACKEND_ESP 1
#else
    #define SFX_SD_BACKEND_SDFAT 0
    #define SFX_SD_BACKEND_ESP 0
#endif

#define SFX_HAS_SD (SFX_SD_BACKEND_SDFAT || SFX_SD_BACKEND_ESP)

#if SFX_HAS_SD

// ============================================================================
// Platform File Type
// ============================================================================

#if SFX_SD_BACKEND_SDFAT
    using SdFile = File32;
#elif SFX_SD_BACKEND_ESP
    using SdFile = fs::File;
#endif


// ============================================================================
// SD Bus Mode
// ============================================================================

/**
 * @brief SD card bus connection mode
 *
 * SPI is available on all platforms.
 * SDIO modes are ESP32-only and provide significantly higher throughput
 * (~20-25 MB/s for 4-bit vs ~2-4 MB/s for SPI).
 */
enum class SdBusMode : uint8_t {
    SPI,        ///< SPI bus (all platforms)
    SDIO_1BIT,  ///< 1-bit SDIO (ESP32 only)
    SDIO_4BIT   ///< 4-bit SDIO (ESP32 only, highest throughput)
};

// ============================================================================
// Error Codes (module-internal, mapped to HubFxError in StorageServer)
// ============================================================================

namespace SdError {
    constexpr uint8_t OK              = 0;
    constexpr uint8_t NOT_INITIALIZED = 1;
    constexpr uint8_t NOT_FOUND       = 2;
    constexpr uint8_t IO_ERROR        = 3;
    constexpr uint8_t IS_DIRECTORY    = 4;
    constexpr uint8_t ALREADY_EXISTS  = 5;
}


// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief SD card storage information
 */
struct StorageInfo {
    bool initialized;
    uint32_t cardSize_MB;
    uint32_t totalSpace_MB;
    uint32_t freeSpace_MB;
    uint8_t fatType;             ///< FAT16/32 (Pico/SdFat), 0 on ESP32
    uint32_t clusterSize_bytes;   ///< Cluster size (Pico/SdFat), 0 on ESP32
    SdBusMode busMode;            ///< Active bus mode
};


// ============================================================================
// SdCardModule
// ============================================================================

class SdCardModule {
public:
    /// Get the singleton instance
    static SdCardModule& instance() {
        static SdCardModule inst;
        return inst;
    }

    // Delete copy/move
    SdCardModule(const SdCardModule&) = delete;
    SdCardModule& operator=(const SdCardModule&) = delete;
    SdCardModule(SdCardModule&&) = delete;
    SdCardModule& operator=(SdCardModule&&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize SD card in SPI mode
     *
     * Works on all platforms. Uses SdFat on Pico, SD.h on ESP32.
     *
     * @param cs_pin   SPI chip select
     * @param sck_pin  SPI clock
     * @param mosi_pin SPI MOSI
     * @param miso_pin SPI MISO
     * @param speed_mhz SPI clock speed (default 25 MHz)
     * @return true on success
     */
    bool begin(uint8_t cs_pin, uint8_t sck_pin, uint8_t mosi_pin,
               uint8_t miso_pin, uint8_t speed_mhz = 25);

#if SFX_SD_BACKEND_ESP
    /**
     * @brief Initialize SD card in SDIO mode (ESP32 only)
     *
     * Uses SD_MMC driver for 1-bit or 4-bit SDIO.
     * Pass -1 for any pin to use platform defaults.
     * 4-bit SDIO provides ~20-25 MB/s vs SPI's ~2-4 MB/s.
     *
     * @param oneBitMode true for 1-bit SDIO, false for 4-bit (default)
     * @param clk  SDIO clock pin (-1 = default)
     * @param cmd  SDIO command pin (-1 = default)
     * @param d0   SDIO data 0 pin (-1 = default)
     * @param d1   SDIO data 1 pin (-1 = default, 4-bit only)
     * @param d2   SDIO data 2 pin (-1 = default, 4-bit only)
     * @param d3   SDIO data 3 pin (-1 = default, 4-bit only)
     * @return true on success
     */
    bool beginSDIO(bool oneBitMode = false,
                   int8_t clk = -1, int8_t cmd = -1,
                   int8_t d0 = -1, int8_t d1 = -1,
                   int8_t d2 = -1, int8_t d3 = -1);
#endif

    /**
     * @brief Retry initialization at current or different speed
     * @param speed_mhz New SPI speed in MHz (0 = same speed, ignored for SDIO)
     * @return true on success
     */
    bool retryInit(uint8_t speed_mhz = 0);

    /// Check if SD card is initialized and ready
    bool isInitialized() const { return _initialized; }

    /// Get active bus mode
    SdBusMode busMode() const { return _busMode; }

    // ========================================================================
    // Directory Operations (thread-safe, lock acquired internally)
    // ========================================================================

    /**
     * @brief List directory contents
     *
     * Calls the callback for each entry. Return false from callback to stop.
     * Acquires mutex internally.
     *
     * @param path Directory path (e.g., "/", "/sounds")
     * @param callback Called for each FileEntry
     * @return SdError code
     */
    uint8_t listDirectory(const char* path,
                          std::function<bool(const FileEntry&)> callback);

    /**
     * @brief List directory tree recursively
     *
     * Calls the callback for each entry with depth level.
     * Acquires mutex internally.
     *
     * @param path Root path
     * @param callback Called with (entry, depth_level)
     * @return SdError code
     */
    uint8_t listTree(const char* path,
                     std::function<bool(const FileEntry&, int depth)> callback);

    // ========================================================================
    // File Information (thread-safe, lock acquired internally)
    // ========================================================================

    /**
     * @brief Get file or directory information
     * @param path File path
     * @param entry Output file entry
     * @return SdError code
     */
    uint8_t getFileInfo(const char* path, FileEntry& entry);

    /**
     * @brief Get SD card storage information
     * @param info Output storage info
     * @return SdError code
     */
    uint8_t getStorageInfo(StorageInfo& info);

    // ========================================================================
    // File Modification (thread-safe, lock acquired internally)
    // ========================================================================

    /**
     * @brief Remove a file
     * @param path File path
     * @return SdError code
     */
    uint8_t removeFile(const char* path);

    /**
     * @brief Create directory (recursive)
     * @param path Directory path
     * @return SdError code
     */
    uint8_t makeDirectory(const char* path);

    // ========================================================================
    // File I/O (caller MUST hold lock via lock()/unlock())
    // ========================================================================

    /**
     * @brief Open file for reading
     *
     * Caller MUST lock() before and unlock() after all file operations.
     *
     * @param path File path
     * @param file Output file handle
     * @return SdError code
     */
    uint8_t openRead(const char* path, SdFile& file);

    /**
     * @brief Open file for writing
     *
     * Caller MUST lock() before and unlock() after all file operations.
     *
     * @param path File path
     * @param file Output file handle
     * @param truncate If true, truncate existing file
     * @return SdError code
     */
    uint8_t openWrite(const char* path, SdFile& file, bool truncate = true);

    // ========================================================================
    // Mutex Access
    // ========================================================================

    void lock()     { sfxMutexLock(_sdMutex); }
    bool tryLock()  { return sfxMutexTryLock(_sdMutex); }
    void unlock()   { sfxMutexUnlock(_sdMutex); }

    /// Direct filesystem access (caller must hold lock)
#if SFX_SD_BACKEND_SDFAT
    SdFat& getSd()  { return _sd; }
#elif SFX_SD_BACKEND_ESP
    fs::FS& getFS() { return *_fs; }
#endif

private:
    SdCardModule();

#if SFX_SD_BACKEND_SDFAT
    SdFat _sd;
#elif SFX_SD_BACKEND_ESP
    fs::FS* _fs;                ///< Points to SD or SD_MMC (not owned)
#endif

    bool _initialized;
    SfxMutex _sdMutex;
    SdBusMode _busMode;

    // Stored SPI pin config for retryInit()
    uint8_t _cs_pin;
    uint8_t _sck_pin;
    uint8_t _mosi_pin;
    uint8_t _miso_pin;
    uint8_t _speed_mhz;

#if SFX_SD_BACKEND_ESP
    // Stored SDIO config for retryInit()
    bool _sdioOneBit;
    int8_t _sdioClk, _sdioCmd, _sdioD0, _sdioD1, _sdioD2, _sdioD3;
#endif

    // Internal recursive tree listing (caller holds lock)
    void listTreeRecursive(const char* path, int depth,
                           std::function<bool(const FileEntry&, int)>& callback,
                           bool& shouldContinue);
};

#endif // SFX_HAS_SD
#endif // SD_CARD_H
