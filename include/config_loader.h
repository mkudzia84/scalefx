#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <cyaml/cyaml.h>

// Rate of fire configuration
typedef struct RateOfFireConfig {
    char *name;
    int rpm;
    int pwm_threshold_us;
    char *sound_file;
} RateOfFireConfig;

// Servo configuration with defaults
typedef struct ServoConfig {
    bool enabled;
    int pwm_pin;
    int output_pin;
    int input_min_us;           // Default: 1000
    int input_max_us;           // Default: 2000
    int output_min_us;          // Default: 1000
    int output_max_us;          // Default: 2000
    float max_speed_us_per_sec; // Default: 500.0
    float max_accel_us_per_sec2;// Default: 2000.0
    int update_rate_hz;         // Default: 50
} ServoConfig;

// Engine FX configuration with defaults
typedef struct EngineFXConfig {
    bool enabled;
    int pin;
    int threshold_us;          // Default: 1500
    char *starting_file;
    char *running_file;
    char *stopping_file;
    int starting_offset_ms;    // Default: 60000 (60 seconds)
    int stopping_offset_ms;    // Default: 25000 (25 seconds)
} EngineFXConfig;

// Gun FX configuration with defaults
typedef struct GunFXConfig {
    bool enabled;
    int trigger_pin;
    
    bool nozzle_flash_enabled;
    int nozzle_flash_pin;
    
    bool smoke_enabled;
    int smoke_fan_pin;
    int smoke_heater_pin;
    int smoke_heater_toggle_pin;
    int smoke_heater_pwm_threshold_us; // Default: 1500
    int smoke_fan_off_delay_ms;        // Default: 2000
    
    ServoConfig pitch_servo;
    ServoConfig yaw_servo;
    
    RateOfFireConfig *rates;
    int rate_count;
} GunFXConfig;

#ifdef ENABLE_JETIEX
// JetiEX telemetry configuration with defaults
typedef struct JetiEXConfigData {
    bool enabled;
    bool remote_config;
    char *serial_port;
    uint32_t baud_rate;
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint8_t update_rate_hz;  // Default: 5
} JetiEXConfigData;
#endif

// Complete helicopter FX configuration
typedef struct HeliFXConfig {
    EngineFXConfig engine;
    GunFXConfig gun;
#ifdef ENABLE_JETIEX
    JetiEXConfigData jetiex;
#endif
} HeliFXConfig;

/**
 * Load and parse YAML configuration file using libcyaml
 * @param config_file Path to YAML configuration file
 * @return Pointer to loaded configuration, or nullptr on error
 */
HeliFXConfig* config_load(const char *config_file);

/**
 * Validate configuration
 * @param config Configuration to validate
 * @return 0 if valid, -1 if invalid
 */
int config_validate(const HeliFXConfig *config);

/**
 * Print configuration to stdout
 * @param config Configuration to display
 */
void config_print(const HeliFXConfig *config);

/**
 * Free configuration memory (uses cyaml_free)
 * @param config Configuration to free
 */
void config_free(HeliFXConfig *config);

/**
 * Save configuration to YAML file
 * @param config_file Path to YAML configuration file
 * @param config Configuration to save
 * @return 0 on success, -1 on error
 */
int config_save(const char *config_file, const HeliFXConfig *config);

#endif // CONFIG_LOADER_H
