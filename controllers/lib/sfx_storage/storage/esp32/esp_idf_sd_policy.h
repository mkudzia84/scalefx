/*
 * EspIdfSdio4BitPolicy — SD policy backed by ESP-IDF native APIs.
 *
 * Replaces the Arduino `SD_MMC.*` shim with direct ESP-IDF calls:
 *   - `sdmmc_host_t` + `sdmmc_slot_config_t` describe the hardware
 *   - `esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card)`
 *     mounts FATFS via VFS at "/sdcard"
 *   - All file ops route through POSIX (`fopen`/`fread`/...) wrapped
 *     by `NativeFile`.
 *
 * Why this exists
 * ───────────────
 *   Arduino-ESP32's `fs::File` is a thin shared-ptr wrapper around a
 *   FILE* that introduces non-deterministic lifetime semantics; the
 *   SD_MMC layer assumes single-task usage and has surfaced cross-core
 *   wedges on HubFX.  Going through esp_vfs_fat_sdmmc_mount directly +
 *   POSIX file ops side-steps both issues:
 *
 *   - VFS-FAT has internal FreeRTOS mutexes (`FF_FS_REENTRANT=1` in the
 *     default sdkconfig) — concurrent fopen/fread/fwrite from Core 0
 *     and Core 1 is safe.
 *   - POSIX FILE* has well-defined ownership: fclose is idempotent,
 *     no shared state, no surprise refcount tear-down.
 *
 * Policy contract (preserved verbatim — drop-in for `SdCardModuleT<>`)
 * ───────────────────────────────────────────────────────────────────
 *   struct Config { ... };
 *   using FileHandle = NativeFile;
 *   static constexpr SdBusMode BUS_MODE = SdBusMode::SDIO_4BIT;
 *
 *   bool mount(const Config& cfg);
 *   void unmount();
 *
 *   SdCardType cardType();
 *   void fillStorageInfo(StorageInfo& info);
 *
 *   FileHandle openDir(const char* path);
 *   FileHandle openReadFile(const char* path);
 *   FileHandle openWriteFile(const char* path, bool truncate);
 *   static FileHandle nextFile(FileHandle& dir);
 *   bool exists(const char* path);
 *   bool removeFile(const char* path);
 *   bool makeDir(const char* path);
 *   bool removeDir(const char* path);
 *
 *   static bool isValid(const FileHandle& f);
 *   static bool isDirectory(FileHandle& f);
 *   static uint32_t fileSize(FileHandle& f);
 *   static void closeFile(FileHandle& f);
 *   static void extractName(FileHandle& f, char* buf, size_t len);
 *
 * Mount path: "/sdcard" — used both as the VFS base path and prefixed
 * onto every NativeFile open call.  Don't change without coordinating
 * with NativeFile's `_basePath` capacity (16 bytes incl. NUL).
 */

#ifndef ESP_IDF_SD_POLICY_H
#define ESP_IDF_SD_POLICY_H

#include "platform/sfx_platform.h"

#if SFX_PLATFORM_ESP32

#include <cstdint>
#include <cstddef>
#include "native_file.h"

// Forward decls — sd_card.h defines these and includes this header at
// the bottom, but a .cpp TU that includes this header in isolation
// (with sd_card.h pulled in next) needs these visible up front.
enum class SdBusMode  : uint8_t;
enum class SdCardType : uint8_t;
struct     StorageInfo;

// IDF's `sdmmc_card_t` is a typedef'd anonymous struct in
// `<sdmmc_types.h>` — can't forward-declare it as `struct sdmmc_card_t;`.
// Hold a `void*` here and cast to `sdmmc_card_t*` inside the .cpp where
// the full type is known.


// ─── Policy ────────────────────────────────────────────────────────────

struct EspIdfSdio4BitPolicy {

    struct Config {
        int8_t  clk = -1;             ///< SDIO clock pin (-1 = ESP-IDF default)
        int8_t  cmd = -1;             ///< SDIO command pin
        int8_t  d0  = -1;             ///< SDIO data 0 (required)
        int8_t  d1  = -1;             ///< SDIO data 1 (required for 4-bit)
        int8_t  d2  = -1;             ///< SDIO data 2
        int8_t  d3  = -1;             ///< SDIO data 3
        bool    formatIfFailed = true;
        uint8_t maxOpenFiles   = 5;
        uint8_t speed_mhz      = 0;   ///< 0 = SDMMC_FREQ_DEFAULT (20 MHz);
                                      ///  set to 40 for SDMMC_FREQ_HIGHSPEED.
    };

    using FileHandle = NativeFile;
    static constexpr SdBusMode BUS_MODE = SdBusMode::SDIO_4BIT;

    // ── Lifecycle ─────────────────────────────────────────────────────
    bool mount(const Config& cfg);
    void unmount();

    // ── Card / storage queries ───────────────────────────────────────
    SdCardType cardType();
    void       fillStorageInfo(StorageInfo& info);

    // ── Filesystem primitives ────────────────────────────────────────
    FileHandle openDir(const char* path) {
        return NativeFile::openDir(kMountPoint, path);
    }
    FileHandle openReadFile(const char* path) {
        return NativeFile::openReadFile(kMountPoint, path);
    }
    FileHandle openWriteFile(const char* path, bool truncate) {
        return NativeFile::openWriteFile(kMountPoint, path, truncate);
    }
    static FileHandle nextFile(FileHandle& dir) {
        return dir.openNextFile();
    }

    bool exists(const char* path);
    bool removeFile(const char* path);
    bool makeDir(const char* path);
    bool removeDir(const char* path);

    // ── File handle inspection (matches policy contract) ─────────────
    static bool     isValid(const FileHandle& f)   { return (bool)f; }
    static bool     isDirectory(FileHandle& f)     { return f.isDirectory(); }
    static uint32_t fileSize(FileHandle& f)        { return (uint32_t)f.size(); }
    static void     closeFile(FileHandle& f)       { f.close(); }
    static void     extractName(FileHandle& f, char* buf, size_t len);

private:
    /// VFS mount point.  Must match NativeFile's `_basePath` capacity
    /// (16 bytes incl. NUL) — "/sdcard" is 7 chars + NUL.
    static constexpr const char* kMountPoint = "/sdcard";

    /// Opaque sdmmc_card_t* — declared `void*` so the header doesn't
    /// have to include `<sdmmc_types.h>`.  Cast inside the .cpp.
    void*  _card    = nullptr;
    bool   _mounted = false;
};


#endif  // SFX_PLATFORM_ESP32
#endif  // ESP_IDF_SD_POLICY_H
