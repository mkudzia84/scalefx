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

#include <concepts>
#include <cstring>
#include <functional>
#include <new>                          // placement-new for PSRAM-resident _data
#include <type_traits>
#include <platform/sfx_platform.h>      // sfxPsramMalloc / sfxPsramFree
#include "yaml_parser.h"

// ============================================================================
// Concept: ConfigSchema<TSchema, TPool>
// ============================================================================
//
// Documents the schema contract previously enforced only by comments. A
// schema instantiation must expose a nested DataType, plus three statics:
// populate(), validate(), defaultPath(). When ConfigStore<TSchema, TPool>
// is instantiated the requires-clause picks up missing or mistyped methods
// at the point of use rather than as link errors deep inside a controller's
// .ino file.

template <typename TSchema, typename TPool>
concept ConfigSchema = requires(typename TSchema::DataType& d,
                                const typename TSchema::DataType& cd,
                                const YamlParser<TPool>& parser,
                                char* err, size_t errLen) {
    typename TSchema::DataType;
    { TSchema::populate(d, parser) }     -> std::convertible_to<bool>;
    { TSchema::validate(cd, err, errLen) } -> std::convertible_to<bool>;
    { TSchema::defaultPath() }            -> std::convertible_to<const char*>;
};

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
    bool notFound    = false;   ///< File absent / unreadable (not a parse error)
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
    // Concept enforced via static_assert rather than a class-level requires
    // clause — the latter forces every out-of-class member definition in
    // config_store.ipp to repeat the constraint. The static_assert still
    // fires at template-instantiation time with a clear message.
    static_assert(ConfigSchema<TSchema, TPool>,
                  "TSchema must satisfy ConfigSchema — expose DataType + "
                  "populate()/validate()/defaultPath() per the concept.");
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

    /** @brief Get current config data (read-only).
     *  Lazy-allocates the PSRAM-resident storage on first access. */
    const Data& data() const { ensureData(); return *_data; }

    /** @brief Get mutable config data (for programmatic updates).
     *  Lazy-allocates the PSRAM-resident storage on first access. */
    Data& data() { ensureData(); return *_data; }

    /** @brief Check if config was successfully loaded from file/string */
    bool isLoaded() const { return _loaded; }

    /** @brief True when the last load found NO file and the store is running on
     *  struct defaults (a functional, expected state for optional configs —
     *  /alerts.yaml, /lightfx.yaml, … don't exist until the operator saves one).
     *  Aggregate CONFIG_STATUS counts this as "OK", like boot's loadOrFallback. */
    bool usingDefaults() const { return _usingDefaults; }

    /** @brief Size of last loaded YAML content in bytes */
    uint16_t fileSize() const { return _fileSize; }

    /** @brief Last error message (empty if no error) */
    const char* lastError() const { return _lastError; }

    /** @brief Check if raw YAML text is cached (available for save) */
    bool hasRawYaml() const { return _rawYaml != nullptr && _rawYamlLen > 0; }

    /** @brief Default file path from schema */
    static const char* defaultPath() { return TSchema::defaultPath(); }

    /** @brief Destructor — frees cached raw YAML + PSRAM-allocated _data */
    ~ConfigStore() {
        freeRawYaml();
        if (_data) {
            _data->~Data();
            sfxPsramFree(_data);
            _data = nullptr;
        }
    }

private:
    /// Phase 4 polish (2026-05-27): _data lives in PSRAM (lazy-allocated
    /// on first use via placement-new) instead of being a value member.
    /// HubFxConfig + GunFxConfig + LandingConfig + AlertsConfig +
    /// LightFxConfig combined ≈ 13-15 KB DRAM the singletons no longer
    /// carry in BSS.  Read paths: apply translators (boot / config reload)
    /// + status queries (protocol rate) — never on the audio path.
    /// Placement-new (rather than calloc-zero) because some Data structs
    /// have non-zero default initialisers (e.g. AudioPlaybackOptions
    /// has loopCount=-1) that zero-fill would corrupt.
    /// `mutable` so the const `data()` accessor can lazy-allocate.
    mutable Data*  _data       = nullptr;
    bool           _loaded     = false;
    bool           _usingDefaults = false;   ///< last load found no file → defaults
    uint16_t       _fileSize   = 0;
    char           _lastError[128] = {};
    FileReadFunc   _fileReader = nullptr;
    FileWriteFunc  _fileWriter = nullptr;
    LoadedCallback _onLoaded;

    // Raw YAML cache (for saveToFile) — PSRAM-allocated via sfxPsramMalloc.
    char*          _rawYaml    = nullptr;
    size_t         _rawYamlLen = 0;

    /// Lazy-allocate _data in PSRAM + run the default constructor via
    /// placement-new.  Returns true on success; false (with _data left
    /// null) on PSRAM exhaustion — the caller short-circuits to an
    /// error result.  Idempotent — re-running on an already-allocated
    /// slot is a no-op.
    bool ensureData() const;

    void storeRawYaml(const char* yaml, size_t len);
    void freeRawYaml();
};

// ============================================================================
// Template Implementation
// ============================================================================

#include "config_store.ipp"

#endif // CONFIG_STORE_H
