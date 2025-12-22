/**
 * Configuration loader using libcyaml for schema-based YAML parsing
 * 
 * This implementation replaces manual libyaml parsing with libcyaml's
 * automatic schema-based approach, reducing code complexity significantly.
 */

#include "config_loader.h"
#include "logging.h"
#include "gpio.h"
#include <cyaml/cyaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ============================================================================
 * Default Values for Optional Configuration Fields
 * ============================================================================ */

// Engine FX Defaults
#define DEFAULT_ENGINE_STARTING_OFFSET_MS   60000   // 60 seconds
#define DEFAULT_ENGINE_STOPPING_OFFSET_MS   25000   // 25 seconds
#define DEFAULT_ENGINE_THRESHOLD_US         1500    // PWM threshold

// Gun FX - Smoke Defaults
#define DEFAULT_SMOKE_FAN_OFF_DELAY_MS      2000    // 2 seconds
#define DEFAULT_SMOKE_HEATER_THRESHOLD_US   1500    // PWM threshold

// Gun FX - Serial Bus Defaults
#define DEFAULT_SERIAL_BAUD_RATE            115200
#define DEFAULT_SERIAL_TIMEOUT_MS           100

// Servo Defaults
#define DEFAULT_SERVO_INPUT_MIN_US          1000    // Standard RC PWM min
#define DEFAULT_SERVO_INPUT_MAX_US          2000    // Standard RC PWM max
#define DEFAULT_SERVO_OUTPUT_MIN_US         1000    // Standard servo min
#define DEFAULT_SERVO_OUTPUT_MAX_US         2000    // Standard servo max
#define DEFAULT_SERVO_MAX_SPEED_US_PER_SEC  4000.0f // 4000 µs/sec
#define DEFAULT_SERVO_MAX_ACCEL_US_PER_SEC2 8000.0f // 8000 µs/sec²
#define DEFAULT_SERVO_MAX_DECEL_US_PER_SEC2 8000.0f // 8000 µs/sec²

/* ============================================================================
 * CYAML Schema Definitions
 * ============================================================================ */

// ServoConfig schema with defaults
static const cyaml_schema_field_t servo_fields[] = {
    CYAML_FIELD_INT("servo_id", CYAML_FLAG_DEFAULT, ServoConfig, servo_id),
    CYAML_FIELD_INT("input_channel", CYAML_FLAG_DEFAULT, ServoConfig, input_channel),
    CYAML_FIELD_INT("input_min_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, input_min_us),
    CYAML_FIELD_INT("input_max_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, input_max_us),
    CYAML_FIELD_INT("output_min_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, output_min_us),
    CYAML_FIELD_INT("output_max_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, output_max_us),
    CYAML_FIELD_FLOAT("max_speed_us_per_sec", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, max_speed_us_per_sec),
    CYAML_FIELD_FLOAT("max_accel_us_per_sec2", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, max_accel_us_per_sec2),
    CYAML_FIELD_FLOAT("max_decel_us_per_sec2", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, max_decel_us_per_sec2),
    CYAML_FIELD_INT("recoil_jerk_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, recoil_jerk_us),
    CYAML_FIELD_INT("recoil_jerk_variance_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, recoil_jerk_variance_us),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t servo_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, ServoConfig, servo_fields),
};

// EngineToggleConfig schema
static const cyaml_schema_field_t engine_toggle_config_fields[] = {
    CYAML_FIELD_INT("input_channel", CYAML_FLAG_DEFAULT, EngineToggleConfig, input_channel),
    CYAML_FIELD_INT("threshold_us", CYAML_FLAG_DEFAULT, EngineToggleConfig, threshold_us),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_toggle_config_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, EngineToggleConfig, engine_toggle_config_fields),
};

// EngineSoundsTransitionsConfig schema
static const cyaml_schema_field_t engine_sounds_transitions_config_fields[] = {
    CYAML_FIELD_INT("starting_offset_ms", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, EngineSoundsTransitionsConfig, starting_offset_ms),
    CYAML_FIELD_INT("stopping_offset_ms", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, EngineSoundsTransitionsConfig, stopping_offset_ms),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_sounds_transitions_config_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, EngineSoundsTransitionsConfig, engine_sounds_transitions_config_fields),
};

// EngineSoundsConfig schema
static const cyaml_schema_field_t engine_sounds_config_fields[] = {
    CYAML_FIELD_STRING_PTR("starting", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, EngineSoundsConfig, starting, 0, CYAML_UNLIMITED),
    CYAML_FIELD_STRING_PTR("running", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, EngineSoundsConfig, running, 0, CYAML_UNLIMITED),
    CYAML_FIELD_STRING_PTR("stopping", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, EngineSoundsConfig, stopping, 0, CYAML_UNLIMITED),
    CYAML_FIELD_MAPPING("transitions", CYAML_FLAG_OPTIONAL, EngineSoundsConfig, transitions, engine_sounds_transitions_config_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_sounds_config_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, EngineSoundsConfig, engine_sounds_config_fields),
};

// RateOfFireConfig schema
static const cyaml_schema_field_t rate_of_fire_fields[] = {
    CYAML_FIELD_STRING_PTR("name", CYAML_FLAG_POINTER, RateOfFireConfig, name, 0, CYAML_UNLIMITED),
    CYAML_FIELD_INT("rpm", CYAML_FLAG_DEFAULT, RateOfFireConfig, rpm),
    CYAML_FIELD_INT("pwm_threshold_us", CYAML_FLAG_DEFAULT, RateOfFireConfig, pwm_threshold_us),
    CYAML_FIELD_STRING_PTR("sound_file", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, RateOfFireConfig, sound_file, 0, CYAML_UNLIMITED),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t rate_of_fire_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, RateOfFireConfig, rate_of_fire_fields),
};

// Trigger configuration schema
static const cyaml_schema_field_t trigger_config_fields[] = {
    CYAML_FIELD_INT("input_channel", CYAML_FLAG_DEFAULT, TriggerConfig, input_channel),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t trigger_config_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, TriggerConfig, trigger_config_fields),
};

// SmokeConfig schema
static const cyaml_schema_field_t smoke_config_fields[] = {
    CYAML_FIELD_INT("heater_toggle_channel", CYAML_FLAG_DEFAULT, SmokeConfig, heater_toggle_channel),
    CYAML_FIELD_INT("heater_pwm_threshold_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, SmokeConfig, heater_pwm_threshold_us),
    CYAML_FIELD_INT("fan_off_delay_ms", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, SmokeConfig, fan_off_delay_ms),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t smoke_config_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, SmokeConfig, smoke_config_fields),
};

// TurretControlConfig schema
static const cyaml_schema_field_t turret_control_config_fields[] = {
    CYAML_FIELD_MAPPING("pitch", CYAML_FLAG_DEFAULT, TurretControlConfig, pitch, servo_fields),
    CYAML_FIELD_MAPPING("yaw", CYAML_FLAG_DEFAULT, TurretControlConfig, yaw, servo_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t turret_control_config_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, TurretControlConfig, turret_control_config_fields),
};

// EngineFXConfig schema
static const cyaml_schema_field_t engine_fx_fields[] = {
    CYAML_FIELD_STRING_PTR("type", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, EngineFXConfig, type, 0, CYAML_UNLIMITED),
    CYAML_FIELD_MAPPING("engine_toggle", CYAML_FLAG_DEFAULT, EngineFXConfig, engine_toggle, engine_toggle_config_fields),
    CYAML_FIELD_MAPPING("sounds", CYAML_FLAG_DEFAULT, EngineFXConfig, sounds, engine_sounds_config_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_fx_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, EngineFXConfig, engine_fx_fields),
};

// GunFXConfig schema
static const cyaml_schema_field_t gun_fx_fields[] = {
    CYAML_FIELD_MAPPING("trigger", CYAML_FLAG_DEFAULT, GunFXConfig, trigger, trigger_config_fields),
    CYAML_FIELD_MAPPING("smoke", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, GunFXConfig, smoke, smoke_config_fields),
    CYAML_FIELD_MAPPING("turret_control", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, GunFXConfig, turret_control, turret_control_config_fields),
    CYAML_FIELD_SEQUENCE_COUNT("rates_of_fire", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, GunFXConfig, rates, rate_count, &rate_of_fire_schema, 0, CYAML_UNLIMITED),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t gun_fx_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, GunFXConfig, gun_fx_fields),
};



// Root ScaleFXConfig schema
static const cyaml_schema_field_t scalefx_config_fields[] = {
    // Make both modules optional; missing sections imply disabled
    CYAML_FIELD_MAPPING("engine_fx", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ScaleFXConfig, engine, engine_fx_fields),
    CYAML_FIELD_MAPPING("gun_fx", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ScaleFXConfig, gun, gun_fx_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t scalefx_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, ScaleFXConfig, scalefx_config_fields),
};

/* ============================================================================
 * CYAML Configuration
 * ============================================================================ */

static const cyaml_config_t cyaml_config = {
    .log_fn = cyaml_log,
    .mem_fn = cyaml_mem,
    .log_level = CYAML_LOG_WARNING,
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

// Apply defaults to fields that CYAML set to 0 (using designated initializers pattern)
#define APPLY_DEFAULT_IF_ZERO(field, default_value) \
    if ((field) == 0) (field) = (default_value)

static inline void apply_defaults_inline(ScaleFXConfig *config) {
    // Engine defaults
    APPLY_DEFAULT_IF_ZERO(config->engine.engine_toggle.threshold_us, DEFAULT_ENGINE_THRESHOLD_US);
    APPLY_DEFAULT_IF_ZERO(config->engine.sounds.transitions.starting_offset_ms, DEFAULT_ENGINE_STARTING_OFFSET_MS);
    APPLY_DEFAULT_IF_ZERO(config->engine.sounds.transitions.stopping_offset_ms, DEFAULT_ENGINE_STOPPING_OFFSET_MS);
    
    // Gun - Smoke defaults
    APPLY_DEFAULT_IF_ZERO(config->gun.smoke.heater_pwm_threshold_us, DEFAULT_SMOKE_HEATER_THRESHOLD_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.smoke.fan_off_delay_ms, DEFAULT_SMOKE_FAN_OFF_DELAY_MS);
    
    // Gun - Pitch servo defaults
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.pitch.input_min_us, DEFAULT_SERVO_INPUT_MIN_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.pitch.input_max_us, DEFAULT_SERVO_INPUT_MAX_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.pitch.output_min_us, DEFAULT_SERVO_OUTPUT_MIN_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.pitch.output_max_us, DEFAULT_SERVO_OUTPUT_MAX_US);
    if (config->gun.turret_control.pitch.max_speed_us_per_sec == 0.0f)
        config->gun.turret_control.pitch.max_speed_us_per_sec = DEFAULT_SERVO_MAX_SPEED_US_PER_SEC;
    if (config->gun.turret_control.pitch.max_accel_us_per_sec2 == 0.0f)
        config->gun.turret_control.pitch.max_accel_us_per_sec2 = DEFAULT_SERVO_MAX_ACCEL_US_PER_SEC2;
    if (config->gun.turret_control.pitch.max_decel_us_per_sec2 == 0.0f)
        config->gun.turret_control.pitch.max_decel_us_per_sec2 = DEFAULT_SERVO_MAX_DECEL_US_PER_SEC2;
    
    // Gun - Yaw servo defaults
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.input_min_us, DEFAULT_SERVO_INPUT_MIN_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.input_max_us, DEFAULT_SERVO_INPUT_MAX_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.output_min_us, DEFAULT_SERVO_OUTPUT_MIN_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.output_max_us, DEFAULT_SERVO_OUTPUT_MAX_US);
    if (config->gun.turret_control.yaw.max_speed_us_per_sec == 0.0f)
        config->gun.turret_control.yaw.max_speed_us_per_sec = DEFAULT_SERVO_MAX_SPEED_US_PER_SEC;
    if (config->gun.turret_control.yaw.max_accel_us_per_sec2 == 0.0f)
        config->gun.turret_control.yaw.max_accel_us_per_sec2 = DEFAULT_SERVO_MAX_ACCEL_US_PER_SEC2;
    if (config->gun.turret_control.yaw.max_decel_us_per_sec2 == 0.0f)
        config->gun.turret_control.yaw.max_decel_us_per_sec2 = DEFAULT_SERVO_MAX_DECEL_US_PER_SEC2;
}

/* ============================================================================
 * Public Functions
 * ============================================================================ */

ScaleFXConfig* config_load(const char *config_file) {
    cyaml_err_t err;
    ScaleFXConfig *config = nullptr;

    LOG_INFO(LOG_CONFIG, "Loading configuration from: %s", config_file);

    err = cyaml_load_file(config_file, &cyaml_config, &scalefx_config_schema,
                          (cyaml_data_t **)&config, nullptr);

    if (err != CYAML_OK) {
        LOG_ERROR(LOG_CONFIG, "Failed to load config: %s", cyaml_strerror(err));
        return nullptr;
    }

    // Apply defaults for optional fields that weren't in the YAML
    apply_defaults_inline(config);

    LOG_INFO(LOG_CONFIG, "Configuration loaded successfully");
    return config;
}

int config_save(const char *config_file, const ScaleFXConfig *config) {
    cyaml_err_t err;

    LOG_INFO(LOG_CONFIG, "Saving configuration to: %s", config_file);

    err = cyaml_save_file(config_file, &cyaml_config, &scalefx_config_schema,
                          (cyaml_data_t *)config, 0);

    if (err != CYAML_OK) {
        LOG_ERROR(LOG_CONFIG, "Failed to save config: %s", cyaml_strerror(err));
        return -1;
    }

    LOG_INFO(LOG_CONFIG, "Configuration saved successfully");
    return 0;
}

void config_free(ScaleFXConfig *config) {
    if (config) {
        cyaml_free(&cyaml_config, &scalefx_config_schema, config, 0);
    }
}

int config_validate(const ScaleFXConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_CONFIG, "Validation failed: nullptr config");
        return -1;
    }

    // Detect if engine section is present (optional)
    bool engine_present = (config->engine.engine_toggle.input_channel != 0) ||
                          (config->engine.sounds.starting != NULL) ||
                          (config->engine.sounds.running != NULL) ||
                          (config->engine.sounds.stopping != NULL);

    // Engine validation (only if present)
    if (engine_present) {
        if (!is_valid_channel(config->engine.engine_toggle.input_channel)) {
            LOG_ERROR(LOG_CONFIG, "Invalid engine input_channel: %d (must be 1-12)", 
                      config->engine.engine_toggle.input_channel);
            return -1;
        }
        // Engine sounds are optional; no strict validation
    }

    // Detect if gun section is present (optional)
    bool gun_present = (config->gun.trigger.input_channel != 0) ||
                       (config->gun.rate_count > 0) ||
                       (config->gun.turret_control.pitch.input_channel != 0) ||
                       (config->gun.turret_control.yaw.input_channel != 0) ||
                       (config->gun.smoke.heater_toggle_channel != 0);

    // Gun validation (only if present)
    if (gun_present) {
        if (!is_valid_channel(config->gun.trigger.input_channel)) {
            LOG_ERROR(LOG_CONFIG, "Invalid gun trigger input_channel: %d (must be 1-12)", 
                      config->gun.trigger.input_channel);
            return -1;
        }
        
        // Validate smoke heater channel if present
        if (config->gun.smoke.heater_toggle_channel != 0 && 
            !is_valid_channel(config->gun.smoke.heater_toggle_channel)) {
            LOG_ERROR(LOG_CONFIG, "Invalid smoke heater_toggle_channel: %d (must be 1-12)", 
                      config->gun.smoke.heater_toggle_channel);
            return -1;
        }
        // Serial bus is auto-detected over USB; no config validation needed
    }

    // Validate servo inputs and servo_id (outputs handled by Pico)
    if (config->gun.turret_control.pitch.input_channel > 0) {
        if (!is_valid_channel(config->gun.turret_control.pitch.input_channel)) {
            LOG_ERROR(LOG_CONFIG, "Invalid pitch input_channel: %d (must be 1-12)",
                      config->gun.turret_control.pitch.input_channel);
            return -1;
        }
        if (config->gun.turret_control.pitch.servo_id <= 0) {
            LOG_ERROR(LOG_CONFIG, "Invalid pitch servo_id: must be > 0");
            return -1;
        }
    }
    if (config->gun.turret_control.yaw.input_channel > 0) {
        if (!is_valid_channel(config->gun.turret_control.yaw.input_channel)) {
            LOG_ERROR(LOG_CONFIG, "Invalid yaw input_channel: %d (must be 1-12)",
                      config->gun.turret_control.yaw.input_channel);
            return -1;
        }
        if (config->gun.turret_control.yaw.servo_id <= 0) {
            LOG_ERROR(LOG_CONFIG, "Invalid yaw servo_id: must be > 0");
            return -1;
        }
    }
    
    // Validate rates of fire
    if (config->gun.rate_count > 0 && !config->gun.rates) {
        LOG_ERROR(LOG_CONFIG, "Invalid rates of fire configuration");
        return -1;
    }

    LOG_INFO(LOG_CONFIG, "Configuration validation passed");
    return 0;
}

void config_print(const ScaleFXConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_CONFIG, "Cannot print nullptr config");
        return;
    }

    // ANSI color codes for pretty printing
    #define COLOR_RESET   "\033[0m"
    #define COLOR_BOLD    "\033[1m"
    #define COLOR_CYAN    "\033[36m"
    #define COLOR_GREEN   "\033[32m"
    #define COLOR_YELLOW  "\033[33m"
    #define COLOR_MAGENTA "\033[35m"
    #define COLOR_BLUE    "\033[34m"
    
    printf("\n");
    printf(COLOR_CYAN COLOR_BOLD "╔════════════════════════════════════════════════════════════════╗\n" COLOR_RESET);
    printf(COLOR_CYAN COLOR_BOLD "║         ScaleFX Configuration Loaded Successfully               ║\n" COLOR_RESET);
    printf(COLOR_CYAN COLOR_BOLD "╚════════════════════════════════════════════════════════════════╝\n" COLOR_RESET);
    printf("\n");
    
    // Engine FX (optional)
    bool engine_present = (config->engine.engine_toggle.input_channel != 0) ||
                          (config->engine.sounds.starting != NULL) ||
                          (config->engine.sounds.running != NULL) ||
                          (config->engine.sounds.stopping != NULL);
    if (engine_present) {
        int gpio = channel_to_gpio(config->engine.engine_toggle.input_channel);
        printf(COLOR_GREEN "✓ Engine FX" COLOR_RESET " | Channel: %d (GPIO %d), Threshold: %d µs\\n", 
               config->engine.engine_toggle.input_channel,
               gpio,
               config->engine.engine_toggle.threshold_us);
    
    // Sound files
    bool has_sounds = config->engine.sounds.starting || 
                     config->engine.sounds.running || 
                     config->engine.sounds.stopping;
    if (has_sounds) {
        printf("    Sounds: ");
        if (config->engine.sounds.starting) printf("[START] ");
        if (config->engine.sounds.running) printf("[RUN] ");
        if (config->engine.sounds.stopping) printf("[STOP]");
        printf("\n");
    }
    printf("\n");
    } else {
        printf(COLOR_YELLOW "✗ Engine FX" COLOR_RESET " (disabled)\n\n");
    }
    
    // Gun FX (optional)
    bool gun_present = (config->gun.trigger.input_channel != 0) ||
                       (config->gun.rate_count > 0) ||
                       (config->gun.turret_control.pitch.input_channel != 0) ||
                       (config->gun.turret_control.yaw.input_channel != 0) ||
                       (config->gun.smoke.heater_toggle_channel != 0);
    if (gun_present) {
        int trigger_gpio = channel_to_gpio(config->gun.trigger.input_channel);
        printf(COLOR_GREEN "✓ Gun FX" COLOR_RESET " | Trigger: Channel %d (GPIO %d)\\n", 
               config->gun.trigger.input_channel, trigger_gpio);
    
    // Smoke Generator
    int smoke_gpio = channel_to_gpio(config->gun.smoke.heater_toggle_channel);
    printf("    " COLOR_MAGENTA "Smoke" COLOR_RESET ": Channel=%d (GPIO %d), Threshold=%d µs, Fan_delay=%d ms\\n", 
           config->gun.smoke.heater_toggle_channel,
           smoke_gpio,
           config->gun.smoke.heater_pwm_threshold_us,
           config->gun.smoke.fan_off_delay_ms);
    
    // Turret/Servo Control
    if (config->gun.turret_control.pitch.input_channel > 0 || config->gun.turret_control.yaw.input_channel > 0) {
        printf("    " COLOR_BLUE "Servos" COLOR_RESET ":\n");
        
        if (config->gun.turret_control.pitch.input_channel > 0) {
            int pitch_gpio = channel_to_gpio(config->gun.turret_control.pitch.input_channel);
            printf("      • Pitch: Servo ID=%d, Channel=%d (GPIO %d), Speed=%d µs/s, Accel=%d µs/s², Decel=%d µs/s²\n", 
                   config->gun.turret_control.pitch.servo_id,
                   config->gun.turret_control.pitch.input_channel,
                   pitch_gpio,
                   (int)config->gun.turret_control.pitch.max_speed_us_per_sec,
                   (int)config->gun.turret_control.pitch.max_accel_us_per_sec2,
                   (int)config->gun.turret_control.pitch.max_decel_us_per_sec2);
            if (config->gun.turret_control.pitch.recoil_jerk_us > 0) {
                printf("               Recoil Jerk=%d µs, Variance=%d µs\n",
                       config->gun.turret_control.pitch.recoil_jerk_us,
                       config->gun.turret_control.pitch.recoil_jerk_variance_us);
            }
        }
        
        if (config->gun.turret_control.yaw.input_channel > 0) {
            int yaw_gpio = channel_to_gpio(config->gun.turret_control.yaw.input_channel);
            printf("      • Yaw:   Servo ID=%d, Channel=%d (GPIO %d), Speed=%d µs/s, Accel=%d µs/s², Decel=%d µs/s²\n", 
                   config->gun.turret_control.yaw.servo_id,
                   config->gun.turret_control.yaw.input_channel,
                   yaw_gpio,
                   (int)config->gun.turret_control.yaw.max_speed_us_per_sec,
                   (int)config->gun.turret_control.yaw.max_accel_us_per_sec2,
                   (int)config->gun.turret_control.yaw.max_decel_us_per_sec2);
            if (config->gun.turret_control.yaw.recoil_jerk_us > 0) {
                printf("               Recoil Jerk=%d µs, Variance=%d µs\n",
                       config->gun.turret_control.yaw.recoil_jerk_us,
                       config->gun.turret_control.yaw.recoil_jerk_variance_us);
            }
        }
    }
    
    // Rates of Fire
    if (config->gun.rate_count > 0) {
        printf("    " COLOR_YELLOW "Rates of Fire" COLOR_RESET ":\n");
        for (int i = 0; i < config->gun.rate_count; i++) {
            printf("      • %s: %d RPM (threshold: %d µs)\n", 
                       config->gun.rates[i].name,
                       config->gun.rates[i].rpm,
                       config->gun.rates[i].pwm_threshold_us);
            }
        }
    } else {
        printf(COLOR_YELLOW "✗ Gun FX" COLOR_RESET " (disabled)\n");
    }
    printf("\n");
    
    printf(COLOR_CYAN COLOR_BOLD "───────────────────────────────────────────────────────────────\n" COLOR_RESET);
    printf("\n");
}
