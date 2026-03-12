/*
 * SD Card Module — Implementation
 *
 * Thread-safe SD card operations using platform-abstracted mutex
 * for multi-core safety.
 * No Serial output — all results communicated through return values
 * and callbacks. Protocol output handled by StorageServer.
 */

#if defined(SFX_HAS_STORAGE)

#include "sd_card.h"


// ============================================================================
// Constructor
// ============================================================================

SdCardModule::SdCardModule()
    : _initialized(false)
    , _cs_pin(0)
    , _sck_pin(0)
    , _mosi_pin(0)
    , _miso_pin(0)
{
    sfxMutexInit(_sdMutex);
}


// ============================================================================
// Lifecycle
// ============================================================================

bool SdCardModule::begin(uint8_t cs_pin, uint8_t sck_pin,
                         uint8_t mosi_pin, uint8_t miso_pin,
                         uint8_t speed_mhz) {
    // Store pin config for retryInit()
    _cs_pin   = cs_pin;
    _sck_pin  = sck_pin;
    _mosi_pin = mosi_pin;
    _miso_pin = miso_pin;

    // Platform-specific SPI pin configuration
#if defined(ARDUINO_ARCH_RP2040)
    SPI.setRX(miso_pin);
    SPI.setTX(mosi_pin);
    SPI.setSCK(sck_pin);
#elif defined(ARDUINO_ARCH_ESP32)
    SPI.begin(sck_pin, miso_pin, mosi_pin, cs_pin);
#endif

    lock();
    bool ok = _sd.begin(cs_pin, SD_SCK_MHZ(speed_mhz));
    unlock();

    _initialized = ok;
    return ok;
}

bool SdCardModule::retryInit(uint8_t speed_mhz) {
    return begin(_cs_pin, _sck_pin, _mosi_pin, _miso_pin, speed_mhz);
}


// ============================================================================
// Directory Operations
// ============================================================================

uint8_t SdCardModule::listDirectory(const char* path,
                                     std::function<bool(const FileEntry&)> callback) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

    File32 dir = _sd.open(path, O_RDONLY);
    if (!dir) {
        unlock();
        return SdError::NOT_FOUND;
    }
    if (!dir.isDirectory()) {
        dir.close();
        unlock();
        return SdError::NOT_FOUND;
    }

    FileEntry entry;
    while (true) {
        File32 f = dir.openNextFile();
        if (!f) break;

        f.getName(entry.name, sizeof(entry.name));
        entry.isDirectory = f.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
        f.close();

        if (!callback(entry)) break;
    }

    dir.close();
    unlock();
    return SdError::OK;
}

uint8_t SdCardModule::listTree(const char* path,
                                std::function<bool(const FileEntry&, int)> callback) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();
    bool shouldContinue = true;
    listTreeRecursive(path, 0, callback, shouldContinue);
    unlock();
    return SdError::OK;
}

void SdCardModule::listTreeRecursive(const char* path, int depth,
                                      std::function<bool(const FileEntry&, int)>& callback,
                                      bool& shouldContinue) {
    if (!shouldContinue) return;

    File32 dir = _sd.open(path, O_RDONLY);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    FileEntry entry;
    while (shouldContinue) {
        File32 f = dir.openNextFile();
        if (!f) break;

        f.getName(entry.name, sizeof(entry.name));
        entry.isDirectory = f.isDirectory();
        entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
        f.close();

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

    dir.close();
}


// ============================================================================
// File Information
// ============================================================================

uint8_t SdCardModule::getFileInfo(const char* path, FileEntry& entry) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();
    File32 f = _sd.open(path, O_RDONLY);
    if (!f) {
        unlock();
        return SdError::NOT_FOUND;
    }

    f.getName(entry.name, sizeof(entry.name));
    entry.isDirectory = f.isDirectory();
    entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
    f.close();
    unlock();
    return SdError::OK;
}

uint8_t SdCardModule::getStorageInfo(StorageInfo& info) {
    if (!_initialized) {
        info.initialized = false;
        return SdError::NOT_INITIALIZED;
    }

    lock();
    info.initialized = true;
    info.cardSize_MB = (uint32_t)(_sd.card()->sectorCount() / 2048);
    info.fatType = _sd.fatType();
    info.clusterSize_bytes = _sd.bytesPerCluster();

    uint32_t clusters = _sd.clusterCount();
    uint32_t freeClusters = _sd.freeClusterCount();
    uint64_t bpc = _sd.bytesPerCluster();
    info.totalSpace_MB = (uint32_t)((uint64_t)clusters * bpc / 1048576ULL);
    info.freeSpace_MB  = (uint32_t)((uint64_t)freeClusters * bpc / 1048576ULL);
    unlock();
    return SdError::OK;
}


// ============================================================================
// File Modification
// ============================================================================

uint8_t SdCardModule::removeFile(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();
    if (!_sd.exists(path)) {
        unlock();
        return SdError::NOT_FOUND;
    }

    bool ok = _sd.remove(path);
    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}

uint8_t SdCardModule::makeDirectory(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

    // Already exists as directory? That's OK.
    if (_sd.exists(path)) {
        File32 f = _sd.open(path, O_RDONLY);
        bool isDir = f && f.isDirectory();
        if (f) f.close();
        unlock();
        return isDir ? SdError::OK : SdError::ALREADY_EXISTS;
    }

    bool ok = _sd.mkdir(path);
    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}


// ============================================================================
// File I/O (caller must hold lock)
// ============================================================================

uint8_t SdCardModule::openRead(const char* path, File32& file) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    file = _sd.open(path, O_RDONLY);
    if (!file) return SdError::NOT_FOUND;

    if (file.isDirectory()) {
        file.close();
        return SdError::IS_DIRECTORY;
    }
    return SdError::OK;
}

uint8_t SdCardModule::openWrite(const char* path, File32& file, bool truncate) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    oflag_t flags = O_WRONLY | O_CREAT;
    if (truncate) flags |= O_TRUNC;

    file = _sd.open(path, flags);
    if (!file) return SdError::IO_ERROR;
    return SdError::OK;
}

#endif // SFX_HAS_STORAGE
