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
 *   File32 file;
 *   sd.openRead("/config.yaml", file);
 *   int n = file.read(buf, sizeof(buf));
 *   file.close();
 *   sd.unlock();
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <Arduino.h>
#include <SPI.h>
#if __has_include(<SdFat.h>)
    #include <SdFat.h>
    #define SFX_HAS_SDFAT 1
#else
    #define SFX_HAS_SDFAT 0
#endif
#include "platform/sfx_platform.h"
#include <functional>

#include "storage_types.h"

#if SFX_HAS_SDFAT

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
    uint8_t fatType;
    uint32_t clusterSize_bytes;
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
     * @brief Initialize SD card with given SPI pins
     * @return true on success
     */
    bool begin(uint8_t cs_pin, uint8_t sck_pin, uint8_t mosi_pin,
               uint8_t miso_pin, uint8_t speed_mhz = 25);

    /**
     * @brief Retry initialization at different speed
     * @return true on success
     */
    bool retryInit(uint8_t speed_mhz);

    /// Check if SD card is initialized and ready
    bool isInitialized() const { return _initialized; }

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
    uint8_t openRead(const char* path, File32& file);

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
    uint8_t openWrite(const char* path, File32& file, bool truncate = true);

    // ========================================================================
    // Mutex Access
    // ========================================================================

    void lock()     { sfxMutexLock(_sdMutex); }
    bool tryLock()  { return sfxMutexTryLock(_sdMutex); }
    void unlock()   { sfxMutexUnlock(_sdMutex); }

    /// Direct SdFat access (caller must hold lock)
    SdFat& getSd()  { return _sd; }

private:
    SdCardModule();

    SdFat _sd;
    bool _initialized;
    SfxMutex _sdMutex;

    // Stored pin config for retry
    uint8_t _cs_pin;
    uint8_t _sck_pin;
    uint8_t _mosi_pin;
    uint8_t _miso_pin;

    // Internal recursive tree listing (caller holds lock)
    void listTreeRecursive(const char* path, int depth,
                           std::function<bool(const FileEntry&, int)>& callback,
                           bool& shouldContinue);
};

#endif // SFX_HAS_SDFAT
#endif // SD_CARD_H
