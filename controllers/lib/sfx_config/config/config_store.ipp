/*
 * Config Store — Template Implementation
 *
 * Included at the bottom of config_store.h.
 */

#ifndef CONFIG_STORE_IPP
#define CONFIG_STORE_IPP

// ============================================================================
// Loading
// ============================================================================

template<typename TSchema, typename TPool>
ConfigResult ConfigStore<TSchema, TPool>::loadFromString(const char* yaml, size_t len) {
    ConfigResult result;
    _lastError[0] = '\0';
    _loaded = false;
    _fileSize = 0;

    if (!yaml || len == 0) {
        snprintf(_lastError, sizeof(_lastError), "Empty YAML input");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
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
    if (!TSchema::populate(_data, parser)) {
        snprintf(_lastError, sizeof(_lastError), "Schema populate failed");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }
    result.populated = true;

    // Phase 3: Validate config against schema rules
    char validationError[128] = {};
    if (!TSchema::validate(_data, validationError, sizeof(validationError))) {
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
        _onLoaded(_data);
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

    // Allocate temporary read buffer
    constexpr size_t MAX_CONFIG_SIZE = 8192;
    char* buffer = new (std::nothrow) char[MAX_CONFIG_SIZE];
    if (!buffer) {
        snprintf(_lastError, sizeof(_lastError), "Buffer allocation failed");
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        return result;
    }

    // Read file
    int bytesRead = _fileReader(filePath, buffer, MAX_CONFIG_SIZE);
    if (bytesRead <= 0) {
        snprintf(_lastError, sizeof(_lastError), "File read failed: %s", filePath);
        strncpy(result.error, _lastError, sizeof(result.error) - 1);
        delete[] buffer;
        return result;
    }

    // Parse the YAML content
    result = loadFromString(buffer, (size_t)bytesRead);
    delete[] buffer;
    return result;
}

// ============================================================================
// Validation
// ============================================================================

template<typename TSchema, typename TPool>
bool ConfigStore<TSchema, TPool>::validate(char* errBuf, size_t errBufSize) const {
    return TSchema::validate(_data, errBuf, errBufSize);
}

// ============================================================================
// State
// ============================================================================

template<typename TSchema, typename TPool>
void ConfigStore<TSchema, TPool>::resetToDefaults() {
    _data = Data{};
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
    _rawYaml = new (std::nothrow) char[len];
    if (_rawYaml) {
        memcpy(_rawYaml, yaml, len);
        _rawYamlLen = len;
    }
}

template<typename TSchema, typename TPool>
void ConfigStore<TSchema, TPool>::freeRawYaml() {
    if (_rawYaml) {
        delete[] _rawYaml;
        _rawYaml = nullptr;
    }
    _rawYamlLen = 0;
}

#endif // CONFIG_STORE_IPP
