/*
 * Flash Module — Implementation
 *
 * Thread-safe LittleFS operations using platform-abstracted mutex
 * for multi-core safety.
 * No Serial output — all results communicated through return values
 * and callbacks. Protocol output handled by StorageServer.
 *
 * LittleFS API differences between platforms:
 *   RP2040/RP2350 (Arduino-Pico):
 *     - Directory listing uses Dir iterator (openDir/next)
 *     - Storage info via FSInfo struct (totalBytes/usedBytes)
 *   ESP32 (Arduino-ESP32):
 *     - Directory listing uses File.openNextFile() pattern
 *     - Storage info via LittleFS.totalBytes()/usedBytes() methods
 *
 * Common across platforms:
 *   - File type is ::File (aliased LFSFile)
 *   - Open modes are "r"/"w"/"a" strings
 *   - mkdir() creates path recursively by default
 */

#if defined(SFX_HAS_STORAGE)

#include "flash.h"
#include <vector>


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
    // ESP32 LittleFS.begin(formatOnFail, basePath, maxOpenFiles, partitionLabel)
    //   - formatOnFail=true: auto-format on first boot (no pre-existing filesystem)
    //   - partitionLabel="littlefs": select the correct partition when multiple
    //     spiffs-subtype partitions exist (e.g., "littlefs" + "spiffs" for WAV)
    bool ok = LittleFS.begin(true, "/littlefs", 10, "littlefs");
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
    // Arduino-ESP32: File.openNextFile() pattern
    File dir = LittleFS.open(path);
    if (dir && dir.isDirectory()) {
        FileEntry entry;
        File f = dir.openNextFile();
        while (f) {
            if (++entryCount > MAX_TREE_ENTRIES) {
                f.close();
                limitHit = true;
                break;
            }

            const char* name = f.name();
            // ESP32 File.name() may return full path — extract filename
            const char* slash = strrchr(name, '/');
            const char* baseName = slash ? slash + 1 : name;
            strncpy(entry.name, baseName, sizeof(entry.name) - 1);
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
    // Arduino-ESP32: File.openNextFile() pattern
    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    FileEntry entry;
    File f = dir.openNextFile();
    while (shouldContinue && f) {
        if (++entryCount > MAX_TREE_ENTRIES) {
            f.close();
            limitHit = true;
            shouldContinue = false;
            break;
        }

        const char* name = f.name();
        const char* slash = strrchr(name, '/');
        const char* baseName = slash ? slash + 1 : name;
        strncpy(entry.name, baseName, sizeof(entry.name) - 1);
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
    LFSFile f = LittleFS.open(path, "r");
    if (!f) {
        unlock();
        return FlashError::NOT_FOUND;
    }

    // Extract just the filename from the path for the entry
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';

    entry.isDirectory = f.isDirectory();
    entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
    f.close();
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
    // Arduino-Pico: FSInfo struct
    FSInfo fsinfo;
    LittleFS.info(fsinfo);

    info.initialized = true;
    info.totalBytes  = (uint32_t)fsinfo.totalBytes;
    info.usedBytes   = (uint32_t)fsinfo.usedBytes;
    info.freeBytes   = (uint32_t)(fsinfo.totalBytes - fsinfo.usedBytes);
#elif defined(ARDUINO_ARCH_ESP32)
    // Arduino-ESP32: direct methods
    info.initialized = true;
    info.totalBytes  = (uint32_t)LittleFS.totalBytes();
    info.usedBytes   = (uint32_t)LittleFS.usedBytes();
    info.freeBytes   = info.totalBytes - info.usedBytes;
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
    if (!LittleFS.exists(path)) {
        unlock();
        return FlashError::NOT_FOUND;
    }

    bool ok = LittleFS.remove(path);
    unlock();
    return ok ? FlashError::OK : FlashError::IO_ERROR;
}

uint8_t FlashModule::removeDirectory(const char* path, bool recursive) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();

    // Check path exists and is a directory
    LFSFile f = LittleFS.open(path, "r");
    if (!f) {
        unlock();
        return FlashError::NOT_FOUND;
    }
    bool isDir = f.isDirectory();
    f.close();

    if (!isDir) {
        unlock();
        // Not a directory — use removeFile() instead
        bool ok = LittleFS.remove(path);
        return ok ? FlashError::OK : FlashError::IO_ERROR;
    }

    if (!recursive) {
        // Strict: only delete if empty. rmdir returns false on non-empty.
        bool ok = LittleFS.rmdir(path);
        unlock();
        return ok ? FlashError::OK : FlashError::IO_ERROR;
    }

    bool ok = removeDirectoryRecursive(path);
    unlock();
    return ok ? FlashError::OK : FlashError::IO_ERROR;
}

bool FlashModule::removeDirectoryRecursive(const char* path, int depth) {
    // Caller MUST hold lock.
    // Two-phase: snapshot child list BEFORE any deletes, then delete from snapshot.
    // Rationale: LittleFS Dir/File iterators are invalidated by remove()/rmdir() on
    // the directory being iterated — observed on RP2040. Deleting during iteration
    // silently truncated enumeration and left children behind, causing the final
    // LittleFS.rmdir(path) to fail on a non-empty dir.

    if (depth >= MAX_TREE_DEPTH) return false;

    struct ChildEntry {
        String name;
        bool   isDir;
    };
    std::vector<ChildEntry> children;

#if defined(ARDUINO_ARCH_RP2040)
    {
        Dir dir = LittleFS.openDir(path);
        while (dir.next()) {
            children.push_back({ dir.fileName(), dir.isDirectory() });
            if (children.size() >= (size_t)MAX_TREE_ENTRIES) break;
        }
    }
#elif defined(ARDUINO_ARCH_ESP32)
    {
        File dir = LittleFS.open(path);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return false;
        }
        File f = dir.openNextFile();
        while (f) {
            // ESP32 LittleFS: f.name() is bare leaf; f.path() is full path.
            String leaf = String(f.name());
            int slash = leaf.lastIndexOf('/');
            if (slash >= 0) leaf = leaf.substring(slash + 1);
            children.push_back({ leaf, f.isDirectory() });
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
            if (!LittleFS.remove(fullPath)) return false;
        }
    }

    // Arduino-Pico LittleFS implicitly collapses an empty directory entry when
    // its last child is removed — a subsequent LittleFS.rmdir() then reports
    // false (path missing). Treat "already gone" as success for idempotency.
    if (LittleFS.rmdir(path)) return true;
    return !LittleFS.exists(path);
}

uint8_t FlashModule::makeDirectory(const char* path, bool createParents) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();

    auto checkExistingDir = [](const char* p) -> int {
        // 1 = exists as dir, 0 = exists as file, -1 = does not exist
        if (!LittleFS.exists(p)) return -1;
        LFSFile f = LittleFS.open(p, "r");
        bool isDir = f && f.isDirectory();
        if (f) f.close();
        return isDir ? 1 : 0;
    };

    if (!createParents) {
        int existing = checkExistingDir(path);
        if (existing == 1) { unlock(); return FlashError::OK; }  // idempotent: dir exists
        if (existing == 0) { unlock(); return FlashError::ALREADY_EXISTS; }  // exists as file

        bool ok = LittleFS.mkdir(path);
        unlock();
        return ok ? FlashError::OK : FlashError::IO_ERROR;
    }

    // PARENTS mode: walk path components, mkdir each in turn. Idempotent.
    char buf[192];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) { unlock(); return FlashError::IO_ERROR; }
    memcpy(buf, path, n + 1);

    // Iterate each '/' boundary; skip leading '/'
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (i > 1) {  // skip the bare "/" root
                int existing = checkExistingDir(buf);
                if (existing == 0) { unlock(); return FlashError::ALREADY_EXISTS; }
                if (existing == -1) {
                    if (!LittleFS.mkdir(buf)) { unlock(); return FlashError::IO_ERROR; }
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

uint8_t FlashModule::openRead(const char* path, LFSFile& file) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    file = LittleFS.open(path, "r");
    if (!file) return FlashError::NOT_FOUND;

    if (file.isDirectory()) {
        file.close();
        return FlashError::IS_DIRECTORY;
    }
    return FlashError::OK;
}

uint8_t FlashModule::openWrite(const char* path, LFSFile& file, bool truncate) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    // LittleFS: "w" = write+truncate+create, "a" = append+create
    const char* mode = truncate ? "w" : "a";

    file = LittleFS.open(path, mode);
    if (!file) return FlashError::IO_ERROR;
    return FlashError::OK;
}

#endif // SFX_HAS_STORAGE
