/**
 * Configuration loader using libcyaml for schema-based YAML parsing
 * 
 * This implementation replaces manual libyaml parsing with libcyaml's
 * automatic schema-based approach, reducing code complexity significantly.
 */

#include "config_loader.h"
#include "logging.h"
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

// Servo Defaults
#define DEFAULT_SERVO_INPUT_MIN_US          1000    // Standard RC PWM min
#define DEFAULT_SERVO_INPUT_MAX_US          2000    // Standard RC PWM max
#define DEFAULT_SERVO_OUTPUT_MIN_US         1000    // Standard servo min
#define DEFAULT_SERVO_OUTPUT_MAX_US         2000    // Standard servo max
#define DEFAULT_SERVO_MAX_SPEED_US_PER_SEC  500.0f  // 500 µs/sec
#define DEFAULT_SERVO_MAX_ACCEL_US_PER_SEC2 2000.0f // 2000 µs/sec²
#define DEFAULT_SERVO_UPDATE_RATE_HZ        50      // 50 Hz (standard servo rate)

// JetiEX Defaults
#define DEFAULT_JETIEX_UPDATE_RATE_HZ       5       // 5 Hz telemetry rate

/* ============================================================================
 * CYAML Schema Definitions
 * ============================================================================ */

// ServoConfig schema with defaults
static const cyaml_schema_field_t servo_fields[] = {
    CYAML_FIELD_BOOL("enabled", CYAML_FLAG_DEFAULT, ServoConfig, enabled),
    CYAML_FIELD_INT("pwm_pin", CYAML_FLAG_DEFAULT, ServoConfig, pwm_pin),
    CYAML_FIELD_INT("output_pin", CYAML_FLAG_DEFAULT, ServoConfig, output_pin),
    CYAML_FIELD_INT("input_min_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, input_min_us),
    CYAML_FIELD_INT("input_max_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, input_max_us),
    CYAML_FIELD_INT("output_min_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, output_min_us),
    CYAML_FIELD_INT("output_max_us", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, output_max_us),
    CYAML_FIELD_FLOAT("max_speed_us_per_sec", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, max_speed_us_per_sec),
    CYAML_FIELD_FLOAT("max_accel_us_per_sec2", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, max_accel_us_per_sec2),
    CYAML_FIELD_INT("update_rate_hz", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, ServoConfig, update_rate_hz),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t servo_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, ServoConfig, servo_fields),
};

// EngineToggleConfig schema
static const cyaml_schema_field_t engine_toggle_config_fields[] = {
    CYAML_FIELD_INT("pin", CYAML_FLAG_DEFAULT, EngineToggleConfig, pin),
    CYAML_FIELD_INT("threshold_us", CYAML_FLAG_DEFAULT, EngineToggleConfig, threshold_us),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_toggle_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, EngineToggleConfig, engine_toggle_config_fields),
};

// EngineSoundsTransitionsConfig schema
static const cyaml_schema_field_t engine_sounds_transitions_config_fields[] = {
    CYAML_FIELD_INT("starting_offset_ms", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, EngineSoundsTransitionsConfig, starting_offset_ms),
    CYAML_FIELD_INT("stopping_offset_ms", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, EngineSoundsTransitionsConfig, stopping_offset_ms),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_sounds_transitions_config_schema = {
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

static const cyaml_schema_value_t engine_sounds_config_schema = {
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

// Trigger configuration (simple inline struct)
static const cyaml_schema_field_t trigger_fields[] = {
    CYAML_FIELD_INT("pin", CYAML_FLAG_DEFAULT, GunFXConfig, trigger.pin),
    CYAML_FIELD_END
};

// NozzleFlashConfig schema
static const cyaml_schema_field_t nozzle_flash_config_fields[] = {
    CYAML_FIELD_BOOL("enabled", CYAML_FLAG_DEFAULT, NozzleFlashConfig, enabled),
    CYAML_FIELD_INT("pin", CYAML_FLAG_DEFAULT, NozzleFlashConfig, pin),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t nozzle_flash_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, NozzleFlashConfig, nozzle_flash_config_fields),
};

// SmokeConfig schema
static const cyaml_schema_field_t smoke_config_fields[] = {
    CYAML_FIELD_BOOL("enabled", CYAML_FLAG_DEFAULT, SmokeConfig, enabled),
    CYAML_FIELD_INT("fan_pin", CYAML_FLAG_DEFAULT, SmokeConfig, fan_pin),
    CYAML_FIELD_INT("heater_pin", CYAML_FLAG_DEFAULT, SmokeConfig, heater_pin),
    CYAML_FIELD_INT("heater_toggle_pin", CYAML_FLAG_DEFAULT, SmokeConfig, heater_toggle_pin),
    CYAML_FIELD_INT("heater_pwm_threshold_us", CYAML_FLAG_DEFAULT, SmokeConfig, heater_pwm_threshold_us),
    CYAML_FIELD_INT("fan_off_delay_ms", CYAML_FLAG_DEFAULT, SmokeConfig, fan_off_delay_ms),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t smoke_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, SmokeConfig, smoke_config_fields),
};

// TurretControlConfig schema
static const cyaml_schema_field_t turret_control_config_fields[] = {
    CYAML_FIELD_MAPPING("pitch", CYAML_FLAG_DEFAULT, TurretControlConfig, pitch, servo_fields),
    CYAML_FIELD_MAPPING("yaw", CYAML_FLAG_DEFAULT, TurretControlConfig, yaw, servo_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t turret_control_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, TurretControlConfig, turret_control_config_fields),
};

// EngineFXConfig schema
static const cyaml_schema_field_t engine_fx_fields[] = {
    CYAML_FIELD_BOOL("enabled", CYAML_FLAG_DEFAULT, EngineFXConfig, enabled),
    CYAML_FIELD_MAPPING("engine_toggle", CYAML_FLAG_DEFAULT, EngineFXConfig, engine_toggle, engine_toggle_config_fields),
    CYAML_FIELD_MAPPING("sounds", CYAML_FLAG_DEFAULT, EngineFXConfig, sounds, engine_sounds_config_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t engine_fx_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, EngineFXConfig, engine_fx_fields),
};

// GunFXConfig schema
static const cyaml_schema_field_t gun_fx_fields[] = {
    CYAML_FIELD_BOOL("enabled", CYAML_FLAG_DEFAULT, GunFXConfig, enabled),
    CYAML_FIELD_MAPPING("trigger", CYAML_FLAG_DEFAULT, GunFXConfig, trigger, trigger_fields),
    CYAML_FIELD_MAPPING("nozzle_flash", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, GunFXConfig, nozzle_flash, nozzle_flash_config_fields),
    CYAML_FIELD_MAPPING("smoke", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, GunFXConfig, smoke, smoke_config_fields),
    CYAML_FIELD_MAPPING("turret_control", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, GunFXConfig, turret_control, turret_control_config_fields),
    CYAML_FIELD_SEQUENCE_COUNT("rates_of_fire", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, GunFXConfig, rates, rate_count, &rate_of_fire_schema, 0, CYAML_UNLIMITED),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t gun_fx_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, GunFXConfig, gun_fx_fields),
};

// JetiEXConfigData schema (loaded but ignored when ENABLE_JETIEX not defined)
#ifdef ENABLE_JETIEX
static const cyaml_schema_field_t jetiex_fields[] = {
    CYAML_FIELD_BOOL("enabled", CYAML_FLAG_DEFAULT, JetiEXConfigData, enabled),
    CYAML_FIELD_BOOL("remote_config", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, JetiEXConfigData, remote_config),
    CYAML_FIELD_STRING_PTR("serial_port", CYAML_FLAG_POINTER, JetiEXConfigData, serial_port, 0, CYAML_UNLIMITED),
    CYAML_FIELD_UINT("baud_rate", CYAML_FLAG_DEFAULT, JetiEXConfigData, baud_rate),
    CYAML_FIELD_UINT("manufacturer_id", CYAML_FLAG_DEFAULT, JetiEXConfigData, manufacturer_id),
    CYAML_FIELD_UINT("device_id", CYAML_FLAG_DEFAULT, JetiEXConfigData, device_id),
    CYAML_FIELD_UINT("update_rate_hz", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, JetiEXConfigData, update_rate_hz),
    CYAML_FIELD_END
};
#else
// Dummy schema when JetiEX is disabled - accepts and ignores all fields
static const cyaml_schema_field_t jetiex_fields[] = {
    CYAML_FIELD_IGNORE("enabled", CYAML_FLAG_DEFAULT),
    CYAML_FIELD_IGNORE("remote_config", CYAML_FLAG_OPTIONAL),
    CYAML_FIELD_IGNORE("serial_port", CYAML_FLAG_OPTIONAL),
    CYAML_FIELD_IGNORE("baud_rate", CYAML_FLAG_OPTIONAL),
    CYAML_FIELD_IGNORE("manufacturer_id", CYAML_FLAG_OPTIONAL),
    CYAML_FIELD_IGNORE("device_id", CYAML_FLAG_OPTIONAL),
    CYAML_FIELD_IGNORE("update_rate_hz", CYAML_FLAG_OPTIONAL),
    CYAML_FIELD_END
};
#endif

static const cyaml_schema_value_t jetiex_schema __attribute__((unused)) = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, JetiEXConfigData, jetiex_fields),
};

// Root HeliFXConfig schema
static const cyaml_schema_field_t helifx_config_fields[] = {
    CYAML_FIELD_MAPPING("engine_fx", CYAML_FLAG_DEFAULT, HeliFXConfig, engine, engine_fx_fields),
    CYAML_FIELD_MAPPING("gun_fx", CYAML_FLAG_DEFAULT, HeliFXConfig, gun, gun_fx_fields),
    CYAML_FIELD_MAPPING("jetiex", CYAML_FLAG_DEFAULT | CYAML_FLAG_OPTIONAL, HeliFXConfig, jetiex, jetiex_fields),
    CYAML_FIELD_END
};

static const cyaml_schema_value_t helifx_config_schema = {
    CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, HeliFXConfig, helifx_config_fields),
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

static inline void apply_defaults_inline(HeliFXConfig *config) {
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
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.pitch.update_rate_hz, DEFAULT_SERVO_UPDATE_RATE_HZ);
    
    // Gun - Yaw servo defaults
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.input_min_us, DEFAULT_SERVO_INPUT_MIN_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.input_max_us, DEFAULT_SERVO_INPUT_MAX_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.output_min_us, DEFAULT_SERVO_OUTPUT_MIN_US);
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.output_max_us, DEFAULT_SERVO_OUTPUT_MAX_US);
    if (config->gun.turret_control.yaw.max_speed_us_per_sec == 0.0f)
        config->gun.turret_control.yaw.max_speed_us_per_sec = DEFAULT_SERVO_MAX_SPEED_US_PER_SEC;
    if (config->gun.turret_control.yaw.max_accel_us_per_sec2 == 0.0f)
        config->gun.turret_control.yaw.max_accel_us_per_sec2 = DEFAULT_SERVO_MAX_ACCEL_US_PER_SEC2;
    APPLY_DEFAULT_IF_ZERO(config->gun.turret_control.yaw.update_rate_hz, DEFAULT_SERVO_UPDATE_RATE_HZ);
    
#ifdef ENABLE_JETIEX
    APPLY_DEFAULT_IF_ZERO(config->jetiex.update_rate_hz, DEFAULT_JETIEX_UPDATE_RATE_HZ);
#endif
}

/* ============================================================================
 * Public Functions
 * ============================================================================ */

HeliFXConfig* config_load(const char *config_file) {
    cyaml_err_t err;
    HeliFXConfig *config = nullptr;

    LOG_INFO(LOG_CONFIG, "Loading configuration from: %s", config_file);

    err = cyaml_load_file(config_file, &cyaml_config, &helifx_config_schema,
                          (cyaml_data_t **)&config, nullptr);

    if (err != CYAML_OK) {
        LOG_ERROR(LOG_CONFIG, "Failed to load config: %s", cyaml_strerror(err));
        return nullptr;
    }

    // Apply defaults for optional fields that weren't in the YAML
    apply_defaults_inline(config);

#ifndef ENABLE_JETIEX
    // Warn if JetiEX is configured but not compiled in
    if (config->jetiex.enabled) {
        LOG_WARN(LOG_CONFIG, "JetiEX telemetry is enabled in config but not compiled (ENABLE_JETIEX=0)");
        LOG_WARN(LOG_CONFIG, "JetiEX functionality will be ignored");
    }
#endif

    LOG_INFO(LOG_CONFIG, "Configuration loaded successfully");
    return config;
}

int config_save(const char *config_file, const HeliFXConfig *config) {
    cyaml_err_t err;

    LOG_INFO(LOG_CONFIG, "Saving configuration to: %s", config_file);

    err = cyaml_save_file(config_file, &cyaml_config, &helifx_config_schema,
                          (cyaml_data_t *)config, 0);

    if (err != CYAML_OK) {
        LOG_ERROR(LOG_CONFIG, "Failed to save config: %s", cyaml_strerror(err));
        return -1;
    }

    LOG_INFO(LOG_CONFIG, "Configuration saved successfully");
    return 0;
}

void config_free(HeliFXConfig *config) {
    if (config) {
        cyaml_free(&cyaml_config, &helifx_config_schema, config, 0);
    }
}

int config_validate(const HeliFXConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_CONFIG, "Validation failed: nullptr config");
        return -1;
    }

    // Engine validation
    if (config->engine.enabled) {
        if (config->engine.engine_toggle.pin < 0) {
            LOG_ERROR(LOG_CONFIG, "Invalid engine pin: %d", config->engine.engine_toggle.pin);
            return -1;
        }
        if (!config->engine.sounds.starting || !config->engine.sounds.running || !config->engine.sounds.stopping) {
            LOG_ERROR(LOG_CONFIG, "Missing engine sound files");
            return -1;
        }
    }

    // Gun validation
    if (config->gun.enabled) {
        if (config->gun.trigger.pin < 0) {
            LOG_ERROR(LOG_CONFIG, "Invalid gun trigger pin: %d", config->gun.trigger.pin);
            return -1;
        }
        
        // Validate servos
        if (config->gun.turret_control.pitch.enabled) {
            if (config->gun.turret_control.pitch.pwm_pin < 0 || config->gun.turret_control.pitch.output_pin < 0) {
                LOG_ERROR(LOG_CONFIG, "Invalid pitch servo pins");
                return -1;
            }
        }
        if (config->gun.turret_control.yaw.enabled) {
            if (config->gun.turret_control.yaw.pwm_pin < 0 || config->gun.turret_control.yaw.output_pin < 0) {
                LOG_ERROR(LOG_CONFIG, "Invalid yaw servo pins");
                return -1;
            }
        }
        
        // Validate rates of fire
        if (config->gun.rate_count > 0 && !config->gun.rates) {
            LOG_ERROR(LOG_CONFIG, "Invalid rates of fire configuration");
            return -1;
        }
    }

#ifdef ENABLE_JETIEX
    // JetiEX validation
    if (config->jetiex.enabled) {
        if (!config->jetiex.serial_port) {
                LOG_ERROR(LOG_CONFIG, "Missing JetiEX serial port");
            return -1;
        }
        if (config->jetiex.baud_rate == 0) {
                LOG_ERROR(LOG_CONFIG, "Invalid JetiEX baud rate");
            return -1;
        }
    }
#endif

    LOG_INFO(LOG_CONFIG, "Configuration validation passed");
    return 0;
}

void config_print(const HeliFXConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_CONFIG, "Cannot print nullptr config");
        return;
    }

    printf("\n=== HeliFX Configuration ===\n");
    
    // Engine FX
    printf("\n[Engine FX]\n");
    printf("  Enabled: %s\n", config->engine.enabled ? "true" : "false");
    if (config->engine.enabled) {
        printf("  Pin: %d\n", config->engine.engine_toggle.pin);
        printf("  Threshold: %d µs\n", config->engine.engine_toggle.threshold_us);
        printf("  Starting: %s\n", config->engine.sounds.starting ?: "(none)");
        printf("  Running: %s\n", config->engine.sounds.running ?: "(none)");
        printf("  Stopping: %s\n", config->engine.sounds.stopping ?: "(none)");
        printf("  Starting Offset: %d ms\n", config->engine.sounds.transitions.starting_offset_ms);
        printf("  Stopping Offset: %d ms\n", config->engine.sounds.transitions.stopping_offset_ms);
    }
    
    // Gun FX
    printf("\n[Gun FX]\n");
    printf("  Enabled: %s\n", config->gun.enabled ? "true" : "false");
    if (config->gun.enabled) {
        printf("  Trigger Pin: %d\n", config->gun.trigger.pin);
        
        printf("\n  [Nozzle Flash]\n");
        printf("    Enabled: %s\n", config->gun.nozzle_flash.enabled ? "true" : "false");
        if (config->gun.nozzle_flash.enabled) {
            printf("    Pin: %d\n", config->gun.nozzle_flash.pin);
        }
        
        printf("\n  [Smoke]\n");
        printf("    Enabled: %s\n", config->gun.smoke.enabled ? "true" : "false");
        if (config->gun.smoke.enabled) {
            printf("    Fan Pin: %d\n", config->gun.smoke.fan_pin);
            printf("    Heater Pin: %d\n", config->gun.smoke.heater_pin);
            printf("    Heater Toggle Pin: %d\n", config->gun.smoke.heater_toggle_pin);
            printf("    Heater PWM Threshold: %d µs\n", config->gun.smoke.heater_pwm_threshold_us);
            printf("    Fan Off Delay: %d ms\n", config->gun.smoke.fan_off_delay_ms);
        }
        
        printf("\n  [Turret Control - Pitch]\n");
        printf("    Enabled: %s\n", config->gun.turret_control.pitch.enabled ? "true" : "false");
        if (config->gun.turret_control.pitch.enabled) {
            printf("    PWM Pin: %d\n", config->gun.turret_control.pitch.pwm_pin);
            printf("    Output Pin: %d\n", config->gun.turret_control.pitch.output_pin);
            printf("    Input Range: %d-%d µs\n", 
                   config->gun.turret_control.pitch.input_min_us,
                   config->gun.turret_control.pitch.input_max_us);
            printf("    Output Range: %d-%d µs\n",
                   config->gun.turret_control.pitch.output_min_us,
                   config->gun.turret_control.pitch.output_max_us);
            printf("    Max Speed: %.1f µs/sec\n", config->gun.turret_control.pitch.max_speed_us_per_sec);
            printf("    Max Accel: %.1f µs/sec²\n", config->gun.turret_control.pitch.max_accel_us_per_sec2);
            printf("    Update Rate: %d Hz\n", config->gun.turret_control.pitch.update_rate_hz);
        }
        
        printf("\n  [Turret Control - Yaw]\n");
        printf("    Enabled: %s\n", config->gun.turret_control.yaw.enabled ? "true" : "false");
        if (config->gun.turret_control.yaw.enabled) {
            printf("    PWM Pin: %d\n", config->gun.turret_control.yaw.pwm_pin);
            printf("    Output Pin: %d\n", config->gun.turret_control.yaw.output_pin);
            printf("    Input Range: %d-%d µs\n",
                   config->gun.turret_control.yaw.input_min_us,
                   config->gun.turret_control.yaw.input_max_us);
            printf("    Output Range: %d-%d µs\n",
                   config->gun.turret_control.yaw.output_min_us,
                   config->gun.turret_control.yaw.output_max_us);
            printf("    Max Speed: %.1f µs/sec\n", config->gun.turret_control.yaw.max_speed_us_per_sec);
            printf("    Max Accel: %.1f µs/sec²\n", config->gun.turret_control.yaw.max_accel_us_per_sec2);
            printf("    Update Rate: %d Hz\n", config->gun.turret_control.yaw.update_rate_hz);
        }
        
        printf("\n  [Rates of Fire]\n");
        for (int i = 0; i < config->gun.rate_count; i++) {
            printf("    %s: %d RPM (threshold: %d µs, sound: %s)\n",
                   config->gun.rates[i].name,
                   config->gun.rates[i].rpm,
                   config->gun.rates[i].pwm_threshold_us,
                   config->gun.rates[i].sound_file ?: "(none)");
        }
    }
    
#ifdef ENABLE_JETIEX
    // JetiEX
    printf("\n[JetiEX]\n");
    printf("  Enabled: %s\n", config->jetiex.enabled ? "true" : "false");
    if (config->jetiex.enabled) {
        printf("  Remote Config: %s\n", config->jetiex.remote_config ? "true" : "false");
        printf("  Serial Port: %s\n", config->jetiex.serial_port ?: "(none)");
        printf("  Baud Rate: %u\n", config->jetiex.baud_rate);
        printf("  Manufacturer ID: 0x%04X\n", config->jetiex.manufacturer_id);
        printf("  Device ID: 0x%04X\n", config->jetiex.device_id);
        printf("  Update Rate: %u Hz\n", config->jetiex.update_rate_hz);
    }
#endif
    
    printf("\n");
}
