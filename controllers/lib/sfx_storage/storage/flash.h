/*
 * Flash Module — Thread-Safe LittleFS File Operations
 *
 * Singleton class for onboard flash (LittleFS) initialization and
 * file operations. Provides thread-safe access via platform-abstracted
 * mutex for multi-core safety.
 *
 * All methods return FlashError codes (0 = OK) and never write to
 * Serial. Protocol output is handled by StorageServer using
 * StreamWriter.
 *
 * API mirrors SdCardModule so StorageServer can work with either
 * storage backend uniformly.
 *
 * Usage:
 *   FlashModule& flash = FlashModule::instance();
 *   flash.begin();
 *
 *   // Thread-safe listing
 *   flash.listDirectory("/", [](const FileEntry& e) {
 *       // ... process entry
 *       return true;  // continue
 *   });
 *
 *   // File I/O (caller manages lock + file lifetime)
 *   flash.lock();
 *   StorageFile file;
 *   flash.openRead("/config.yaml", file);
 *   int n = file.read(buf, sizeof(buf));
 *   file.close();
 *   flash.unlock();
 */

#ifndef FLASH_H
#define FLASH_H

#include <Arduino.h>
#include "platform/sfx_platform.h"
#include <functional>

#include "storage_types.h"

// File handle type — ESP32 uses NativeFile (POSIX RAII over VFS); Pico
// still uses Arduino LittleFS's `::File`.
#if SFX_PLATFORM_ESP32
    #include "esp32/native_file.h"
    using StorageFile = NativeFile;
#else
    #include <LittleFS.h>
    using StorageFile = ::File;
#endif


// ============================================================================
// Error Codes (module-internal, mapped to HubFxError in StorageServer)
// ============================================================================

namespace FlashError {
    constexpr uint8_t OK              = 0;
    constexpr uint8_t NOT_INITIALIZED = 1;
    constexpr uint8_t NOT_FOUND       = 2;
    constexpr uint8_t IO_ERROR        = 3;
    constexpr uint8_t IS_DIRECTORY    = 4;
    constexpr uint8_t ALREADY_EXISTS  = 5;
    constexpr uint8_t LIMIT_EXCEEDED  = 6;  ///< Tree depth or entry count limit hit (result truncated)
}


// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief Flash storage information
 *
 * Reports LittleFS capacity in bytes (flash is typically < 2 MB,
 * so byte-level granularity is more useful than MB).
 */
struct FlashStorageInfo {
    bool initialized;
    uint32_t totalBytes;
    uint32_t usedBytes;
    uint32_t freeBytes;
};


// ============================================================================
// FlashModule
// ============================================================================

class FlashModule {
public:
    /// File handle type (matches SdCardModuleT convention for template bridges)
    using FileHandle = StorageFile;

    /// Get the singleton instance
    static FlashModule& instance() {
        static FlashModule inst;
        return inst;
    }

    // Delete copy/move
    FlashModule(const FlashModule&) = delete;
    FlashModule& operator=(const FlashModule&) = delete;
    FlashModule(FlashModule&&) = delete;
    FlashModule& operator=(FlashModule&&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize LittleFS flash file system
     * @return true on success
     */
    bool begin();

    /// Check if flash is initialized and ready
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
     * @return FlashError code
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
     * @return FlashError code
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
     * @return FlashError code
     */
    uint8_t getFileInfo(const char* path, FileEntry& entry);

    /**
     * @brief Get flash storage information
     * @param info Output storage info
     * @return FlashError code
     */
    uint8_t getStorageInfo(FlashStorageInfo& info);

    // ========================================================================
    // File Modification (thread-safe, lock acquired internally)
    // ========================================================================

    /**
     * @brief Remove a file
     * @param path File path
     * @return FlashError code
     */
    uint8_t removeFile(const char* path);

    /**
     * @brief Remove a directory
     * @param path Directory path
     * @param recursive If true, delete all contents; if false, fail on non-empty dir
     * @return FlashError code
     */
    uint8_t removeDirectory(const char* path, bool recursive = true);

    /**
     * @brief Create directory
     * @param path Directory path
     * @param createParents If true, create missing parents (mkdir -p semantics, idempotent)
     * @return FlashError code
     */
    uint8_t makeDirectory(const char* path, bool createParents = false);

    // ========================================================================
    // File I/O (caller MUST hold lock via lock()/unlock())
    // ========================================================================

    /**
     * @brief Open file for reading
     *
     * Caller MUST lock() before and unlock() after all file operations.
     *
     * @param path File path
     * @param file Output file handle (StorageFile)
     * @return FlashError code
     */
    uint8_t openRead(const char* path, StorageFile& file);

    /**
     * @brief Open file for writing
     *
     * Caller MUST lock() before and unlock() after all file operations.
     *
     * @param path File path
     * @param file Output file handle (StorageFile)
     * @param truncate If true, truncate existing file
     * @return FlashError code
     */
    uint8_t openWrite(const char* path, StorageFile& file, bool truncate = true);

    // ========================================================================
    // Mutex Access
    // ========================================================================

    void lock()     { sfxMutexLock(_flashMutex); }
    bool tryLock()  { return sfxMutexTryLock(_flashMutex); }
    void unlock()   { sfxMutexUnlock(_flashMutex); }

    /// Remove a file WITHOUT acquiring the flash lock — for callers that
    /// already hold it (storage server upload-cancel cleanup).  Mirrors
    /// `SdCardModule::policy().removeFile()` on the SD side.  Returns
    /// true on success.
    bool removeFileNoLock(const char* path);

private:
    FlashModule();

    bool _initialized;
    SfxMutex _flashMutex;

    // Internal recursive tree listing (caller holds lock)
    void listTreeRecursive(const char* path, int depth,
                           std::function<bool(const FileEntry&, int)>& callback,
                           bool& shouldContinue, int& entryCount,
                           bool& limitHit);

    // Internal recursive directory removal (caller holds lock)
    bool removeDirectoryRecursive(const char* path, int depth = 0);
};

#endif // FLASH_H
