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
#include "engine_fx.h"
#include "gun_fx.h"

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
     * @brief Initialize with SD card storage
     * @param sd Pointer to SdFat instance
     * @return true if successful
     */
    bool begin(SdFat* sd);
    
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

    // ---- Accessors ----
    
    const HubFXSettings& settings() const { return _settings; }
    const EngineFXSettings& engineSettings() const { return _settings.engine; }
    const GunFXSettings& gunSettings() const { return _settings.gun; }
    bool isLoaded() const { return _settings.loaded; }

    // ---- Debug ----
    void print() const;

private:
    // Storage
    ConfigStorage _storage = ConfigStorage::SD;
    SdFat* _sd = nullptr;
    bool _initialized = false;
    
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
