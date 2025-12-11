/**
 * @file helifx_jetiex.c
 * @brief JetiEX telemetry integration for HeliFX
 */

#include "helifx_jetiex.h"
#include "logging.h"
#include "servo.h"
#include <string.h>
#include <stdlib.h>

// Global state for callbacks
static HeliFXConfig *g_config = nullptr;
static char *g_config_file_path = nullptr;
static GunFX *g_gun = nullptr;
static EngineFX *g_engine = nullptr;

// Parameter value storage (synchronized with JetiEX library)
static struct {
    uint16_t gun_rate_1_rpm;
    uint16_t gun_rate_2_rpm;
    uint16_t gun_rate_1_pwm;
    uint16_t gun_rate_2_pwm;
    uint16_t smoke_fan_delay_ms;
    uint16_t heater_pwm_threshold;
    uint16_t engine_pwm_threshold;
    uint16_t servo_max_speed;
    uint16_t servo_max_accel;
    uint8_t telemetry_rate_hz;
    bool nozzle_flash_enabled;
    bool smoke_enabled;
} g_parameters = {0};

// Helper: update servo speed/accel for both pitch and yaw
static void update_servo_setting(bool (*getter)(GunFX*, Servo**), 
                                 void (*setter)(Servo*, float),
                                 float value, const char *name) {
    if (!g_gun || !g_config->gun.enabled) return;
    
    Servo *servo = nullptr;
    if (getter(g_gun, &servo) && servo) {
        setter(servo, value);
    }
}

// JetiEX parameter change callback
static void on_parameter_change(uint8_t param_id, void *user_data) {
    (void)user_data;
    if (!g_config) return;
    
    // Handle gun rate parameters (0-3)
    if (param_id <= 3) {
        int rate_idx = param_id / 2;
        bool is_rpm = (param_id % 2) == 0;
        
        if (g_config->gun.enabled && rate_idx < g_config->gun.rate_count) {
            if (is_rpm) {
                uint16_t *rates_rpm[] = {&g_parameters.gun_rate_1_rpm, &g_parameters.gun_rate_2_rpm};
                g_config->gun.rates[rate_idx].rpm = *rates_rpm[rate_idx];
            } else {
                uint16_t *rates_pwm[] = {&g_parameters.gun_rate_1_pwm, &g_parameters.gun_rate_2_pwm};
                g_config->gun.rates[rate_idx].pwm_threshold_us = *rates_pwm[rate_idx];
            }
            LOG_INFO(LOG_JETIEX, "Gun rate %d %s updated", rate_idx + 1, is_rpm ? "RPM" : "PWM");
            
            // Rebuild rates array and apply
            if (g_gun) {
                RateOfFire *rates = (RateOfFire*)malloc(g_config->gun.rate_count * sizeof(RateOfFire));
                if (rates) {
                    for (int i = 0; i < g_config->gun.rate_count; i++) {
                        rates[i].rounds_per_minute = g_config->gun.rates[i].rpm;
                        rates[i].pwm_threshold_us = g_config->gun.rates[i].pwm_threshold_us;
                        rates[i].sound = nullptr;
                    }
                    gun_fx_set_rates_of_fire(g_gun, rates, g_config->gun.rate_count);
                    free(rates);
                }
            }
        }
        return;
    }
    
    switch (param_id) {
        case 4: // Smoke Fan Delay
            if (g_config->gun.enabled && g_config->gun.smoke_enabled) {
                g_config->gun.smoke_fan_off_delay_ms = g_parameters.smoke_fan_delay_ms;
                if (g_gun) gun_fx_set_smoke_fan_off_delay(g_gun, g_parameters.smoke_fan_delay_ms);
                LOG_INFO(LOG_JETIEX, "Smoke fan delay set to %d ms", g_parameters.smoke_fan_delay_ms);
            }
            break;
        case 5: // Heater PWM Threshold
            if (g_config->gun.enabled && g_config->gun.smoke_enabled) {
                g_config->gun.smoke_heater_pwm_threshold_us = g_parameters.heater_pwm_threshold;
                LOG_INFO(LOG_JETIEX, "Heater PWM threshold set to %d", g_parameters.heater_pwm_threshold);
            }
            break;
        case 6: // Engine PWM Threshold
            if (g_config->engine.enabled) {
                g_config->engine.threshold_us = g_parameters.engine_pwm_threshold;
                LOG_INFO(LOG_JETIEX, "Engine PWM threshold set to %d", g_parameters.engine_pwm_threshold);
            }
            break;
        case 7: // Servo Max Speed
            if (g_config->gun.enabled) {
                g_config->gun.pitch_servo.max_speed_us_per_sec = (float)g_parameters.servo_max_speed;
                g_config->gun.yaw_servo.max_speed_us_per_sec = (float)g_parameters.servo_max_speed;
                if (g_gun) {
                    Servo *pitch = gun_fx_get_pitch_servo(g_gun);
                    if (pitch) servo_set_max_speed(pitch, (float)g_parameters.servo_max_speed);
                    Servo *yaw = gun_fx_get_yaw_servo(g_gun);
                    if (yaw) servo_set_max_speed(yaw, (float)g_parameters.servo_max_speed);
                }
                LOG_INFO(LOG_JETIEX, "Servo max speed set to %d", g_parameters.servo_max_speed);
            }
            break;
        case 8: // Servo Max Accel
            if (g_config->gun.enabled) {
                g_config->gun.pitch_servo.max_accel_us_per_sec2 = (float)g_parameters.servo_max_accel;
                g_config->gun.yaw_servo.max_accel_us_per_sec2 = (float)g_parameters.servo_max_accel;
                if (g_gun) {
                    Servo *pitch = gun_fx_get_pitch_servo(g_gun);
                    if (pitch) servo_set_max_acceleration(pitch, (float)g_parameters.servo_max_accel);
                    Servo *yaw = gun_fx_get_yaw_servo(g_gun);
                    if (yaw) servo_set_max_acceleration(yaw, (float)g_parameters.servo_max_accel);
                }
                LOG_INFO(LOG_JETIEX, "Servo max accel set to %d", g_parameters.servo_max_accel);
            }
            break;
        case 9: // Telemetry Rate Hz
            g_config->jetiex.update_rate_hz = g_parameters.telemetry_rate_hz;
            LOG_INFO(LOG_JETIEX, "Telemetry rate set to %d Hz", g_parameters.telemetry_rate_hz);
            break;
        case 10: // Nozzle Flash Enable
            if (g_config->gun.enabled) {
                g_config->gun.nozzle_flash_enabled = g_parameters.nozzle_flash_enabled;
                LOG_INFO(LOG_JETIEX, "Nozzle flash %s", g_parameters.nozzle_flash_enabled ? "enabled" : "disabled");
            }
            break;
        case 11: // Smoke Enable
            if (g_config->gun.enabled) {
                g_config->gun.smoke_enabled = g_parameters.smoke_enabled;
                LOG_INFO(LOG_JETIEX, "Smoke %s", g_parameters.smoke_enabled ? "enabled" : "disabled");
            }
            break;
    }
}

// JetiEX save callback
static bool on_save_config(void *user_data) {
    (void)user_data;
    
    if (!g_config_file_path || !g_config) {
        LOG_ERROR(LOG_JETIEX, "Cannot save: config path or data missing");
        return false;
    }
    
    LOG_INFO(LOG_JETIEX, "Saving configuration to %s", g_config_file_path);
    
    if (config_save(g_config_file_path, g_config) == 0) {
        LOG_INFO(LOG_JETIEX, "Configuration saved successfully");
        return true;
    } else {
        LOG_ERROR(LOG_JETIEX, "Failed to save configuration");
        return false;
    }
}

JetiEX* helifx_jetiex_init(HeliFXConfig *config, const char *config_file_path,
                           GunFX *gun, EngineFX *engine) {
    if (!config || !config->jetiex.enabled) {
        return nullptr;
    }
    
    // Store global pointers for callbacks
    g_config = config;
    g_config_file_path = (char*)config_file_path;
    g_gun = gun;
    g_engine = engine;
    
    // Initialize parameter values from config
    if (config->gun.enabled && config->gun.rate_count > 0) {
        g_parameters.gun_rate_1_rpm = config->gun.rates[0].rpm;
        g_parameters.gun_rate_1_pwm = config->gun.rates[0].pwm_threshold_us;
        if (config->gun.rate_count > 1) {
            g_parameters.gun_rate_2_rpm = config->gun.rates[1].rpm;
            g_parameters.gun_rate_2_pwm = config->gun.rates[1].pwm_threshold_us;
        }
        g_parameters.smoke_fan_delay_ms = config->gun.smoke_fan_off_delay_ms;
        g_parameters.heater_pwm_threshold = config->gun.smoke_heater_pwm_threshold_us;
        g_parameters.nozzle_flash_enabled = config->gun.nozzle_flash_enabled;
        g_parameters.smoke_enabled = config->gun.smoke_enabled;
    }
    
    if (config->engine.enabled) {
        g_parameters.engine_pwm_threshold = config->engine.threshold_us;
    }
    
    if (config->gun.enabled) {
        if (config->gun.pitch_servo.enabled) {
            g_parameters.servo_max_speed = (uint16_t)config->gun.pitch_servo.max_speed_us_per_sec;
            g_parameters.servo_max_accel = (uint16_t)config->gun.pitch_servo.max_accel_us_per_sec2;
        }
    }
    
    g_parameters.telemetry_rate_hz = config->jetiex.update_rate_hz;
    
    LOG_INFO(LOG_JETIEX, "Initializing JetiEX telemetry...");
    
    // Create JetiEX configuration
    JetiEXConfig jetiex_config = {
        .serial_port = config->jetiex.serial_port,
        .baud_rate = config->jetiex.baud_rate,
        .manufacturer_id = config->jetiex.manufacturer_id,
        .device_id = config->jetiex.device_id,
        .update_rate_hz = config->jetiex.update_rate_hz,
        .text_messages = true,
        .remote_config = config->jetiex.remote_config,
        .config_changed_callback = on_parameter_change,
        .config_save_callback = on_save_config,
        .user_data = nullptr
    };
    
    JetiEX *jetiex = jetiex_create(&jetiex_config);
    if (!jetiex) {
        LOG_ERROR(LOG_JETIEX, "Failed to create JetiEX telemetry");
        return nullptr;
    }
    
    // Register configurable parameters
    JetiEXParameter params[] = {
        {0, "Gun Rate 1 RPM", JETIEX_PARAM_UINT16, &g_parameters.gun_rate_1_rpm, 0, 10000, JETIEX_PARAM_PERSISTENT},
        {1, "Gun Rate 2 RPM", JETIEX_PARAM_UINT16, &g_parameters.gun_rate_2_rpm, 0, 10000, JETIEX_PARAM_PERSISTENT},
        {2, "Gun Rate 1 PWM", JETIEX_PARAM_UINT16, &g_parameters.gun_rate_1_pwm, 1000, 2000, JETIEX_PARAM_PERSISTENT},
        {3, "Gun Rate 2 PWM", JETIEX_PARAM_UINT16, &g_parameters.gun_rate_2_pwm, 1000, 2000, JETIEX_PARAM_PERSISTENT},
        {4, "Smoke Fan Delay", JETIEX_PARAM_UINT16, &g_parameters.smoke_fan_delay_ms, 0, 10000, JETIEX_PARAM_PERSISTENT},
        {5, "Heater PWM Threshold", JETIEX_PARAM_UINT16, &g_parameters.heater_pwm_threshold, 1000, 2000, JETIEX_PARAM_PERSISTENT},
        {6, "Engine PWM Threshold", JETIEX_PARAM_UINT16, &g_parameters.engine_pwm_threshold, 1000, 2000, JETIEX_PARAM_PERSISTENT},
        {7, "Servo Max Speed", JETIEX_PARAM_UINT16, &g_parameters.servo_max_speed, 100, 5000, JETIEX_PARAM_PERSISTENT},
        {8, "Servo Max Accel", JETIEX_PARAM_UINT16, &g_parameters.servo_max_accel, 100, 10000, JETIEX_PARAM_PERSISTENT},
        {9, "Telemetry Rate Hz", JETIEX_PARAM_UINT8, &g_parameters.telemetry_rate_hz, 1, 100, JETIEX_PARAM_PERSISTENT},
        {10, "Nozzle Flash Enable", JETIEX_PARAM_BOOL, &g_parameters.nozzle_flash_enabled, 0, 1, JETIEX_PARAM_PERSISTENT},
        {11, "Smoke Enable", JETIEX_PARAM_BOOL, &g_parameters.smoke_enabled, 0, 1, JETIEX_PARAM_PERSISTENT}
    };
    
    for (int i = 0; i < 12; i++) {
        jetiex_add_parameter(jetiex, &params[i]);
    }
    
    LOG_INFO(LOG_JETIEX, "JetiEX initialized successfully");
    return jetiex;
}

void helifx_jetiex_update(JetiEX *jetiex, GunFX *gun, EngineFX *engine) {
    if (!jetiex) return;
    
    // Update gun rate sensor (sensor 0)
    if (gun) {
        int gun_rpm = gun_fx_get_current_rpm(gun);
        jetiex_update_sensor(jetiex, 0, gun_rpm);
    }
    
    // Update engine state sensor (sensor 1)
    if (engine) {
        EngineState state = engine_fx_get_state(engine);
        jetiex_update_sensor(jetiex, 1, (int32_t)state);
    }
}

void helifx_jetiex_cleanup(JetiEX *jetiex) {
    if (!jetiex) return;
    
    LOG_INFO(LOG_JETIEX, "Stopping JetiEX telemetry");
    jetiex_destroy(jetiex);
    
    // Clear global pointers
    g_config = nullptr;
    g_config_file_path = nullptr;
    g_gun = nullptr;
    g_engine = nullptr;
}
