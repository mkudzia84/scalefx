/*
 * SD Card Module — Policy-Based Template for SD Card File Operations
 *
 * SdCardModuleT<TPolicy> provides thread-safe SD card initialization and
 * file operations. The bus mode (SPI, SDIO 1-bit, SDIO 4-bit) is selected
 * at compile time via a policy class that encapsulates all platform-specific
 * and bus-specific behavior.
 *
 * Each policy defines a nested Config type that captures the pin assignments
 * and driver parameters for that bus mode:
 *
 *   PicoSpiSdPolicy::Config        — SPI on Pico (SdFat backend)
 *   EspIdfSdio4BitPolicy::Config   — 4-bit SDIO on ESP32 via ESP-IDF native
 *                                    (esp_vfs_fat_sdmmc_mount + POSIX I/O)
 *
 * The correct policy is auto-selected at the bottom of this header based on
 * the target platform, and aliased as `SdCardModule` for transparent usage.
 *
 * Usage:
 *   SdCardModule& sd = SdCardModule::instance();
 *   SdCardModule::Config cfg { .clk = 7, .cmd = 9, .d0 = 8 };
 *   sd.begin(cfg);
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

#include "platform/sfx_platform.h"
#include <functional>
#include <vector>
#include <string>
#include <atomic>    // std::atomic<bool> _initialized (cross-core SD_INIT vs audio read)
#include <cstring>   // strlen / strncpy / strrchr / memcpy (was transitive via <Arduino.h>)

#include "storage_types.h"

// ============================================================================
// Platform SD Backend Detection
// ============================================================================

#if SFX_PLATFORM_PICO
    #if __has_include(<SdFat.h>)
        #define SFX_SD_BACKEND_SDFAT 1
    #else
        #define SFX_SD_BACKEND_SDFAT 0
    #endif
    #define SFX_SD_BACKEND_ESP 0
#elif SFX_PLATFORM_ESP32
    #define SFX_SD_BACKEND_SDFAT 0
    #define SFX_SD_BACKEND_ESP 1
#else
    #define SFX_SD_BACKEND_SDFAT 0
    #define SFX_SD_BACKEND_ESP 0
#endif

#define SFX_HAS_SD (SFX_SD_BACKEND_SDFAT || SFX_SD_BACKEND_ESP)

#if SFX_HAS_SD


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
    constexpr uint8_t LIMIT_EXCEEDED  = 6;  ///< Tree depth or entry count limit hit
}


// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief SD card type (matches ESP32 sdcard_type_t values)
 */
enum class SdCardType : uint8_t {
    NONE    = 0,
    MMC     = 1,
    SD      = 2,
    SDHC    = 3,
    UNKNOWN = 4
};

/**
 * @brief SD card storage information
 */
struct StorageInfo {
    bool initialized;
    uint32_t cardSize_MB;
    uint32_t totalSpace_MB;
    uint32_t usedSpace_MB;        ///< Used space in MB
    uint32_t freeSpace_MB;
    uint8_t fatType;              ///< FAT16/32 (Pico/SdFat), 0 on ESP32
    uint32_t clusterSize_bytes;   ///< Cluster size (Pico/SdFat), 0 on ESP32
    SdBusMode busMode;            ///< Active bus mode
    SdCardType cardType;          ///< Card type (SD, SDHC, MMC, etc.)
};


// ============================================================================
// SdCardModuleT<TPolicy> — Policy-Based SD Card Singleton
// ============================================================================

/**
 * @brief Thread-safe SD card module parameterized by bus policy.
 *
 * TPolicy must provide:
 *
 *   // Types
 *   struct Config { ... };           // Bus-specific pin/driver configuration
 *   using FileHandle = ...;          // SdFat::File32 (Pico) / NativeFile (ESP32 IDF)
 *   static constexpr SdBusMode BUS_MODE = ...;
 *
 *   // Lifecycle
 *   bool mount(const Config& cfg);   // Platform-specific mount
 *   void unmount();                  // Platform-specific unmount
 *
 *   // Card queries
 *   SdCardType cardType();
 *   void fillStorageInfo(StorageInfo& info);
 *
 *   // Filesystem primitives
 *   FileHandle openDir(const char* path);
 *   FileHandle openReadFile(const char* path);
 *   FileHandle openWriteFile(const char* path, bool truncate);
 *   static FileHandle nextFile(FileHandle& dir);
 *   bool exists(const char* path);
 *   bool removeFile(const char* path);
 *   bool makeDir(const char* path);
 *   bool removeDir(const char* path);
 *
 *   // File handle inspection
 *   static bool isValid(const FileHandle& f);
 *   static bool isDirectory(FileHandle& f);
 *   static uint32_t fileSize(FileHandle& f);
 *   static void closeFile(FileHandle& f);
 *   static void extractName(FileHandle& f, char* buf, size_t len);
 *
 * @tparam TPolicy  Bus/platform policy (PicoSpiSdPolicy, EspSdio1BitPolicy, etc.)
 */
template <typename TPolicy>
class SdCardModuleT {
public:
    using Config     = typename TPolicy::Config;
    using FileHandle = typename TPolicy::FileHandle;
    static constexpr SdBusMode BUS_MODE = TPolicy::BUS_MODE;

    /// Get the singleton instance
    static SdCardModuleT& instance() {
        static SdCardModuleT inst;
        return inst;
    }

    // Delete copy/move
    SdCardModuleT(const SdCardModuleT&) = delete;
    SdCardModuleT& operator=(const SdCardModuleT&) = delete;
    SdCardModuleT(SdCardModuleT&&) = delete;
    SdCardModuleT& operator=(SdCardModuleT&&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Initialize the SD card with bus-specific configuration
     *
     * @param cfg  Policy-defined configuration (pin assignments, speed, etc.)
     * @return true on success
     */
    bool begin(const Config& cfg);

    /**
     * @brief Retry initialization with stored configuration
     *
     * Unmounts first, then re-mounts with the same Config passed to begin().
     * To change parameters (e.g., SPI speed), modify config() before calling.
     *
     * @return true on success
     */
    bool retryInit();

    /// Check if SD card is initialized and ready
    bool isInitialized() const { return _initialized; }

    /// Get active bus mode (compile-time constant)
    SdBusMode busMode() const { return BUS_MODE; }

    /// Get card type (only valid when initialized)
    SdCardType cardType();

    /// Mount epoch — changes on every (un)mount. A consumer holding a FILE* open
    /// across reads captures this at open() and aborts if it changes (the remount
    /// invalidated its handle). See `_mountEpoch`.
    uint32_t mountEpoch() const { return _mountEpoch.load(std::memory_order_acquire); }

    /**
     * @brief Unmount the SD card
     *
     * Must be called before re-mounting (hot-swap or error recovery).
     * Some backends (e.g., SD_MMC) are no-ops if already mounted.
     */
    void unmount();

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
     * @brief Remove a directory
     * @param path Directory path
     * @param recursive If true, delete all contents; if false, fail on non-empty dir
     * @return SdError code
     */
    uint8_t removeDirectory(const char* path, bool recursive = true);

    /**
     * @brief Create directory
     * @param path Directory path
     * @param createParents If true, create missing parents (mkdir -p semantics, idempotent)
     * @return SdError code
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
     * @param file Output file handle
     * @return SdError code
     */
    uint8_t openRead(const char* path, FileHandle& file);

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
    uint8_t openWrite(const char* path, FileHandle& file, bool truncate = true);

    // ========================================================================
    // Mutex Access
    // ========================================================================

    void lock()     { sfxMutexLock(_sdMutex); }
    bool tryLock()  { return sfxMutexTryLock(_sdMutex); }
    void unlock()   { sfxMutexUnlock(_sdMutex); }

    // ========================================================================
    // Policy & Config Access
    // ========================================================================

    /// Access the policy for platform-specific API (e.g., rawFS(), rawSD())
    TPolicy&       policy()       { return _policy; }
    const TPolicy& policy() const { return _policy; }

    /// Access stored config (modify before retryInit() to change parameters)
    Config&       config()       { return _config; }
    const Config& config() const { return _config; }

private:
    SdCardModuleT() { sfxMutexInit(_sdMutex); }

    TPolicy _policy;
    // Cross-core (Rule 15): written by begin()/unmount() on Core 0 (SD_INIT),
    // read by WavStreamSource::open() on the Core-1 audio task. atomic<bool> so
    // the cross-core read has defined ordering (existing `if (!_initialized)`
    // sites keep working via the implicit seq_cst load).
    std::atomic<bool> _initialized{false};

    // Mount epoch — bumped on every (un)mount. A long-lived consumer that holds
    // an open FILE* across many reads (audio WavStreamSource, the asset loader)
    // captures the epoch at open() and aborts the moment it changes, so a
    // concurrent SD_INIT remount can't have it read through a stale/recycled VFS
    // handle (use-after-unmount). Cross-core read on the audio task.
    std::atomic<uint32_t> _mountEpoch{0};
    SfxMutex _sdMutex;
    Config _config{};

    // Internal recursive tree listing (caller holds lock)
    void listTreeRecursive(const char* path, int depth,
                           std::function<bool(const FileEntry&, int)>& callback,
                           bool& shouldContinue, int& entryCount,
                           bool& limitHit);

    // Internal recursive directory removal (caller holds lock)
    bool removeDirectoryRecursive(const char* path, int depth = 0);
};


// ============================================================================
// Template Implementation
// ============================================================================

#include "sd_card.ipp"


// ============================================================================
// Platform Policy Selection + SdCardModule Alias
// ============================================================================

#if SFX_PLATFORM_PICO
    #include "pico/pico_sd_policy.h"
    /// Pico: SPI with SdFat backend
    using SdCardModule = SdCardModuleT<PicoSpiSdPolicy>;
#elif SFX_PLATFORM_ESP32
    /// ESP32: SDIO 4-bit via ESP-IDF native (`esp_vfs_fat_sdmmc_mount` +
    /// `<driver/sdmmc_host.h>` + POSIX `fopen`/`fread`/...).  Task- and
    /// core-safe by construction via VFS-FAT's `FF_FS_REENTRANT` mutex.
    #include "esp32/esp_idf_sd_policy.h"
    using SdCardModule = SdCardModuleT<EspIdfSdio4BitPolicy>;
#endif

/// File handle type alias for external use (AudioMixer).
///
/// Guarded to ESP32 because SdFat (used on Pico) ships its own `class SdFile`
/// which collides with this alias at template instantiation. Pico controllers
/// don't use AudioMixer, so the alias is only needed on ESP32.
#if SFX_PLATFORM_ESP32
using SdFile = SdCardModule::FileHandle;
#endif


#endif // SFX_HAS_SD
#endif // SD_CARD_H
