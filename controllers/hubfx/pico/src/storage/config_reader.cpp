/**
 * ConfigReader - Implementation
 * 
 * OOP YAML parser for Raspberry Pi Pico.
 * Uses a line-by-line approach suitable for embedded systems.
 */

#include "config_reader.h"

// ============================================================================
//  DEBUG
// ============================================================================

#ifndef CONFIG_DEBUG
#define CONFIG_DEBUG 1
#endif

#if CONFIG_DEBUG
#define LOG(fmt, ...) Serial.printf("[Config] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif

// ============================================================================
//  PARSING HELPERS (static)
// ============================================================================

void ConfigReader::trimString(char* str) {
    if (!str || !*str) return;
    
    // Trim leading
    char* start = str;
    while (*start && isspace(*start)) start++;
    
    // Trim trailing
    char* end = start + strlen(start) - 1;
    while (end > start && isspace(*end)) *end-- = '\0';
    
    // Shift if needed
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

int ConfigReader::getIndentLevel(const char* line) {
    int spaces = 0;
    while (*line == ' ') {
        spaces++;
        line++;
    }
    return spaces / 2;
}

bool ConfigReader::isCommentOrEmpty(const char* line) {
    while (*line && isspace(*line)) line++;
    return *line == '\0' || *line == '#';
}

bool ConfigReader::isListItem(const char* line) {
    while (*line && isspace(*line)) line++;
    return *line == '-';
}

bool ConfigReader::parseKey(const char* line, char* key, size_t keySize) {
    // Skip leading whitespace and list marker
    while (*line && (isspace(*line) || *line == '-')) line++;
    
    const char* colon = strchr(line, ':');
    if (!colon) return false;
    
    size_t keyLen = colon - line;
    if (keyLen >= keySize) keyLen = keySize - 1;
    
    strncpy(key, line, keyLen);
    key[keyLen] = '\0';
    trimString(key);
    
    return true;
}

bool ConfigReader::parseValue(const char* line, char* value, size_t valueSize) {
    const char* colon = strchr(line, ':');
    if (!colon) return false;
    
    colon++; // Move past colon
    while (*colon && isspace(*colon)) colon++; // Skip whitespace
    
    // Empty value (nested object)
    if (*colon == '\0' || *colon == '\n') {
        value[0] = '\0';
        return true;
    }
    
    size_t len = strlen(colon);
    if (len >= valueSize) len = valueSize - 1;
    
    // Handle quoted strings
    if (colon[0] == '"' || colon[0] == '\'') {
        const char* start = colon + 1;
        const char* end = strchr(start, colon[0]);
        if (end) {
            len = end - start;
            if (len >= valueSize) len = valueSize - 1;
        } else {
            len = strlen(start);
        }
        strncpy(value, start, len);
    } else {
        strncpy(value, colon, len);
    }
    value[len] = '\0';
    trimString(value);
    
    return true;
}

bool ConfigReader::parseBool(const char* value) {
    if (!value) return false;
    return (strcasecmp(value, "true") == 0 || 
            strcasecmp(value, "yes") == 0 || 
            strcasecmp(value, "on") == 0 ||
            strcmp(value, "1") == 0);
}

int ConfigReader::parseInt(const char* value) {
    return value ? atoi(value) : 0;
}

float ConfigReader::parseFloat(const char* value) {
    return value ? atof(value) : 0.0f;
}

// ============================================================================
//  INITIALIZATION
// ============================================================================

bool ConfigReader::begin(SdFat* sd) {
    if (!sd) {
        LOG("SD card pointer required");
        return false;
    }
    
    _storage = ConfigStorage::SD;
    _sd = sd;
    _initialized = true;
    
    LOG("Initialized with SD card storage");
    return true;
}

bool ConfigReader::beginFlash() {
    if (!LittleFS.begin()) {
        LOG("LittleFS initialization failed");
        return false;
    }
    
    _storage = ConfigStorage::Flash;
    _initialized = true;
    
    LOG("Initialized with flash storage");
    return true;
}

// ============================================================================
//  DEFAULTS
// ============================================================================

void ConfigReader::loadDefaults() {
    // Clear everything
    memset(&_settings, 0, sizeof(_settings));
    memset(_engineStartingSound, 0, sizeof(_engineStartingSound));
    memset(_engineRunningSound, 0, sizeof(_engineRunningSound));
    memset(_engineStoppingSound, 0, sizeof(_engineStoppingSound));
    memset(_gunSoundFiles, 0, sizeof(_gunSoundFiles));
    
    // ---- Engine FX Defaults ----
    _settings.engine.enabled = false;
    _settings.engine.togglePin = -1;
    _settings.engine.toggleInputType = PwmInputType::None;
    _settings.engine.toggleThresholdUs = 1500;
    _settings.engine.startingOffsetFromStoppingMs = 60000;
    _settings.engine.stoppingOffsetFromStartingMs = 25000;
    _settings.engine.channelStartup = 0;
    _settings.engine.channelRunning = 1;
    _settings.engine.channelShutdown = 2;
    
    // ---- Gun FX Defaults ----
    _settings.gun.enabled = false;
    _settings.gun.triggerChannel = -1;
    _settings.gun.rateCount = 0;
    _settings.gun.audioChannel = 3;
    
    // Smoke defaults
    _settings.gun.smoke.heaterToggleChannel = -1;
    _settings.gun.smoke.heaterThresholdUs = 1500;
    _settings.gun.smoke.fanOffDelayMs = 2000;
    
    // Pitch servo defaults
    _settings.gun.pitch.inputChannel = -1;
    _settings.gun.pitch.servoId = 1;
    _settings.gun.pitch.inputMinUs = 1000;
    _settings.gun.pitch.inputMaxUs = 2000;
    _settings.gun.pitch.outputMinUs = 1000;
    _settings.gun.pitch.outputMaxUs = 2000;
    _settings.gun.pitch.maxSpeedUsPerSec = 4000;
    _settings.gun.pitch.maxAccelUsPerSec2 = 8000;
    _settings.gun.pitch.maxDecelUsPerSec2 = 8000;
    
    // Yaw servo defaults
    _settings.gun.yaw.inputChannel = -1;
    _settings.gun.yaw.servoId = 2;
    _settings.gun.yaw.inputMinUs = 1000;
    _settings.gun.yaw.inputMaxUs = 2000;
    _settings.gun.yaw.outputMinUs = 1000;
    _settings.gun.yaw.outputMaxUs = 2000;
    _settings.gun.yaw.maxSpeedUsPerSec = 4000;
    _settings.gun.yaw.maxAccelUsPerSec2 = 8000;
    _settings.gun.yaw.maxDecelUsPerSec2 = 8000;
    
    _settings.loaded = false;
}

// ============================================================================
//  SAVING (Flash only)
// ============================================================================

bool ConfigReader::save(const char* yaml, size_t length, const char* filename) {
    if (_storage != ConfigStorage::Flash) {
        LOG("Save only supported for flash storage");
        return false;
    }
    
    if (!_initialized) {
        LOG("Not initialized");
        return false;
    }
    
    // Open file for writing
    File file = LittleFS.open(filename, "w");
    if (!file) {
        LOG("Failed to open %s for writing", filename);
        return false;
    }
    
    // Write data
    size_t written = file.write((const uint8_t*)yaml, length);
    file.close();
    
    if (written != length) {
        LOG("Write failed: wrote %d/%d bytes", written, length);
        return false;
    }
    
    LOG("Saved %d bytes to %s", written, filename);
    return true;
}

bool ConfigReader::backup() {
    if (_storage != ConfigStorage::Flash) {
        LOG("Backup only supported for flash storage");
        return false;
    }
    
    // Read current config
    File src = LittleFS.open("/config.yaml", "r");
    if (!src) {
        LOG("No config.yaml to backup");
        return false;
    }
    
    // Create backup file
    File dst = LittleFS.open("/config.yaml.bak", "w");
    if (!dst) {
        src.close();
        LOG("Failed to create backup file");
        return false;
    }
    
    // Copy data
    uint8_t buf[256];
    size_t total = 0;
    while (src.available()) {
        size_t read = src.read(buf, sizeof(buf));
        dst.write(buf, read);
        total += read;
    }
    
    src.close();
    dst.close();
    
    LOG("Backed up %d bytes to config.yaml.bak", total);
    return true;
}

bool ConfigReader::restore() {
    if (_storage != ConfigStorage::Flash) {
        LOG("Restore only supported for flash storage");
        return false;
    }
    
    // Read backup
    File src = LittleFS.open("/config.yaml.bak", "r");
    if (!src) {
        LOG("No backup file found");
        return false;
    }
    
    // Restore to config
    File dst = LittleFS.open("/config.yaml", "w");
    if (!dst) {
        src.close();
        LOG("Failed to open config.yaml for writing");
        return false;
    }
    
    // Copy data
    uint8_t buf[256];
    size_t total = 0;
    while (src.available()) {
        size_t read = src.read(buf, sizeof(buf));
        dst.write(buf, read);
        total += read;
    }
    
    src.close();
    dst.close();
    
    LOG("Restored %d bytes from backup", total);
    return true;
}

int ConfigReader::getSize(const char* filename) {
    File file;
    
    if (_storage == ConfigStorage::Flash) {
        file = LittleFS.open(filename, "r");
    } else {
        File32 f;
        if (!f.open(_sd, filename, O_RDONLY)) {
            return -1;
        }
        return f.size();
    }
    
    if (!file) return -1;
    
    int size = file.size();
    file.close();
    return size;
}

// ============================================================================
//  SECTION PARSERS
// ============================================================================

void ConfigReader::parseServoConfig(const char* key, const char* value, ServoInputConfig& servo) {
    if (strcmp(key, "servo_id") == 0) {
        servo.servoId = parseInt(value);
    } else if (strcmp(key, "input_channel") == 0) {
        servo.inputChannel = parseInt(value);
    } else if (strcmp(key, "input_min_us") == 0) {
        servo.inputMinUs = parseInt(value);
    } else if (strcmp(key, "input_max_us") == 0) {
        servo.inputMaxUs = parseInt(value);
    } else if (strcmp(key, "output_min_us") == 0) {
        servo.outputMinUs = parseInt(value);
    } else if (strcmp(key, "output_max_us") == 0) {
        servo.outputMaxUs = parseInt(value);
    } else if (strcmp(key, "max_speed_us_per_sec") == 0) {
        servo.maxSpeedUsPerSec = parseInt(value);
    } else if (strcmp(key, "max_accel_us_per_sec2") == 0) {
        servo.maxAccelUsPerSec2 = parseInt(value);
    } else if (strcmp(key, "max_decel_us_per_sec2") == 0) {
        servo.maxDecelUsPerSec2 = parseInt(value);
    } else if (strcmp(key, "recoil_jerk_us") == 0) {
        servo.recoilJerkUs = parseInt(value);
    } else if (strcmp(key, "recoil_jerk_variance_us") == 0) {
        servo.recoilJerkVarianceUs = parseInt(value);
    }
}

void ConfigReader::parseEngineFX(const char* key, const char* value, ParseContext& ctx) {
    // Level 1: subsection names or type
    if (ctx.subsection[0] == '\0') {
        if (strcmp(key, "enabled") == 0) {
            _settings.engine.enabled = parseBool(value);
        }
    }
    // Level 2: engine_toggle fields
    else if (strcmp(ctx.subsection, "engine_toggle") == 0 && ctx.subsubsection[0] == '\0') {
        if (strcmp(key, "input_channel") == 0) {
            _settings.engine.togglePin = parseInt(value);  // Will be mapped to actual pin
            _settings.engine.toggleInputType = PwmInputType::Pwm;
        } else if (strcmp(key, "threshold_us") == 0) {
            _settings.engine.toggleThresholdUs = parseInt(value);
        }
    }
    // Level 2: sounds fields
    else if (strcmp(ctx.subsection, "sounds") == 0 && ctx.subsubsection[0] == '\0') {
        if (strcmp(key, "starting") == 0) {
            strncpy(_engineStartingSound, value, ConfigReaderConfig::MAX_PATH_LENGTH - 1);
            _settings.engine.startupSound.filename = _engineStartingSound;
        } else if (strcmp(key, "running") == 0) {
            strncpy(_engineRunningSound, value, ConfigReaderConfig::MAX_PATH_LENGTH - 1);
            _settings.engine.runningSound.filename = _engineRunningSound;
        } else if (strcmp(key, "stopping") == 0) {
            strncpy(_engineStoppingSound, value, ConfigReaderConfig::MAX_PATH_LENGTH - 1);
            _settings.engine.shutdownSound.filename = _engineStoppingSound;
        }
    }
    // Level 3: sounds.transitions fields
    else if (strcmp(ctx.subsection, "sounds") == 0 && strcmp(ctx.subsubsection, "transitions") == 0) {
        if (strcmp(key, "starting_offset_ms") == 0) {
            _settings.engine.startingOffsetFromStoppingMs = parseInt(value);
        } else if (strcmp(key, "stopping_offset_ms") == 0) {
            _settings.engine.stoppingOffsetFromStartingMs = parseInt(value);
        }
    }
}

void ConfigReader::parseGunFX(const char* key, const char* value, ParseContext& ctx, int indent, bool listItem) {
    // Level 1: enabled flag
    if (ctx.subsection[0] == '\0') {
        if (strcmp(key, "enabled") == 0) {
            _settings.gun.enabled = parseBool(value);
        }
    }
    // Level 2: trigger fields
    else if (strcmp(ctx.subsection, "trigger") == 0 && ctx.subsubsection[0] == '\0') {
        if (strcmp(key, "input_channel") == 0) {
            _settings.gun.triggerChannel = parseInt(value);
        }
    }
    // Level 2: smoke fields
    else if (strcmp(ctx.subsection, "smoke") == 0 && ctx.subsubsection[0] == '\0') {
        if (strcmp(key, "heater_toggle_channel") == 0) {
            _settings.gun.smoke.heaterToggleChannel = parseInt(value);
        } else if (strcmp(key, "heater_pwm_threshold_us") == 0) {
            _settings.gun.smoke.heaterThresholdUs = parseInt(value);
        } else if (strcmp(key, "fan_off_delay_ms") == 0) {
            _settings.gun.smoke.fanOffDelayMs = parseInt(value);
        }
    }
    // Level 2/3: rates_of_fire list
    else if (strcmp(ctx.subsection, "rates_of_fire") == 0) {
        // New list item
        if (listItem && indent == 2) {
            ctx.listIndex++;
            if (ctx.listIndex >= GunFXConfig::MAX_RATES_OF_FIRE) return;
            _settings.gun.rateCount = ctx.listIndex + 1;
        }
        
        if (ctx.listIndex >= 0 && ctx.listIndex < GunFXConfig::MAX_RATES_OF_FIRE) {
            RateOfFireConfig& rate = _settings.gun.ratesOfFire[ctx.listIndex];
            if (strcmp(key, "rpm") == 0) {
                rate.rpm = parseInt(value);
            } else if (strcmp(key, "pwm_threshold_us") == 0) {
                rate.pwmThresholdUs = parseInt(value);
            } else if (strcmp(key, "sound_file") == 0) {
                strncpy(_gunSoundFiles[ctx.listIndex], value, ConfigReaderConfig::MAX_PATH_LENGTH - 1);
                rate.soundFile = _gunSoundFiles[ctx.listIndex];
            } else if (strcmp(key, "sound_volume") == 0) {
                rate.soundVolume = parseFloat(value);
            }
        }
    }
    // Level 3: turret_control.pitch or turret_control.yaw
    else if (strcmp(ctx.subsection, "turret_control") == 0) {
        if (strcmp(ctx.subsubsection, "pitch") == 0) {
            parseServoConfig(key, value, _settings.gun.pitch);
        } else if (strcmp(ctx.subsubsection, "yaw") == 0) {
            parseServoConfig(key, value, _settings.gun.yaw);
        }
    }
}

// ============================================================================
//  LOAD
// ============================================================================

bool ConfigReader::load(const char* filename) {
    if (!filename || !_initialized) return false;
    
    File32 file;
    
    if (_storage == ConfigStorage::SD) {
        if (!_sd) return false;
        file = _sd->open(filename, FILE_READ);
        if (!file) {
            LOG("Failed to open: %s", filename);
            return false;
        }
    } else {
        return false; // LittleFS not supported yet - needs proper File wrapper
    }
    
    if (!file) {
        LOG("File invalid after opening: %s", filename);
        return false;
    }
    
    LOG("Loading: %s", filename);
    
    // Load defaults first
    loadDefaults();
    
    // Parse line by line
    char line[ConfigReaderConfig::MAX_LINE_LENGTH];
    ParseContext ctx = {};
    ctx.listIndex = -1;
    int lineNum = 0;
    
    while (file.available()) {
        // Read line
        int i = 0;
        while (file.available() && i < (int)sizeof(line) - 1) {
            char c = file.read();
            if (c == '\n' || c == '\r') {
                if (c == '\r' && file.peek() == '\n') file.read();
                break;
            }
            line[i++] = c;
        }
        line[i] = '\0';
        lineNum++;
        
        // Skip empty/comment lines
        if (isCommentOrEmpty(line)) continue;
        
        // Get indent and key/value
        int indent = getIndentLevel(line);
        bool listItem = isListItem(line);
        char key[64], value[256];
        
        if (!parseKey(line, key, sizeof(key))) continue;
        parseValue(line, value, sizeof(value));
        
        // Track section context based on indent
        if (indent == 0) {
            strncpy(ctx.section, key, sizeof(ctx.section) - 1);
            ctx.subsection[0] = '\0';
            ctx.subsubsection[0] = '\0';
            ctx.listIndex = -1;
        }
        else if (indent == 1) {
            strncpy(ctx.subsection, key, sizeof(ctx.subsection) - 1);
            ctx.subsubsection[0] = '\0';
            if (strcmp(key, "rates_of_fire") != 0) {
                ctx.listIndex = -1;
            }
        }
        else if (indent == 2 && !listItem) {
            // Check if this is a subsubsection start
            if (value[0] == '\0') {
                strncpy(ctx.subsubsection, key, sizeof(ctx.subsubsection) - 1);
            }
        }
        
        // Route to section parser
        if (strcmp(ctx.section, "engine_fx") == 0) {
            parseEngineFX(key, value, ctx);
        }
        else if (strcmp(ctx.section, "gun_fx") == 0) {
            parseGunFX(key, value, ctx, indent, listItem);
        }
    }
    
    file.close();
    _settings.loaded = true;
    
    LOG("Loaded %d lines", lineNum);
    return true;
}

// ============================================================================
//  DEBUG PRINT
// ============================================================================

void ConfigReader::print() const {
    Serial.println("=== HubFX Configuration ===");
    
    // Engine FX
    Serial.println("Engine FX:");
    Serial.printf("  Enabled: %s\n", _settings.engine.enabled ? "yes" : "no");
    Serial.printf("  Toggle: pin=%d, threshold=%d us\n",
                  _settings.engine.togglePin,
                  _settings.engine.toggleThresholdUs);
    Serial.println("  Sounds:");
    if (_settings.engine.startupSound.filename) {
        Serial.printf("    Starting: %s\n", _settings.engine.startupSound.filename);
    }
    if (_settings.engine.runningSound.filename) {
        Serial.printf("    Running: %s\n", _settings.engine.runningSound.filename);
    }
    if (_settings.engine.shutdownSound.filename) {
        Serial.printf("    Stopping: %s\n", _settings.engine.shutdownSound.filename);
    }
    Serial.printf("  Transitions: start_offset=%d ms, stop_offset=%d ms\n",
                  _settings.engine.startingOffsetFromStoppingMs,
                  _settings.engine.stoppingOffsetFromStartingMs);
    
    // Gun FX
    Serial.println("Gun FX:");
    Serial.printf("  Enabled: %s\n", _settings.gun.enabled ? "yes" : "no");
    Serial.printf("  Trigger channel: %d\n", _settings.gun.triggerChannel);
    Serial.printf("  Smoke: heater_ch=%d, threshold=%d us, fan_delay=%d ms\n",
                  _settings.gun.smoke.heaterToggleChannel,
                  _settings.gun.smoke.heaterThresholdUs,
                  _settings.gun.smoke.fanOffDelayMs);
    
    // Rates of fire
    Serial.printf("  Rates of fire: %d\n", _settings.gun.rateCount);
    for (int i = 0; i < _settings.gun.rateCount; i++) {
        const RateOfFireConfig& rate = _settings.gun.ratesOfFire[i];
        Serial.printf("    [%d] %d RPM @ %d us\n", i, rate.rpm, rate.pwmThresholdUs);
        if (rate.soundFile) {
            Serial.printf("        Sound: %s (vol=%.1f)\n", rate.soundFile, rate.soundVolume);
        }
    }
    
    // Turret
    Serial.println("  Turret Control:");
    Serial.printf("    Pitch: servo=%d, input_ch=%d, range=%d-%d us\n",
                  _settings.gun.pitch.servoId,
                  _settings.gun.pitch.inputChannel,
                  _settings.gun.pitch.outputMinUs,
                  _settings.gun.pitch.outputMaxUs);
    Serial.printf("    Yaw: servo=%d, input_ch=%d, range=%d-%d us\n",
                  _settings.gun.yaw.servoId,
                  _settings.gun.yaw.inputChannel,
                  _settings.gun.yaw.outputMinUs,
                  _settings.gun.yaw.outputMaxUs);
    
    Serial.println("===========================");
}
