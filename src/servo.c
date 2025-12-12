#include "servo.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <threads.h>
#include <unistd.h>
#include <sys/time.h>

struct Servo {
    ServoConfig config;
    
    float current_output_us;    // Current output position
    float current_velocity_us_per_sec;  // Current velocity
    int target_output_us;       // Target position (mapped from input)
    atomic_int input_us;        // Current input value (read by servo_set_input, read by thread)
    
    // Thread control
    thrd_t thread;
    mtx_t mutex;
    atomic_bool running;
};

/**
 * @brief Get current time in milliseconds
 */
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/**
 * @brief Map input value to output range
 */
static int map_input_to_output(const ServoConfig *config, int input_us) {
    // Clamp input to valid range
    if (input_us < config->input_min_us) input_us = config->input_min_us;
    if (input_us > config->input_max_us) input_us = config->input_max_us;
    
    // Map input range to output range
    float input_range = config->input_max_us - config->input_min_us;
    float output_range = config->output_max_us - config->output_min_us;
    
    if (input_range <= 0) return config->output_min_us;
    
    float normalized = (float)(input_us - config->input_min_us) / input_range;
    int output_us = config->output_min_us + (int)(normalized * output_range);
    
    // Clamp output
    if (output_us < config->output_min_us) output_us = config->output_min_us;
    if (output_us > config->output_max_us) output_us = config->output_max_us;
    
    return output_us;
}

/**
 * @brief Clamp value to range
 */
static float clamp_float(float value, float min, float max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

/**
 * @brief Servo processing thread
 */
static int servo_thread_func(void *arg) {
    Servo *servo = (Servo *)arg;
    long long last_time = get_time_ms();
    
    LOG_INFO(LOG_SERVO, "Processing thread started");
    
    while (atomic_load(&servo->running)) {
        long long current_time = get_time_ms();
        float delta_time_ms = (float)(current_time - last_time);
        last_time = current_time;
        
        if (delta_time_ms < 1.0f) {
            usleep(1000); // Prevent division by very small numbers
            continue;
        }
        
        mtx_lock(&servo->mutex);
        
        // Convert delta time to seconds
        float delta_time_sec = delta_time_ms / 1000.0f;
        
        // Map input to target output
        int current_input = atomic_load(&servo->input_us);
        servo->target_output_us = map_input_to_output(&servo->config, current_input);
        
        // Calculate desired change
        float error = servo->target_output_us - servo->current_output_us;
        
        // If no speed/accel limits, jump to target immediately
        if (servo->config.max_speed_us_per_sec <= 0.0f && servo->config.max_accel_us_per_sec2 <= 0.0f) {
            servo->current_output_us = servo->target_output_us;
            servo->current_velocity_us_per_sec = 0.0f;
            mtx_unlock(&servo->mutex);
            
            // Calculate sleep time based on update rate
            int update_interval_ms = 1000 / (servo->config.update_rate_hz > 0 ? servo->config.update_rate_hz : 50);
            usleep(update_interval_ms * 1000);
            continue;
        }
        
        // Calculate desired velocity to reach target
        float desired_velocity = 0.0f;
        if (fabs(error) > 0.5f) {
            // Simple proportional control - velocity proportional to error
            desired_velocity = error / delta_time_sec;
            
            // Limit by max speed
            if (servo->config.max_speed_us_per_sec > 0.0f) {
                desired_velocity = clamp_float(desired_velocity, 
                                              -servo->config.max_speed_us_per_sec,
                                              servo->config.max_speed_us_per_sec);
            }
        }
        
        // Apply acceleration limiting
        if (servo->config.max_accel_us_per_sec2 > 0.0f) {
            float velocity_change = desired_velocity - servo->current_velocity_us_per_sec;
            float max_velocity_change = servo->config.max_accel_us_per_sec2 * delta_time_sec;
            
            velocity_change = clamp_float(velocity_change, -max_velocity_change, max_velocity_change);
            servo->current_velocity_us_per_sec += velocity_change;
        } else {
            servo->current_velocity_us_per_sec = desired_velocity;
        }
        
        // Update position based on velocity
        servo->current_output_us += servo->current_velocity_us_per_sec * delta_time_sec;
        
        // Clamp to output range
        servo->current_output_us = clamp_float(servo->current_output_us,
                                              servo->config.output_min_us,
                                              servo->config.output_max_us);
        
        // If very close to target, snap to it
        if (fabs(servo->target_output_us - servo->current_output_us) < 0.5f) {
            servo->current_output_us = servo->target_output_us;
            servo->current_velocity_us_per_sec = 0.0f;
        }
        
        mtx_unlock(&servo->mutex);
        
        // Calculate sleep time based on update rate
        int update_interval_ms = 1000 / (servo->config.update_rate_hz > 0 ? servo->config.update_rate_hz : 50);
        usleep(update_interval_ms * 1000);
    }
    
    LOG_INFO(LOG_SERVO, "Processing thread stopped");
    return thrd_success;
}

Servo* servo_create(const ServoConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_SERVO, "Config is nullptr");
        return nullptr;
    }
    
    if (config->input_min_us >= config->input_max_us) {
        LOG_ERROR(LOG_SERVO, "Invalid input range");
        return nullptr;
    }
    
    if (config->output_min_us >= config->output_max_us) {
        LOG_ERROR(LOG_SERVO, "Invalid output range");
        return nullptr;
    }
    
    Servo *servo = (Servo *)calloc(1, sizeof(Servo));
    if (!servo) {
        LOG_ERROR(LOG_SERVO, "Cannot allocate memory");
        return nullptr;
    }
    
    memcpy(&servo->config, config, sizeof(ServoConfig));
    
    // Set default update rate if not specified
    if (servo->config.update_rate_hz <= 0) {
        servo->config.update_rate_hz = 50;
    }
    
    // Initialize to center position
    int center_input = (config->input_min_us + config->input_max_us) / 2;
    atomic_init(&servo->input_us, center_input);
    servo->target_output_us = map_input_to_output(config, center_input);
    servo->current_output_us = servo->target_output_us;
    servo->current_velocity_us_per_sec = 0.0f;
    
    mtx_init(&servo->mutex, mtx_plain);
    atomic_init(&servo->running, true);
    
    // Start processing thread
    if (thrd_create(&servo->thread, servo_thread_func, servo) != thrd_success) {
        LOG_ERROR(LOG_SERVO, "Failed to create processing thread");
        mtx_destroy(&servo->mutex);
        free(servo);
        return nullptr;
    }
    
    LOG_INFO(LOG_SERVO, "Created (input: %d-%d us, output: %d-%d us, speed: %.0f us/s, accel: %.0f us/s², rate: %dHz)",
             config->input_min_us, config->input_max_us,
             config->output_min_us, config->output_max_us,
             config->max_speed_us_per_sec, config->max_accel_us_per_sec2,
             servo->config.update_rate_hz);
    
    return servo;
}

void servo_destroy(Servo *servo) {
    if (!servo) return;
    
    // Stop thread
    atomic_store(&servo->running, false);
    thrd_join(servo->thread, nullptr);
    
    mtx_destroy(&servo->mutex);
    free(servo);
    
    LOG_INFO(LOG_SERVO, "Destroyed");
}

void servo_set_input(Servo *servo, int input_us) {
    if (!servo) return;
    
    atomic_store(&servo->input_us, input_us);
}

int servo_get_output(Servo *servo) {
    if (!servo) return 0;
    
    int output;
    mtx_lock(&servo->mutex);
    output = (int)servo->current_output_us;
    mtx_unlock(&servo->mutex);
    
    return output;
}

int servo_get_target(Servo *servo) {
    if (!servo) return 0;
    
    int target;
    mtx_lock(&servo->mutex);
    target = servo->target_output_us;
    mtx_unlock(&servo->mutex);
    
    return target;
}

float servo_get_velocity(Servo *servo) {
    if (!servo) return 0.0f;
    
    float velocity;
    mtx_lock(&servo->mutex);
    velocity = servo->current_velocity_us_per_sec;
    mtx_unlock(&servo->mutex);
    
    return velocity;
}

void servo_reset(Servo *servo, int position_us) {
    if (!servo) return;
    
    mtx_lock(&servo->mutex);
    servo->current_output_us = clamp_float(position_us,
                                          servo->config.output_min_us,
                                          servo->config.output_max_us);
    servo->target_output_us = servo->current_output_us;
    servo->current_velocity_us_per_sec = 0.0f;
    mtx_unlock(&servo->mutex);
}

int servo_set_config(Servo *servo, const ServoConfig *config) {
    if (!servo || !config) return -1;
    
    if (config->input_min_us >= config->input_max_us) return -1;
    if (config->output_min_us >= config->output_max_us) return -1;
    
    mtx_lock(&servo->mutex);
    
    memcpy(&servo->config, config, sizeof(ServoConfig));
    
    // Set default update rate if not specified
    if (servo->config.update_rate_hz <= 0) {
        servo->config.update_rate_hz = 50;
    }
    
    // Reclamp current values to new ranges
    servo->current_output_us = clamp_float(servo->current_output_us,
                                          config->output_min_us,
                                          config->output_max_us);
    servo->target_output_us = clamp_float(servo->target_output_us,
                                         config->output_min_us,
                                         config->output_max_us);
    
    mtx_unlock(&servo->mutex);
    
    return 0;
}

int servo_get_config(Servo *servo, ServoConfig *config) {
    if (!servo || !config) return -1;
    
    mtx_lock(&servo->mutex);
    memcpy(config, &servo->config, sizeof(ServoConfig));
    mtx_unlock(&servo->mutex);
    
    return 0;
}

void servo_set_max_speed(Servo *servo, float max_speed_us_per_sec) {
    if (!servo) return;
    
    mtx_lock(&servo->mutex);
    servo->config.max_speed_us_per_sec = max_speed_us_per_sec;
    mtx_unlock(&servo->mutex);
}

void servo_set_max_acceleration(Servo *servo, float max_accel_us_per_sec2) {
    if (!servo) return;
    
    mtx_lock(&servo->mutex);
    servo->config.max_accel_us_per_sec2 = max_accel_us_per_sec2;
    mtx_unlock(&servo->mutex);
}

