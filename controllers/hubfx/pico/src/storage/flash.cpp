/*
 * Flash Module — Implementation
 *
 * Thread-safe LittleFS operations using pico mutex for dual-core safety.
 * No Serial output — all results communicated through return values
 * and callbacks. Protocol output handled by StorageServer.
 *
 * LittleFS API differences from SdFat:
 *   - Directory listing uses Dir iterator (openDir/next), not File32.openNextFile()
 *   - Storage info via FSInfo struct (totalBytes/usedBytes), not sector counts
 *   - File type is ::File (aliased LFSFile), not File32
 *   - Open modes are "r"/"w"/"a" strings, not O_RDONLY/O_WRONLY flags
 *   - mkdir() creates path recursively by default
 */

#include "flash.h"


// ============================================================================
// Constructor
// ============================================================================

FlashModule::FlashModule()
    : _initialized(false)
{
    mutex_init(&_flashMutex);
}


// ============================================================================
// Lifecycle
// ============================================================================

bool FlashModule::begin() {
    lock();
    bool ok = LittleFS.begin();
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

    Dir dir = LittleFS.openDir(path);

    FileEntry entry;
    while (dir.next()) {
        strncpy(entry.name, dir.fileName().c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.isDirectory = dir.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)dir.fileSize();

        if (!callback(entry)) break;
    }

    unlock();
    return FlashError::OK;
}

uint8_t FlashModule::listTree(const char* path,
                               std::function<bool(const FileEntry&, int)> callback) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();
    bool shouldContinue = true;
    listTreeRecursive(path, 0, callback, shouldContinue);
    unlock();
    return FlashError::OK;
}

void FlashModule::listTreeRecursive(const char* path, int depth,
                                     std::function<bool(const FileEntry&, int)>& callback,
                                     bool& shouldContinue) {
    if (!shouldContinue) return;

    Dir dir = LittleFS.openDir(path);

    FileEntry entry;
    while (shouldContinue && dir.next()) {
        strncpy(entry.name, dir.fileName().c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.isDirectory = dir.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)dir.fileSize();

        if (!callback(entry, depth)) {
            shouldContinue = false;
            break;
        }

        if (entry.isDirectory) {
            // Build full path for recursion
            char fullPath[192];
            size_t pathLen = strlen(path);
            const char* sep = (pathLen > 0 && path[pathLen - 1] == '/') ? "" : "/";
            snprintf(fullPath, sizeof(fullPath), "%s%s%s", path, sep, entry.name);
            listTreeRecursive(fullPath, depth + 1, callback, shouldContinue);
        }
    }
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

    entry.isDirectory = f.isDir();
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
    FSInfo fsinfo;
    LittleFS.info(fsinfo);

    info.initialized = true;
    info.totalBytes  = (uint32_t)fsinfo.totalBytes;
    info.usedBytes   = (uint32_t)fsinfo.usedBytes;
    info.freeBytes   = (uint32_t)(fsinfo.totalBytes - fsinfo.usedBytes);
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

uint8_t FlashModule::makeDirectory(const char* path) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    lock();

    // Already exists as directory? That's OK.
    if (LittleFS.exists(path)) {
        LFSFile f = LittleFS.open(path, "r");
        bool isDir = f && f.isDir();
        if (f) f.close();
        unlock();
        return isDir ? FlashError::OK : FlashError::ALREADY_EXISTS;
    }

    bool ok = LittleFS.mkdir(path);
    unlock();
    return ok ? FlashError::OK : FlashError::IO_ERROR;
}


// ============================================================================
// File I/O (caller must hold lock)
// ============================================================================

uint8_t FlashModule::openRead(const char* path, LFSFile& file) {
    if (!_initialized) return FlashError::NOT_INITIALIZED;

    file = LittleFS.open(path, "r");
    if (!file) return FlashError::NOT_FOUND;

    if (file.isDir()) {
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
