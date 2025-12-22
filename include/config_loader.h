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
    int servo_id;               // Pico servo ID (1, 2, or 3)
    int input_channel;          // Input channel 1-12
    int input_min_us;           // Default: 1000
    int input_max_us;           // Default: 2000
    int output_min_us;          // Default: 1000
    int output_max_us;          // Default: 2000
    float max_speed_us_per_sec; // Default: 4000.0
    float max_accel_us_per_sec2;// Default: 8000.0
    float max_decel_us_per_sec2;// Default: 8000.0
    int recoil_jerk_us;         // Recoil jerk offset per shot (optional, 0=disabled)
    int recoil_jerk_variance_us;// Random variance for recoil jerk (optional)
} ServoConfig;

// Engine Toggle configuration
typedef struct EngineToggleConfig {
    int input_channel;         // Input channel 1-12
    int threshold_us;          // Default: 1500
} EngineToggleConfig;

// Gun Trigger configuration
typedef struct TriggerConfig {
    int input_channel;         // Input channel 1-12
} TriggerConfig;// Engine Sounds Transitions configuration
typedef struct EngineSoundsTransitionsConfig {
    int starting_offset_ms;    // Default: 60000 (60 seconds)
    int stopping_offset_ms;    // Default: 25000 (25 seconds)
} EngineSoundsTransitionsConfig;

// Engine Sounds configuration
typedef struct EngineSoundsConfig {
    char *starting;
    char *running;
    char *stopping;
    EngineSoundsTransitionsConfig transitions;
} EngineSoundsConfig;

// Engine type enumeration
typedef enum {
    ENGINE_TYPE_TURBINE = 0,
    ENGINE_TYPE_RADIAL,      // Future
    ENGINE_TYPE_DIESEL,      // Future
    ENGINE_TYPE_COUNT
} EngineType;

// Engine FX configuration with defaults
typedef struct EngineFXConfig {
    char *type;               // Engine type: "turbine", "radial", "diesel" (default: turbine)
    EngineToggleConfig engine_toggle;
    EngineSoundsConfig sounds;
} EngineFXConfig;

// Smoke configuration
typedef struct SmokeConfig {
    int heater_toggle_channel; // Input channel 1-12
    int heater_pwm_threshold_us; // Default: 1500
    int fan_off_delay_ms;        // Default: 2000
} SmokeConfig;

// Turret Control configuration
typedef struct TurretControlConfig {
    ServoConfig pitch;
    ServoConfig yaw;
} TurretControlConfig;

// Gun FX configuration with defaults
typedef struct GunFXConfig {
    TriggerConfig trigger;
    SmokeConfig smoke;
    TurretControlConfig turret_control;
    RateOfFireConfig *rates;
    int rate_count;
} GunFXConfig;

// Complete ScaleFX configuration
typedef struct ScaleFXConfig {
    EngineFXConfig engine;
    GunFXConfig gun;
} ScaleFXConfig;

/**
 * Load and parse YAML configuration file using libcyaml
 * @param config_file Path to YAML configuration file
 * @return Pointer to loaded configuration, or nullptr on error
 */
ScaleFXConfig* config_load(const char *config_file);

/**
 * Validate configuration
 * @param config Configuration to validate
 * @return 0 if valid, -1 if invalid
 */
int config_validate(const ScaleFXConfig *config);

/**
 * Print configuration to stdout
 * @param config Configuration to display
 */
void config_print(const ScaleFXConfig *config);

/**
 * Free configuration memory (uses cyaml_free)
 * @param config Configuration to free
 */
void config_free(ScaleFXConfig *config);

/**
 * Save configuration to YAML file
 * @param config_file Path to YAML configuration file
 * @param config Configuration to save
 * @return 0 on success, -1 on error
 */
int config_save(const char *config_file, const ScaleFXConfig *config);

#endif // CONFIG_LOADER_H
