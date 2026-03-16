/*
 * SD Card Module — Template Method Implementations
 *
 * All SdCardModuleT<TPolicy> method bodies. Included from sd_card.h
 * after the class declaration and before policy includes.
 *
 * File operations delegate to the policy's filesystem primitives,
 * while this template handles mutex acquisition, error mapping,
 * recursive traversal, and safety limits.
 */


// ============================================================================
// Lifecycle
// ============================================================================

template <typename TPolicy>
bool SdCardModuleT<TPolicy>::begin(const Config& cfg) {
    _config = cfg;

    lock();
    bool ok = _policy.mount(cfg);
    unlock();

    _initialized = ok;
    return ok;
}

template <typename TPolicy>
bool SdCardModuleT<TPolicy>::retryInit() {
    unmount();
    return begin(_config);
}

template <typename TPolicy>
SdCardType SdCardModuleT<TPolicy>::cardType() {
    if (!_initialized) return SdCardType::NONE;
    return _policy.cardType();
}

template <typename TPolicy>
void SdCardModuleT<TPolicy>::unmount() {
    lock();
    _initialized = false;
    _policy.unmount();
    unlock();
}


// ============================================================================
// Directory Operations
// ============================================================================

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::listDirectory(
        const char* path,
        std::function<bool(const FileEntry&)> callback) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();
    int entryCount = 0;
    bool limitHit = false;

    FileHandle dir = _policy.openDir(path);
    if (!TPolicy::isValid(dir)) {
        unlock();
        return SdError::NOT_FOUND;
    }
    if (!TPolicy::isDirectory(dir)) {
        TPolicy::closeFile(dir);
        unlock();
        return SdError::NOT_FOUND;
    }

    FileEntry entry;
    while (true) {
        FileHandle f = TPolicy::nextFile(dir);
        if (!TPolicy::isValid(f)) break;

        if (++entryCount > MAX_TREE_ENTRIES) {
            TPolicy::closeFile(f);
            limitHit = true;
            break;
        }

        TPolicy::extractName(f, entry.name, sizeof(entry.name));
        entry.isDirectory = TPolicy::isDirectory(f);
        entry.size = entry.isDirectory ? 0 : TPolicy::fileSize(f);
        TPolicy::closeFile(f);

        if (!callback(entry)) break;
    }

    TPolicy::closeFile(dir);
    unlock();
    return limitHit ? SdError::LIMIT_EXCEEDED : SdError::OK;
}

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::listTree(
        const char* path,
        std::function<bool(const FileEntry&, int)> callback) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();
    bool shouldContinue = true;
    int entryCount = 0;
    bool limitHit = false;
    listTreeRecursive(path, 0, callback, shouldContinue, entryCount, limitHit);
    unlock();
    return limitHit ? SdError::LIMIT_EXCEEDED : SdError::OK;
}

template <typename TPolicy>
void SdCardModuleT<TPolicy>::listTreeRecursive(
        const char* path, int depth,
        std::function<bool(const FileEntry&, int)>& callback,
        bool& shouldContinue, int& entryCount,
        bool& limitHit) {
    if (!shouldContinue) return;

    // Safety: prevent infinite recursion on corrupted circular directory refs
    if (depth >= MAX_TREE_DEPTH) {
        limitHit = true;
        shouldContinue = false;
        return;
    }

    FileHandle dir = _policy.openDir(path);
    if (!TPolicy::isValid(dir) || !TPolicy::isDirectory(dir)) {
        if (TPolicy::isValid(dir)) TPolicy::closeFile(dir);
        return;
    }

    FileEntry entry;
    while (shouldContinue) {
        FileHandle f = TPolicy::nextFile(dir);
        if (!TPolicy::isValid(f)) break;

        // Safety: prevent infinite iteration on corrupted FAT directory chains
        if (++entryCount > MAX_TREE_ENTRIES) {
            TPolicy::closeFile(f);
            limitHit = true;
            shouldContinue = false;
            break;
        }

        TPolicy::extractName(f, entry.name, sizeof(entry.name));
        entry.isDirectory = TPolicy::isDirectory(f);
        entry.size = entry.isDirectory ? 0 : TPolicy::fileSize(f);
        TPolicy::closeFile(f);

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

    TPolicy::closeFile(dir);
}


// ============================================================================
// File Information
// ============================================================================

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::getFileInfo(const char* path, FileEntry& entry) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

    FileHandle f = _policy.openReadFile(path);
    if (!TPolicy::isValid(f)) {
        unlock();
        return SdError::NOT_FOUND;
    }

    // Extract basename from path (avoid platform-specific file.name() behavior)
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.isDirectory = TPolicy::isDirectory(f);
    entry.size = entry.isDirectory ? 0 : TPolicy::fileSize(f);
    TPolicy::closeFile(f);

    unlock();
    return SdError::OK;
}

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::getStorageInfo(StorageInfo& info) {
    if (!_initialized) {
        info.initialized = false;
        return SdError::NOT_INITIALIZED;
    }

    lock();
    info.initialized = true;
    info.busMode = BUS_MODE;

    _policy.fillStorageInfo(info);
    info.cardType = _policy.cardType();

    unlock();
    return SdError::OK;
}


// ============================================================================
// File Modification
// ============================================================================

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::removeFile(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();
    if (!_policy.exists(path)) {
        unlock();
        return SdError::NOT_FOUND;
    }
    bool ok = _policy.removeFile(path);
    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::removeDirectory(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

    // Check path exists — open to determine file vs directory
    FileHandle f = _policy.openDir(path);
    if (!TPolicy::isValid(f)) {
        unlock();
        return SdError::NOT_FOUND;
    }
    bool isDir = TPolicy::isDirectory(f);
    TPolicy::closeFile(f);

    if (!isDir) {
        bool ok = _policy.removeFile(path);
        unlock();
        return ok ? SdError::OK : SdError::IO_ERROR;
    }

    bool ok = removeDirectoryRecursive(path);
    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}

template <typename TPolicy>
bool SdCardModuleT<TPolicy>::removeDirectoryRecursive(const char* path, int depth) {
    // Caller MUST hold lock.
    // Depth-first: remove all children, then the directory itself.

    // Safety: prevent infinite recursion on corrupted circular directory refs
    if (depth >= MAX_TREE_DEPTH) return false;

    FileHandle dir = _policy.openDir(path);
    if (!TPolicy::isValid(dir) || !TPolicy::isDirectory(dir)) {
        if (TPolicy::isValid(dir)) TPolicy::closeFile(dir);
        return false;
    }

    while (true) {
        FileHandle f = TPolicy::nextFile(dir);
        if (!TPolicy::isValid(f)) break;

        char name[64];
        TPolicy::extractName(f, name, sizeof(name));
        bool isDir = TPolicy::isDirectory(f);
        TPolicy::closeFile(f);

        char fullPath[192];
        size_t pathLen = strlen(path);
        const char* sep = (pathLen > 0 && path[pathLen - 1] == '/') ? "" : "/";
        snprintf(fullPath, sizeof(fullPath), "%s%s%s", path, sep, name);

        if (isDir) {
            if (!removeDirectoryRecursive(fullPath, depth + 1)) {
                TPolicy::closeFile(dir);
                return false;
            }
        } else {
            if (!_policy.removeFile(fullPath)) {
                TPolicy::closeFile(dir);
                return false;
            }
        }
    }

    TPolicy::closeFile(dir);
    return _policy.removeDir(path);
}

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::makeDirectory(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

    // Already exists as directory? That's OK.
    if (_policy.exists(path)) {
        FileHandle f = _policy.openDir(path);
        bool isDir = TPolicy::isValid(f) && TPolicy::isDirectory(f);
        if (TPolicy::isValid(f)) TPolicy::closeFile(f);
        unlock();
        return isDir ? SdError::OK : SdError::ALREADY_EXISTS;
    }

    bool ok = _policy.makeDir(path);
    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}


// ============================================================================
// File I/O (caller must hold lock)
// ============================================================================

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::openRead(const char* path, FileHandle& file) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    file = _policy.openReadFile(path);
    if (!TPolicy::isValid(file)) return SdError::NOT_FOUND;

    if (TPolicy::isDirectory(file)) {
        TPolicy::closeFile(file);
        return SdError::IS_DIRECTORY;
    }

    return SdError::OK;
}

template <typename TPolicy>
uint8_t SdCardModuleT<TPolicy>::openWrite(const char* path, FileHandle& file,
                                           bool truncate) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    file = _policy.openWriteFile(path, truncate);
    if (!TPolicy::isValid(file)) return SdError::IO_ERROR;

    return SdError::OK;
}
