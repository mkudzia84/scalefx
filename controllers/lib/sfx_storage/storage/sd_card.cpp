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

#if !SFX_HAS_SD
// Empty translation unit when no SD backend is available
#else


// ============================================================================
// Constructor
// ============================================================================

SdCardModule::SdCardModule()
    :
#if SFX_SD_BACKEND_ESP
      _fs(nullptr),
#endif
      _initialized(false)
    , _busMode(SdBusMode::SPI)
    , _cs_pin(0), _sck_pin(0), _mosi_pin(0), _miso_pin(0)
    , _speed_mhz(25)
#if SFX_SD_BACKEND_ESP
    , _sdioOneBit(false)
    , _sdioClk(-1), _sdioCmd(-1)
    , _sdioD0(-1), _sdioD1(-1), _sdioD2(-1), _sdioD3(-1)
#endif
{
    sfxMutexInit(_sdMutex);
}


// ============================================================================
// Lifecycle
// ============================================================================

bool SdCardModule::begin(uint8_t cs_pin, uint8_t sck_pin,
                         uint8_t mosi_pin, uint8_t miso_pin,
                         uint8_t speed_mhz) {
    _cs_pin    = cs_pin;
    _sck_pin   = sck_pin;
    _mosi_pin  = mosi_pin;
    _miso_pin  = miso_pin;
    _speed_mhz = speed_mhz;
    _busMode   = SdBusMode::SPI;

#if SFX_SD_BACKEND_SDFAT
    // Pico: configure SPI pins then init SdFat
    SPI.setRX(miso_pin);
    SPI.setTX(mosi_pin);
    SPI.setSCK(sck_pin);

    lock();
    bool ok = _sd.begin(cs_pin, SD_SCK_MHZ(speed_mhz));
    unlock();
#elif SFX_SD_BACKEND_ESP
    // ESP32: init SPI bus then mount via SD library
    SPI.begin(sck_pin, miso_pin, mosi_pin, cs_pin);

    lock();
    bool ok = SD.begin(cs_pin, SPI, (uint32_t)speed_mhz * 1000000U);
    unlock();

    if (ok) _fs = &SD;
#endif

    _initialized = ok;
    return ok;
}

#if SFX_SD_BACKEND_ESP
bool SdCardModule::beginSDIO(bool oneBitMode,
                              int8_t clk, int8_t cmd,
                              int8_t d0, int8_t d1,
                              int8_t d2, int8_t d3) {
    _busMode    = oneBitMode ? SdBusMode::SDIO_1BIT : SdBusMode::SDIO_4BIT;
    _sdioOneBit = oneBitMode;
    _sdioClk = clk; _sdioCmd = cmd;
    _sdioD0 = d0; _sdioD1 = d1; _sdioD2 = d2; _sdioD3 = d3;

    // Set custom pins if any specified (all -1 = use platform defaults)
    if (clk >= 0 || cmd >= 0 || d0 >= 0) {
        SD_MMC.setPins(clk, cmd, d0, d1, d2, d3);
    }

    lock();
    bool ok = SD_MMC.begin("/sdcard", oneBitMode);
    unlock();

    if (ok) _fs = &SD_MMC;

    _initialized = ok;
    return ok;
}
#endif

bool SdCardModule::retryInit(uint8_t speed_mhz) {
#if SFX_SD_BACKEND_ESP
    if (_busMode != SdBusMode::SPI) {
        // SDIO: retry with same config
        return beginSDIO(_sdioOneBit, _sdioClk, _sdioCmd,
                         _sdioD0, _sdioD1, _sdioD2, _sdioD3);
    }
#endif
    if (speed_mhz > 0) _speed_mhz = speed_mhz;
    return begin(_cs_pin, _sck_pin, _mosi_pin, _miso_pin, _speed_mhz);
}


// ============================================================================
// Directory Operations
// ============================================================================

uint8_t SdCardModule::listDirectory(const char* path,
                                     std::function<bool(const FileEntry&)> callback) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

#if SFX_SD_BACKEND_SDFAT
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
#elif SFX_SD_BACKEND_ESP
    File dir = _fs->open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        unlock();
        return SdError::NOT_FOUND;
    }

    FileEntry entry;
    File f = dir.openNextFile();
    while (f) {
        const char* name = f.name();
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
#endif

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

#if SFX_SD_BACKEND_SDFAT
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
            char fullPath[192];
            size_t pathLen = strlen(path);
            const char* sep = (pathLen > 0 && path[pathLen - 1] == '/') ? "" : "/";
            snprintf(fullPath, sizeof(fullPath), "%s%s%s", path, sep, entry.name);
            listTreeRecursive(fullPath, depth + 1, callback, shouldContinue);
        }
    }

    dir.close();
#elif SFX_SD_BACKEND_ESP
    File dir = _fs->open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    FileEntry entry;
    File f = dir.openNextFile();
    while (shouldContinue && f) {
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
            listTreeRecursive(fullPath, depth + 1, callback, shouldContinue);
        }

        f = dir.openNextFile();
    }
    dir.close();
#endif
}


// ============================================================================
// File Information
// ============================================================================

uint8_t SdCardModule::getFileInfo(const char* path, FileEntry& entry) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

#if SFX_SD_BACKEND_SDFAT
    File32 f = _sd.open(path, O_RDONLY);
    if (!f) {
        unlock();
        return SdError::NOT_FOUND;
    }

    f.getName(entry.name, sizeof(entry.name));
    entry.isDirectory = f.isDirectory();
    entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
    f.close();
#elif SFX_SD_BACKEND_ESP
    File f = _fs->open(path);
    if (!f) {
        unlock();
        return SdError::NOT_FOUND;
    }

    // Extract filename from path (ESP32 File.name() may return full path)
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.isDirectory = f.isDirectory();
    entry.size = entry.isDirectory ? 0 : (uint32_t)f.size();
    f.close();
#endif

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
    info.busMode = _busMode;

#if SFX_SD_BACKEND_SDFAT
    info.cardSize_MB = (uint32_t)(_sd.card()->sectorCount() / 2048);
    info.fatType = _sd.fatType();
    info.clusterSize_bytes = _sd.bytesPerCluster();

    uint32_t clusters = _sd.clusterCount();
    uint32_t freeClusters = _sd.freeClusterCount();
    uint64_t bpc = _sd.bytesPerCluster();
    info.totalSpace_MB = (uint32_t)((uint64_t)clusters * bpc / 1048576ULL);
    info.freeSpace_MB  = (uint32_t)((uint64_t)freeClusters * bpc / 1048576ULL);
#elif SFX_SD_BACKEND_ESP
    // ESP32: dispatch to SD or SD_MMC based on bus mode
    if (_busMode == SdBusMode::SPI) {
        info.cardSize_MB   = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
        info.totalSpace_MB = (uint32_t)(SD.totalBytes() / (1024ULL * 1024ULL));
        uint32_t usedMB    = (uint32_t)(SD.usedBytes() / (1024ULL * 1024ULL));
        info.freeSpace_MB  = info.totalSpace_MB > usedMB ? info.totalSpace_MB - usedMB : 0;
    } else {
        info.cardSize_MB   = (uint32_t)(SD_MMC.cardSize() / (1024ULL * 1024ULL));
        info.totalSpace_MB = (uint32_t)(SD_MMC.totalBytes() / (1024ULL * 1024ULL));
        uint32_t usedMB    = (uint32_t)(SD_MMC.usedBytes() / (1024ULL * 1024ULL));
        info.freeSpace_MB  = info.totalSpace_MB > usedMB ? info.totalSpace_MB - usedMB : 0;
    }
    info.fatType = 0;           // Not exposed by ESP32 SD APIs
    info.clusterSize_bytes = 0; // Not exposed by ESP32 SD APIs
#endif

    unlock();
    return SdError::OK;
}


// ============================================================================
// File Modification
// ============================================================================

uint8_t SdCardModule::removeFile(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

#if SFX_SD_BACKEND_SDFAT
    if (!_sd.exists(path)) {
        unlock();
        return SdError::NOT_FOUND;
    }
    bool ok = _sd.remove(path);
#elif SFX_SD_BACKEND_ESP
    if (!_fs->exists(path)) {
        unlock();
        return SdError::NOT_FOUND;
    }
    bool ok = _fs->remove(path);
#endif

    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}

uint8_t SdCardModule::makeDirectory(const char* path) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

    lock();

#if SFX_SD_BACKEND_SDFAT
    // Already exists as directory? That's OK.
    if (_sd.exists(path)) {
        File32 f = _sd.open(path, O_RDONLY);
        bool isDir = f && f.isDirectory();
        if (f) f.close();
        unlock();
        return isDir ? SdError::OK : SdError::ALREADY_EXISTS;
    }
    bool ok = _sd.mkdir(path);
#elif SFX_SD_BACKEND_ESP
    // Already exists as directory? That's OK.
    if (_fs->exists(path)) {
        File f = _fs->open(path);
        bool isDir = f && f.isDirectory();
        if (f) f.close();
        unlock();
        return isDir ? SdError::OK : SdError::ALREADY_EXISTS;
    }
    bool ok = _fs->mkdir(path);
#endif

    unlock();
    return ok ? SdError::OK : SdError::IO_ERROR;
}


// ============================================================================
// File I/O (caller must hold lock)
// ============================================================================

uint8_t SdCardModule::openRead(const char* path, SdFile& file) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

#if SFX_SD_BACKEND_SDFAT
    file = _sd.open(path, O_RDONLY);
    if (!file) return SdError::NOT_FOUND;

    if (file.isDirectory()) {
        file.close();
        return SdError::IS_DIRECTORY;
    }
#elif SFX_SD_BACKEND_ESP
    file = _fs->open(path, FILE_READ);
    if (!file) return SdError::NOT_FOUND;

    if (file.isDirectory()) {
        file.close();
        return SdError::IS_DIRECTORY;
    }
#endif

    return SdError::OK;
}

uint8_t SdCardModule::openWrite(const char* path, SdFile& file, bool truncate) {
    if (!_initialized) return SdError::NOT_INITIALIZED;

#if SFX_SD_BACKEND_SDFAT
    oflag_t flags = O_WRONLY | O_CREAT;
    if (truncate) flags |= O_TRUNC;

    file = _sd.open(path, flags);
    if (!file) return SdError::IO_ERROR;
#elif SFX_SD_BACKEND_ESP
    const char* mode = truncate ? FILE_WRITE : FILE_APPEND;
    file = _fs->open(path, mode, true);  // true = create if not exists
    if (!file) return SdError::IO_ERROR;
#endif

    return SdError::OK;
}

#endif // SFX_HAS_SD
#endif // SFX_HAS_STORAGE
