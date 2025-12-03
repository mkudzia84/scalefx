#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <stdbool.h>

// Rate of fire configuration
typedef struct {
    char name[64];
    int rpm;
    int pwm_threshold_us;
    char sound_file[512];
} RateOfFireConfig;

// Engine FX configuration
typedef struct {
    bool enabled;
    int pin;
    int threshold_us;
    char starting_file[512];
    char running_file[512];
    char stopping_file[512];
    int starting_offset_ms;
    int stopping_offset_ms;
} EngineFXConfig;

// Gun FX configuration
typedef struct {
    bool enabled;
    int trigger_pin;
    
    bool nozzle_flash_enabled;
    int nozzle_flash_pin;
    
    bool smoke_enabled;
    int smoke_fan_pin;
    int smoke_heater_pin;
    int smoke_heater_toggle_pin;
    int smoke_heater_pwm_threshold_us;
    int smoke_fan_off_delay_ms;
    
    RateOfFireConfig *rates;
    int rate_count;
} GunFXConfig;

// Complete helicopter FX configuration
typedef struct {
    EngineFXConfig engine;
    GunFXConfig gun;
} HeliFXConfig;

/**
 * Load and parse YAML configuration file
 * @param config_file Path to YAML configuration file
 * @param config Output configuration structure
 * @return 0 on success, -1 on error
 */
int config_load(const char *config_file, HeliFXConfig *config);

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
 * Free dynamically allocated configuration memory
 * @param config Configuration to free
 */
void config_free(HeliFXConfig *config);

#endif // CONFIG_LOADER_H
