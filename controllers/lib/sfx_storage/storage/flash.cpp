/*
 * Flash Module — Implementation
 *
 * Thread-safe LittleFS operations using platform-abstracted mutex
 * for multi-core safety.  No Serial output — all results communicated
 * through return values and callbacks. Protocol output is handled by
 * StorageServer using StreamWriter.
 *
 * Two backends, one surface
 * ─────────────────────────
 *   RP2040/RP2350 (Arduino-Pico):
 *     - Arduino LittleFS library (`LittleFS.*`, `Dir`, `File`)
 *     - Per-call locking provided by FlashModule's own `_flashMutex`
 *
 *   ESP32 (Arduino-ESP32 v3.x):
 *     - `esp_vfs_littlefs_register` mounts LittleFS via VFS at "/littlefs"
 *     - All file ops route through POSIX (`fopen`/`fread`/`fwrite`/
 *       `opendir`/`readdir`/`mkdir`/`unlink`/`stat`) wrapped by NativeFile
 *     - VFS provides internal task-safety; the FlashModule mutex remains
 *       for mount/unmount and upload-exclusivity gating
 *
 *   The wire-side API (StorageFile, list/info/remove/mkdir/open/lock) is
 *   identical on both platforms — switch happens behind the StorageFile
 *   alias in flash.h.
 */

#if defined(SFX_HAS_STORAGE)

#include "flash.h"
#include <vector>
#include <string>

#if defined(ARDUINO_ARCH_ESP32)
    #include <esp_littlefs.h>
    #include <sys/stat.h>
    #include <dirent.h>
    #include <cstring>
    #include <cstdio>
    #include <unistd.h>
#endif


// ============================================================================
// Constants (ESP32 only)
// ============================================================================
#if defined(ARDUINO_ARCH_ESP32)
namespace {
constexpr const char* kFlashBase      = "/littlefs";   ///< VFS mount path
constexpr const char* kFlashPartition = "littlefs";    ///< partition label (matches partitions.csv)

/// Join "/littlefs" + user path into out (size 192).
inline void joinFlash(char* out, size_t outSize, const char* path) {
    if (!path) path = "";
    const bool needsSlash = (path[0] != '/');
    snprintf(out, outSize, "%s%s%s", kFlashBase, needsSlash ? "/" : "", path);
}
}  // namespace
#endif


// ============================================================================
// Constructor
// ============================================================================

FlashModule::FlashModule()
    : _initialized(false)
{
    sfxMutexInit(_flashMutex);
}


// ============================================================================
// Lifecycle
// ============================================================================

bool FlashModule::begin() {
    lock();
#if defined(ARDUINO_ARCH_ESP32)
    // esp_vfs_littlefs_register — registers LittleFS as a VFS at
    // `base_path` against the partition with the given label (must
    // match partitions.csv: `littlefs, data, spiffs, ...`).
    //   - format_if_mount_failed=true → first-boot auto-format
    //   - dont_mount=false           → mount immediately
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = kFlashBase;
    conf.partition_label        = kFlashPartition;
    conf.format_if_mount_failed = true;
    conf.dont_mount             = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    // ESP_OK on first mount; ESP_ERR_INVALID_STATE if already mounted —
    // treat that as success (idempotent begin).
    bool ok = (err == ESP_OK) || (err == ESP_ERR_INVALID_STATE);
#else
    bool ok = LittleFS.begin();
#endif
    unlock();

    _initialized = ok;
    return ok;
}


// ============================================================================
// Directory Operations
// ============================================================================

uint8_t FlashModule::listDirectory(const char* path,
                                    std::function<bool(const FileEntry&)> callback) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();
    int entryCount = 0;
    bool limitHit = false;

#if defined(ARDUINO_ARCH_RP2040)
    // Arduino-Pico: Dir iterator API
    Dir dir = LittleFS.openDir(path);

    FileEntry entry;
    while (dir.next()) {
        if (++entryCount > MAX_TREE_ENTRIES) {
            limitHit = true;
            break;
        }

        strncpy(entry.name, dir.fileName().c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.isDirectory = dir.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)dir.fileSize();

        if (!callback(entry)) break;
    }
#elif defined(ARDUINO_ARCH_ESP32)
    // ESP-IDF native: POSIX opendir/readdir/stat via NativeFile
    NativeFile dir = NativeFile::openDir(kFlashBase, path);
    if (dir && dir.isDirectory()) {
        FileEntry entry;
        NativeFile f = dir.openNextFile();
        while (f) {
            if (++entryCount > MAX_TREE_ENTRIES) {
                f.close();
                limitHit = true;
                break;
            }
            const char* name = f.name();
            strncpy(entry.name, name, sizeof(entry.name) - 1);
            entry.name[sizeof(entry.name) - 1] = '\0';
            entry.isDirectory = f.isDirectory();
            entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
            f.close();

            if (!callback(entry)) break;
            f = dir.openNextFile();
        }
        dir.close();
    }
#endif

    unlock();
    return limitHit ? FlashError::LIMIT_EXCEEDED : FlashError::OK;
}

uint8_t FlashModule::listTree(const char* path,
                               std::function<bool(const FileEntry&, int)> callback) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();
    bool shouldContinue = true;
    int entryCount = 0;
    bool limitHit = false;
    listTreeRecursive(path, 0, callback, shouldContinue, entryCount, limitHit);
    unlock();
    return limitHit ? FlashError::LIMIT_EXCEEDED : FlashError::OK;
}

void FlashModule::listTreeRecursive(const char* path, int depth,
                                     std::function<bool(const FileEntry&, int)>& callback,
                                     bool& shouldContinue, int& entryCount,
                                     bool& limitHit) {
    if (!shouldContinue) return;

    // Safety: prevent infinite recursion on corrupted directory refs
    if (depth >= MAX_TREE_DEPTH) {
        limitHit = true;
        shouldContinue = false;
        return;
    }

#if defined(ARDUINO_ARCH_RP2040)
    // Arduino-Pico: Dir iterator API
    Dir dir = LittleFS.openDir(path);

    FileEntry entry;
    while (shouldContinue && dir.next()) {
        if (++entryCount > MAX_TREE_ENTRIES) {
            limitHit = true;
            shouldContinue = false;
            break;
        }

        strncpy(entry.name, dir.fileName().c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.isDirectory = dir.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)dir.fileSize();

        if (!callback(entry, depth)) {
            shouldContinue = false;
            break;
        }

        if (entry.isDirectory) {
            char fullPath[192];
            size_t pathLen = strlen(path);
            const char* sep = (pathLen > 0 && path[pathLen - 1] == '/') ? "" : "/";
            snprintf(fullPath, sizeof(fullPath), "%s%s%s", path, sep, entry.name);
            listTreeRecursive(fullPath, depth + 1, callback, shouldContinue,
                              entryCount, limitHit);
        }
    }
#elif defined(ARDUINO_ARCH_ESP32)
    // ESP-IDF native: walk via NativeFile dir iteration
    NativeFile dir = NativeFile::openDir(kFlashBase, path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    FileEntry entry;
    NativeFile f = dir.openNextFile();
    while (shouldContinue && f) {
        if (++entryCount > MAX_TREE_ENTRIES) {
            f.close();
            limitHit = true;
            shouldContinue = false;
            break;
        }

        const char* name = f.name();
        strncpy(entry.name, name, sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.isDirectory = f.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
        f.close();

        if (!callback(entry, depth)) {
            shouldContinue = false;
            break;
        }

        if (entry.isDirectory) {
            char fullPath[192];
            size_t pathLen = strlen(path);
            const char* sep = (pathLen > 0 && path[pathLen - 1] == '/') ? "" : "/";
            snprintf(fullPath, sizeof(fullPath), "%s%s%s", path, sep, entry.name);
            listTreeRecursive(fullPath, depth + 1, callback, shouldContinue,
                              entryCount, limitHit);
        }

        f = dir.openNextFile();
    }
    dir.close();
#endif
}


// ============================================================================
// File Information
// ============================================================================

uint8_t FlashModule::getFileInfo(const char* path, FileEntry& entry) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();

#if defined(ARDUINO_ARCH_ESP32)
    char full[192];
    joinFlash(full, sizeof(full), path);
    struct stat st;
    if (stat(full, &st) != 0) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.isDirectory = S_ISDIR(st.st_mode);
    entry.size = entry.isDirectory ? 0 : (uint32_t)st.st_size;
#else
    StorageFile f = LittleFS.open(path, "r");
    if (!f) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.isDirectory = f.isDirectory();
    entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
    f.close();
#endif

    unlock();
    return FlashError::OK;
}

uint8_t FlashModule::getStorageInfo(FlashStorageInfo& info) {
    if (!_initialized) {
        info.initialized = false;
        return FlashError::NOT_INITIALIZED;
    }

    lock();

#if defined(ARDUINO_ARCH_RP2040)
    FSInfo fsinfo;
    LittleFS.info(fsinfo);
    info.initialized = true;
    info.totalBytes  = (uint32_t)fsinfo.totalBytes;
    info.usedBytes   = (uint32_t)fsinfo.usedBytes;
    info.freeBytes   = (uint32_t)(fsinfo.totalBytes - fsinfo.usedBytes);
#elif defined(ARDUINO_ARCH_ESP32)
    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(kFlashPartition, &total, &used);
    info.initialized = (err == ESP_OK);
    info.totalBytes  = (err == ESP_OK) ? (uint32_t)total : 0;
    info.usedBytes   = (err == ESP_OK) ? (uint32_t)used  : 0;
    info.freeBytes   = info.totalBytes > info.usedBytes
                       ? info.totalBytes - info.usedBytes : 0;
#endif

    unlock();
    return FlashError::OK;
}


// ============================================================================
// File Modification
// ============================================================================

uint8_t FlashModule::removeFile(const char* path) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();
#if defined(ARDUINO_ARCH_ESP32)
    char full[192];
    joinFlash(full, sizeof(full), path);
    struct stat st;
    if (stat(full, &st) != 0) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    bool ok = (unlink(full) == 0);
#else
    if (!LittleFS.exists(path)) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    bool ok = LittleFS.remove(path);
#endif
    unlock();
    return ok ? FlashError::OK : FlashError::IO_ERROR;
}

bool FlashModule::removeFileNoLock(const char* path) {
    // Caller already holds _flashMutex (storage-server upload cancel).
    if (!_initialized) return false;
#if defined(ARDUINO_ARCH_ESP32)
    char full[192];
    joinFlash(full, sizeof(full), path);
    return unlink(full) == 0;
#else
    return LittleFS.remove(path);
#endif
}

uint8_t FlashModule::removeDirectory(const char* path, bool recursive) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();

#if defined(ARDUINO_ARCH_ESP32)
    char full[192];
    joinFlash(full, sizeof(full), path);
    struct stat st;
    if (stat(full, &st) != 0) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    bool isDir = S_ISDIR(st.st_mode);
#else
    StorageFile f = LittleFS.open(path, "r");
    if (!f) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    bool isDir = f.isDirectory();
    f.close();
#endif

    if (!isDir) {
        // Not a directory — fall back to removeFile() semantics.
#if defined(ARDUINO_ARCH_ESP32)
        char full2[192];
        joinFlash(full2, sizeof(full2), path);
        bool ok = (unlink(full2) == 0);
#else
        bool ok = LittleFS.remove(path);
#endif
        unlock();
        return ok ? FlashError::OK : FlashError::IO_ERROR;
    }

    if (!recursive) {
        // Strict: only delete if empty.
#if defined(ARDUINO_ARCH_ESP32)
        char full2[192];
        joinFlash(full2, sizeof(full2), path);
        bool ok = (rmdir(full2) == 0);
#else
        bool ok = LittleFS.rmdir(path);
#endif
        unlock();
        return ok ? FlashError::OK : FlashError::IO_ERROR;
    }

    bool ok = removeDirectoryRecursive(path);
    unlock();
    return ok ? FlashError::OK : FlashError::IO_ERROR;
}

bool FlashModule::removeDirectoryRecursive(const char* path, int depth) {
    // Caller MUST hold lock.
    // Two-phase: snapshot child list BEFORE any deletes, then delete from
    // snapshot.  Rationale: LittleFS iterator state is invalidated by
    // remove()/rmdir() during iteration — observed on RP2040 originally,
    // applies to esp_littlefs as well.

    if (depth >= MAX_TREE_DEPTH) return false;

    struct ChildEntry {
        std::string name;
        bool        isDir;
    };
    std::vector<ChildEntry> children;

#if defined(ARDUINO_ARCH_RP2040)
    {
        Dir dir = LittleFS.openDir(path);
        while (dir.next()) {
            children.push_back({ std::string(dir.fileName().c_str()), dir.isDirectory() });
            if (children.size() >= (size_t)MAX_TREE_ENTRIES) break;
        }
    }
#elif defined(ARDUINO_ARCH_ESP32)
    {
        NativeFile dir = NativeFile::openDir(kFlashBase, path);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return false;
        }
        NativeFile f = dir.openNextFile();
        while (f) {
            children.push_back({ std::string(f.name()), f.isDirectory() });
            f.close();
            if (children.size() >= (size_t)MAX_TREE_ENTRIES) break;
            f = dir.openNextFile();
        }
        dir.close();
    }
#endif

    size_t pathLen = strlen(path);
    const char* sep = (pathLen > 0 && path[pathLen - 1] == '/') ? "" : "/";

    for (const auto& c : children) {
        char fullPath[192];
        snprintf(fullPath, sizeof(fullPath), "%s%s%s", path, sep, c.name.c_str());
        if (c.isDir) {
            if (!removeDirectoryRecursive(fullPath, depth + 1)) return false;
        } else {
#if defined(ARDUINO_ARCH_ESP32)
            char vfsFull[192];
            joinFlash(vfsFull, sizeof(vfsFull), fullPath);
            if (unlink(vfsFull) != 0) return false;
#else
            if (!LittleFS.remove(fullPath)) return false;
#endif
        }
    }

#if defined(ARDUINO_ARCH_ESP32)
    char vfsFull[192];
    joinFlash(vfsFull, sizeof(vfsFull), path);
    if (rmdir(vfsFull) == 0) return true;
    // Mirror the Pico-side idempotency: if the dir is already gone
    // (esp_littlefs collapses empty dirs on the last child remove on
    // some configs), report success.
    struct stat st;
    return stat(vfsFull, &st) != 0;
#else
    if (LittleFS.rmdir(path)) return true;
    return !LittleFS.exists(path);
#endif
}

uint8_t FlashModule::makeDirectory(const char* path, bool createParents) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();

    auto checkExistingDir = [](const char* p) -> int {
        // 1 = exists as dir, 0 = exists as file, -1 = does not exist
#if defined(ARDUINO_ARCH_ESP32)
        char full[192];
        joinFlash(full, sizeof(full), p);
        struct stat st;
        if (stat(full, &st) != 0) return -1;
        return S_ISDIR(st.st_mode) ? 1 : 0;
#else
        if (!LittleFS.exists(p)) return -1;
        StorageFile f = LittleFS.open(p, "r");
        bool isDir = f && f.isDirectory();
        if (f) f.close();
        return isDir ? 1 : 0;
#endif
    };

    auto doMkdir = [](const char* p) -> bool {
#if defined(ARDUINO_ARCH_ESP32)
        char full[192];
        joinFlash(full, sizeof(full), p);
        return mkdir(full, 0775) == 0;
#else
        return LittleFS.mkdir(p);
#endif
    };

    if (!createParents) {
        int existing = checkExistingDir(path);
        if (existing == 1) { unlock(); return FlashError::OK; }
        if (existing == 0) { unlock(); return FlashError::ALREADY_EXISTS; }

        bool ok = doMkdir(path);
        unlock();
        return ok ? FlashError::OK : FlashError::IO_ERROR;
    }

    // PARENTS mode: walk path components, mkdir each. Idempotent.
    char buf[192];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) { unlock(); return FlashError::IO_ERROR; }
    memcpy(buf, path, n + 1);

    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (i > 1) {
                int existing = checkExistingDir(buf);
                if (existing == 0) { unlock(); return FlashError::ALREADY_EXISTS; }
                if (existing == -1) {
                    if (!doMkdir(buf)) { unlock(); return FlashError::IO_ERROR; }
                }
            }
            buf[i] = saved;
        }
    }
    unlock();
    return FlashError::OK;
}


// ============================================================================
// File I/O (caller must hold lock)
// ============================================================================

uint8_t FlashModule::openRead(const char* path, StorageFile& file) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

#if defined(ARDUINO_ARCH_ESP32)
    file = NativeFile::openReadFile(kFlashBase, path);
    if (!file) return FlashError::NOT_FOUND;
    if (file.isDirectory()) {
        file.close();
        return FlashError::IS_DIRECTORY;
    }
    return FlashError::OK;
#else
    file = LittleFS.open(path, "r");
    if (!file) return FlashError::NOT_FOUND;
    if (file.isDirectory()) {
        file.close();
        return FlashError::IS_DIRECTORY;
    }
    return FlashError::OK;
#endif
}

uint8_t FlashModule::openWrite(const char* path, StorageFile& file, bool truncate) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

#if defined(ARDUINO_ARCH_ESP32)
    file = NativeFile::openWriteFile(kFlashBase, path, truncate);
    if (!file) return FlashError::IO_ERROR;
    return FlashError::OK;
#else
    // LittleFS: "w" = write+truncate+create, "a" = append+create
    const char* mode = truncate ? "w" : "a";
    file = LittleFS.open(path, mode);
    if (!file) return FlashError::IO_ERROR;
    return FlashError::OK;
#endif
}

#endif  // SFX_HAS_STORAGE
