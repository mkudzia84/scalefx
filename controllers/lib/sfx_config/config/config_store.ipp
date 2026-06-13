/*
 * Config Store — Template Implementation
 *
 * Included at the bottom of config_store.h.
 */

#ifndef CONFIG_STORE_IPP
#define CONFIG_STORE_IPP

// ============================================================================
// PSRAM lazy-allocation of the Data slot
// ============================================================================

template<typename TSchema, typename TPool>
bool ConfigStore<TSchema, TPool>::ensureData() const {
    if (_data) return true;
    void* mem = sfxPsramMalloc(sizeof(Data));
    if (!mem) return false;
    // Placement-new to invoke Data's default ctor — some schemas have
    // non-zero default initialisers (loopCount=-1 in AudioPlaybackOptions,
    // rpm=600 in RofItem, voltages=6000 in SmokeConfig, etc.) that
    // calloc-zero would corrupt.
    _data = new (mem) Data{};
    return true;
}

// ============================================================================
// Loading
// ============================================================================

template<typename TSchema, typename TPool>
ConfigResult ConfigStore<TSchema, TPool>::loadFromString(const char* yaml, size_t len) {
    ConfigResult result;
    _lastError[0] = '\0';
    _loaded = false;
    _usingDefaults = false;   // a real string/file load is in progress
    _fileSize = 0;

    // Allocate the PSRAM-resident data slot on first call.
    if (!ensureData()) {
        snprintf(_lastError, sizeof(_lastError),
                 "PSRAM alloc failed (Data %u bytes)", (unsigned)sizeof(Data));
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    if (!yaml || len == 0) {
        // Empty input = "use schema defaults", NOT an error.  A 0-byte file
        // (interrupted upload, operator-cleared config) is a valid request to
        // fall back — reset to defaults AND fire the loaded callback so a
        // RELOAD of a now-empty file actually re-applies defaults to the live
        // board instead of leaving stale runtime config (or silently no-op'ing
        // the apply).  Flagged notFound so reload-all tolerates it and boot's
        // loadOrFallback skips a redundant second apply.
        resetToDefaults();
        _loaded        = false;
        _usingDefaults = true;
        _fileSize      = 0;
        snprintf(_lastError, sizeof(_lastError), "Empty YAML input — using defaults");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        result.notFound = true;
        if (_onLoaded) _onLoaded(*_data);
        return result;
    }

    // Phase 1: Parse YAML text into node tree
    YamlParser<TPool> parser;
    if (!parser.parse(yaml, len)) {
        snprintf(_lastError, sizeof(_lastError), "YAML parse: %s", parser.error());
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }
    result.parsed = true;

    // Phase 2: Populate config struct from YAML tree
    resetToDefaults();
    if (!TSchema::populate(*_data, parser)) {
        snprintf(_lastError, sizeof(_lastError), "Schema populate failed");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }
    result.populated = true;

    // Phase 3: Validate config against schema rules
    char validationError[128] = {};
    if (!TSchema::validate(*_data, validationError, sizeof(validationError))) {
        snprintf(_lastError, sizeof(_lastError), "Validation: %s", validationError);
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        // Still mark as loaded — data is populated, just invalid
        _loaded = true;
        _fileSize = (uint16_t)len;
        return result;
    }
    result.validated = true;

    // Success
    _loaded = true;
    _fileSize = (uint16_t)len;
    result.ok = true;

    // Cache raw YAML for saveToFile()
    storeRawYaml(yaml, len);

    // Fire loaded callback
    if (_onLoaded) {
        _onLoaded(*_data);
    }

    return result;
}

template<typename TSchema, typename TPool>
ConfigResult ConfigStore<TSchema, TPool>::loadFromFile(const char* path) {
    ConfigResult result;
    _lastError[0] = '\0';

    if (!_fileReader) {
        snprintf(_lastError, sizeof(_lastError), "No file reader set");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    const char* filePath = path ? path : TSchema::defaultPath();

    // Allocate temporary read buffer in PSRAM — 8 KB is a heavy
    // transient that used to fall through `new[]` into whichever heap
    // the allocator picked.  Explicit sfxPsramMalloc keeps the DMA-cap
    // pool clean during boot config-load (same lesson as YAML pools
    // in Phase 4 polish 2026-05-27).
    constexpr size_t MAX_CONFIG_SIZE = 8192;
    char* buffer = static_cast<char*>(sfxPsramMalloc(MAX_CONFIG_SIZE));
    if (!buffer) {
        snprintf(_lastError, sizeof(_lastError), "Buffer allocation failed (%u bytes)",
                 (unsigned)MAX_CONFIG_SIZE);
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    // Read file
    int bytesRead = _fileReader(filePath, buffer, MAX_CONFIG_SIZE);
    if (bytesRead <= 0) {
        // Absent, unreadable, OR 0-byte — all mean "no usable config": reset
        // to schema defaults and fire the loaded callback so a RELOAD that
        // finds the file gone/empty re-applies defaults to the live board
        // (same contract as the empty-string path in loadFromString).  Without
        // the reset, a reload here would leave stale runtime config behind.
        resetToDefaults();
        snprintf(_lastError, sizeof(_lastError), "File read failed: %s", filePath);
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        result.notFound = true;   // absent / unreadable — reload-all tolerates this
        _loaded        = false;   // no file backing the store now
        _usingDefaults = true;    // running on struct defaults (functional)
        _fileSize      = 0;
        sfxPsramFree(buffer);
        if (_onLoaded && _data) _onLoaded(*_data);
        return result;
    }

    // Parse the YAML content
    result = loadFromString(buffer, (size_t)bytesRead);
    sfxPsramFree(buffer);
    return result;
}

// ============================================================================
// Validation
// ============================================================================

template<typename TSchema, typename TPool>
bool ConfigStore<TSchema, TPool>::validate(char* errBuf, size_t errBufSize) const {
    if (!ensureData()) return false;
    return TSchema::validate(*_data, errBuf, errBufSize);
}

// ============================================================================
// State
// ============================================================================

template<typename TSchema, typename TPool>
void ConfigStore<TSchema, TPool>::resetToDefaults() {
    if (!ensureData()) return;
    *_data = Data{};
}

// ============================================================================
// Saving
// ============================================================================

template<typename TSchema, typename TPool>
ConfigResult ConfigStore<TSchema, TPool>::saveToFile(const char* path) {
    ConfigResult result;
    _lastError[0] = '\0';

    if (!_fileWriter) {
        snprintf(_lastError, sizeof(_lastError), "No file writer set");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    if (!_rawYaml || _rawYamlLen == 0) {
        snprintf(_lastError, sizeof(_lastError), "No config data to save (load first)");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    const char* filePath = path ? path : TSchema::defaultPath();

    int written = _fileWriter(filePath, _rawYaml, _rawYamlLen);
    if (written < 0 || (size_t)written != _rawYamlLen) {
        snprintf(_lastError, sizeof(_lastError), "File write failed: %s (%d/%u bytes)",
                 filePath, written, (unsigned)_rawYamlLen);
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    result.ok = true;
    result.parsed = true;
    result.populated = true;
    result.validated = true;
    return result;
}

// ============================================================================
// Raw YAML Cache
// ============================================================================

template<typename TSchema, typename TPool>
void ConfigStore<TSchema, TPool>::storeRawYaml(const char* yaml, size_t len) {
    freeRawYaml();
    // PSRAM-explicit for the per-store YAML cache — held for the
    // lifetime of the store, lazily reused by saveToFile().
    _rawYaml = static_cast<char*>(sfxPsramMalloc(len));
    if (_rawYaml) {
        memcpy(_rawYaml, yaml, len);
        _rawYamlLen = len;
    }
}

template<typename TSchema, typename TPool>
void ConfigStore<TSchema, TPool>::freeRawYaml() {
    sfxPsramFree(_rawYaml);
    _rawYaml = nullptr;
    _rawYamlLen = 0;
}

#endif // CONFIG_STORE_IPP
