/**
 * ConfigReader - Configuration Loader
 * 
 * OOP YAML configuration reader for Raspberry Pi Pico.
 * Reads configuration from SD card or LittleFS flash storage.
 * 
 * Provides settings structures that map directly to EngineFX and GunFX.
 */

#ifndef CONFIG_READER_H
#define CONFIG_READER_H

#include <Arduino.h>
#include <SdFat.h>
#include <LittleFS.h>
#include <pico/mutex.h>
#include "../effects/engine_fx.h"
#include "../effects/gun_fx.h"

// ============================================================================
//  CONSTANTS
// ============================================================================

namespace ConfigReaderConfig {
    constexpr size_t MAX_STRING_LENGTH = 64;
    constexpr size_t MAX_PATH_LENGTH   = 128;
    constexpr size_t MAX_LINE_LENGTH   = 256;
}

// ============================================================================
//  STORAGE TYPE
// ============================================================================

enum class ConfigStorage : uint8_t {
    SD,     // SD card via SPI
    Flash   // LittleFS on Pico flash
};

// ============================================================================
//  HUB FX SETTINGS (combined output)
// ============================================================================

struct HubFXSettings {
    EngineFXSettings engine;
    GunFXSettings gun;
    bool loaded = false;
};

// ============================================================================
//  CONFIG READER CLASS
// ============================================================================

class ConfigReader {
public:
    ConfigReader() = default;
    ~ConfigReader() = default;

    // Non-copyable
    ConfigReader(const ConfigReader&) = delete;
    ConfigReader& operator=(const ConfigReader&) = delete;

    // ---- Initialization ----
    
    /**
     * @brief Initialize with SD card storage (uses SdCardModule singleton)
     * @return true if successful
     */
    bool begin();
    
    /**
     * @brief Legacy: Initialize with SD card storage (parameters ignored)
     * @deprecated Use begin() instead - SdCardModule singleton is used internally
     */
    bool begin(SdFat* sd, mutex_t* sdMutex = nullptr);
    
    /**
     * @brief Initialize with LittleFS flash storage
     * @return true if successful
     */
    bool beginFlash();

    // ---- Loading ----
    
    /**
     * @brief Load configuration from YAML file
     * @param filename Path to YAML file
     * @return true if successful
     */
    bool load(const char* filename);
    
    /**
     * @brief Load default configuration values
     */
    void loadDefaults();
    
    // ---- Saving (Flash only) ----
    
    /**
     * @brief Save configuration to flash
     * @param yaml YAML content to save
     * @param length Size of YAML content
     * @param filename Target file (default: /config.yaml)
     * @return true if successful
     */
    bool save(const char* yaml, size_t length, const char* filename = "/config.yaml");
    
    /**
     * @brief Backup current config to .bak file
     * @return true if successful
     */
    bool backup();
    
    /**
     * @brief Restore config from .bak file
     * @return true if successful
     */
    bool restore();
    
    /**
     * @brief Get size of config file
     * @param filename Path to file (default: /config.yaml)
     * @return File size in bytes, or -1 if error
     */
    int getSize(const char* filename = "/config.yaml");

    // ---- Accessors ----
    
    const HubFXSettings& settings() const { return _settings; }
    const EngineFXSettings& engineSettings() const { return _settings.engine; }
    const GunFXSettings& gunSettings() const { return _settings.gun; }
    bool isLoaded() const { return _settings.loaded; }
    bool isFlashStorage() const { return _storage == ConfigStorage::Flash; }

    // ---- Debug ----
    void print() const;

private:
    // Storage
    ConfigStorage _storage = ConfigStorage::SD;
    bool _initialized = false;
    
    // SD locking helpers (use SdCardModule singleton)
    void sdLock();
    void sdUnlock();
    SdFat& sd();
    
    // Configuration output
    HubFXSettings _settings;
    
    // String buffers for file paths (since settings use const char*)
    char _engineStartingSound[ConfigReaderConfig::MAX_PATH_LENGTH] = "";
    char _engineRunningSound[ConfigReaderConfig::MAX_PATH_LENGTH] = "";
    char _engineStoppingSound[ConfigReaderConfig::MAX_PATH_LENGTH] = "";
    char _gunSoundFiles[GunFXConfig::MAX_RATES_OF_FIRE][ConfigReaderConfig::MAX_PATH_LENGTH] = {};

    // ---- Parsing Context ----
    struct ParseContext {
        char section[64] = "";
        char subsection[64] = "";
        char subsubsection[64] = "";
        int listIndex = -1;
    };
    
    // ---- Parsing Helpers ----
    static void trimString(char* str);
    static int getIndentLevel(const char* line);
    static bool isCommentOrEmpty(const char* line);
    static bool isListItem(const char* line);
    static bool parseKey(const char* line, char* key, size_t keySize);
    static bool parseValue(const char* line, char* value, size_t valueSize);
    static bool parseBool(const char* value);
    static int parseInt(const char* value);
    static float parseFloat(const char* value);
    
    // ---- Section Parsers ----
    void parseEngineFX(const char* key, const char* value, ParseContext& ctx);
    void parseGunFX(const char* key, const char* value, ParseContext& ctx, int indent, bool listItem);
    void parseServoConfig(const char* key, const char* value, ServoInputConfig& servo);
};

#endif // CONFIG_READER_H
