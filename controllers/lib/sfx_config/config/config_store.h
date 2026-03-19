/*
 * Config Store — Templatized Configuration Manager
 *
 * ConfigStore<TSchema> provides type-safe configuration loading, validation,
 * and access.  The config format is defined per board via a Schema type that
 * specifies:
 *
 *   - DataType:       C++ struct holding all config fields (with defaults)
 *   - populate():     Maps YAML tree → struct fields
 *   - validate():     Checks constraints (ranges, required fields)
 *   - defaultPath():  Default file path on storage (e.g., "/config.yaml")
 *
 * The board-specific schema is the ONLY thing that changes between boards.
 * YamlParser, ConfigStore, and ConfigServer are generic.
 *
 * Schema Contract:
 *
 *   struct MyBoardSchema {
 *       using DataType = MyBoardConfig;  // POD-ish struct with default initializers
 *
 *       static bool populate(DataType& data, const YamlParser<>& parser);
 *       static bool validate(const DataType& data, char* errBuf, size_t errBufSize);
 *       static const char* defaultPath();  // e.g., "/config.yaml"
 *   };
 *
 * population function uses the parser's query API:
 *
 *   static bool populate(DataType& data, const YamlParser<>& parser) {
 *       data.engineEnabled = parser.getBool("engine_fx.enabled", true);
 *       strncpy(data.engineType, parser.getString("engine_fx.type", "turbine"), sizeof(data.engineType));
 *       data.triggerChannel = (uint8_t)parser.getInt("gun_fx.trigger.input_channel", 2);
 *
 *       // Sequence access
 *       int n = parser.sequenceLength("gun_fx.rates_of_fire");
 *       data.rateCount = (n > MAX_RATES) ? MAX_RATES : n;
 *       for (int i = 0; i < data.rateCount; i++) {
 *           auto* item = parser.sequenceItem("gun_fx.rates_of_fire", i);
 *           data.rates[i].rpm = YamlParser<>::getIntFrom(item, "rpm", 200);
 *       }
 *       return true;
 *   }
 *
 * Usage:
 *   ConfigStore<HubFxConfigSchema> configStore;
 *   if (configStore.loadFromString(yamlText, len)) {
 *       auto& cfg = configStore.data();
 *       if (cfg.engineEnabled) startEngine(cfg.engineType);
 *   }
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <Arduino.h>
#include <cstring>
#include <functional>
#include "yaml_parser.h"

// ============================================================================
// Config Load Result
// ============================================================================

/**
 * @brief Result of a config load/validate operation
 */
struct ConfigResult {
    bool ok          = false;   ///< Overall success
    bool parsed      = false;   ///< YAML parsing succeeded
    bool populated   = false;   ///< Schema populate() succeeded
    bool validated   = false;   ///< Schema validate() succeeded
    char error[128]  = {};      ///< Error description (empty if ok)

    operator bool() const { return ok; }
};

// ============================================================================
// Config Store
// ============================================================================

/**
 * @brief Templatized configuration manager
 *
 * @tparam TSchema Schema type defining DataType, populate(), validate(), defaultPath()
 * @tparam TPool   YAML parser pool config (default: DefaultYamlPool)
 */
template<typename TSchema, typename TPool = DefaultYamlPool>
class ConfigStore {
public:
    using Data = typename TSchema::DataType;

    ConfigStore() = default;

    // ========================================================================
    // Loading
    // ========================================================================

    /**
     * @brief Load config from YAML string
     *
     * Parses the YAML, populates the config struct via schema, then validates.
     * On failure, the config is reset to defaults.
     *
     * @param yaml YAML content
     * @param len  Content length
     * @return ConfigResult with success/error info
     */
    ConfigResult loadFromString(const char* yaml, size_t len);

    /**
     * @brief Load config from a file read into a buffer
     *
     * Convenience: calls the fileReader callback, then loadFromString().
     * Use setFileReader() before calling this.
     *
     * @param path File path (nullptr uses schema default)
     * @return ConfigResult with success/error info
     */
    ConfigResult loadFromFile(const char* path = nullptr);

    // ========================================================================
    // File I/O Callbacks
    // ========================================================================

    /**
     * @brief Function that reads a file into a buffer
     *
     * @param path     File path to read
     * @param buffer   Output buffer
     * @param maxLen   Maximum bytes to read
     * @return Bytes read, or -1 on error
     */
    using FileReadFunc = int (*)(const char* path, char* buffer, size_t maxLen);

    /**
     * @brief Function that writes a buffer to a file
     *
     * @param path     File path to write
     * @param data     Data to write
     * @param len      Data length
     * @return Bytes written, or -1 on error
     */
    using FileWriteFunc = int (*)(const char* path, const char* data, size_t len);

    /**
     * @brief Set the file read function (e.g., wrapping FlashModule or SdCardModule)
     */
    void setFileReader(FileReadFunc fn) { _fileReader = fn; }

    /**
     * @brief Set the file write function (e.g., wrapping FlashModule)
     */
    void setFileWriter(FileWriteFunc fn) { _fileWriter = fn; }

    // ========================================================================
    // Saving
    // ========================================================================

    /**
     * @brief Save the last-loaded raw YAML text to a file
     *
     * Writes the cached YAML source text via the fileWriter callback.
     * Only available after a successful loadFromString() or loadFromFile().
     *
     * @param path File path (nullptr uses schema default)
     * @return ConfigResult with success/error info
     */
    ConfigResult saveToFile(const char* path = nullptr);

    // ========================================================================
    // Loaded Callback
    // ========================================================================

    /**
     * @brief Callback invoked after a successful config load.
     *
     * Fires at the end of loadFromString() / loadFromFile() when the
     * config is parsed, populated, and validated (result.ok == true).
     * The callback receives a const reference to the populated data.
     */
    using LoadedCallback = std::function<void(const Data& config)>;

    /**
     * @brief Register callback for when config is successfully loaded
     */
    void onLoaded(LoadedCallback cb) { _onLoaded = std::move(cb); }

    // ========================================================================
    // Validation
    // ========================================================================

    /**
     * @brief Validate current config against schema rules
     * @param errBuf   Optional buffer for error message
     * @param errBufSize Buffer size
     * @return true if validation passes
     */
    bool validate(char* errBuf = nullptr, size_t errBufSize = 0) const;

    // ========================================================================
    // State
    // ========================================================================

    /**
     * @brief Reset config to default values (from struct initializers)
     */
    void resetToDefaults();

    /** @brief Get current config data (read-only) */
    const Data& data() const { return _data; }

    /** @brief Get mutable config data (for programmatic updates) */
    Data& data() { return _data; }

    /** @brief Check if config was successfully loaded from file/string */
    bool isLoaded() const { return _loaded; }

    /** @brief Size of last loaded YAML content in bytes */
    uint16_t fileSize() const { return _fileSize; }

    /** @brief Last error message (empty if no error) */
    const char* lastError() const { return _lastError; }

    /** @brief Check if raw YAML text is cached (available for save) */
    bool hasRawYaml() const { return _rawYaml != nullptr && _rawYamlLen > 0; }

    /** @brief Default file path from schema */
    static const char* defaultPath() { return TSchema::defaultPath(); }

    /** @brief Destructor — frees cached raw YAML */
    ~ConfigStore() { freeRawYaml(); }

private:
    Data           _data{};             ///< Config data with default initializers
    bool           _loaded    = false;
    uint16_t       _fileSize  = 0;
    char           _lastError[128] = {};
    FileReadFunc   _fileReader  = nullptr;
    FileWriteFunc  _fileWriter  = nullptr;
    LoadedCallback _onLoaded;

    // Raw YAML cache (for saveToFile)
    char*          _rawYaml    = nullptr;
    size_t         _rawYamlLen = 0;

    void storeRawYaml(const char* yaml, size_t len);
    void freeRawYaml();
};

// ============================================================================
// Template Implementation
// ============================================================================

#include "config_store.ipp"

#endif // CONFIG_STORE_H
