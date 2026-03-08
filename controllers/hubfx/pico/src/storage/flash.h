/*
 * Flash Module — Thread-Safe LittleFS File Operations
 *
 * Singleton class for onboard flash (LittleFS) initialization and
 * file operations. Provides thread-safe access via pico mutex for
 * multi-core safety.
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
 *   LFSFile file;
 *   flash.openRead("/config.yaml", file);
 *   int n = file.read(buf, sizeof(buf));
 *   file.close();
 *   flash.unlock();
 */

#ifndef FLASH_H
#define FLASH_H

#include <Arduino.h>
#include <LittleFS.h>
#include <pico/mutex.h>
#include <functional>

#include "storage_types.h"

// Use LittleFS File type to avoid ambiguity with SdFat File
using LFSFile = ::File;


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
     * @brief Create directory (recursive)
     * @param path Directory path
     * @return FlashError code
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
     * @param file Output file handle (LFSFile)
     * @return FlashError code
     */
    uint8_t openRead(const char* path, LFSFile& file);

    /**
     * @brief Open file for writing
     *
     * Caller MUST lock() before and unlock() after all file operations.
     *
     * @param path File path
     * @param file Output file handle (LFSFile)
     * @param truncate If true, truncate existing file
     * @return FlashError code
     */
    uint8_t openWrite(const char* path, LFSFile& file, bool truncate = true);

    // ========================================================================
    // Mutex Access
    // ========================================================================

    void lock()     { mutex_enter_blocking(&_flashMutex); }
    bool tryLock()  { return mutex_try_enter(&_flashMutex, nullptr); }
    void unlock()   { mutex_exit(&_flashMutex); }

    /// Direct LittleFS access (caller must hold lock)
    FS& getFS()     { return LittleFS; }

private:
    FlashModule();

    bool _initialized;
    mutex_t _flashMutex;

    // Internal recursive tree listing (caller holds lock)
    void listTreeRecursive(const char* path, int depth,
                           std::function<bool(const FileEntry&, int)>& callback,
                           bool& shouldContinue);
};

#endif // FLASH_H
