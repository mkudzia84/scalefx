#include "servo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

struct Servo {
    ServoConfig config;
    
    float current_output_us;    // Current output position
    float current_velocity_us_per_sec;  // Current velocity
    int target_output_us;       // Target position (mapped from input)
    int input_us;               // Current input value
    
    // Thread control
    pthread_t thread;
    pthread_mutex_t mutex;
    bool running;
};

/**
 * @brief Get current time in milliseconds
 */
static long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
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
static void* servo_thread_func(void *arg) {
    Servo *servo = (Servo *)arg;
    long long last_time = get_time_ms();
    
    printf("[SERVO] Processing thread started\n");
    
    while (servo->running) {
        long long current_time = get_time_ms();
        float delta_time_ms = (float)(current_time - last_time);
        last_time = current_time;
        
        if (delta_time_ms < 1.0f) {
            usleep(1000); // Prevent division by very small numbers
            continue;
        }
        
        pthread_mutex_lock(&servo->mutex);
        
        // Convert delta time to seconds
        float delta_time_sec = delta_time_ms / 1000.0f;
        
        // Map input to target output
        servo->target_output_us = map_input_to_output(&servo->config, servo->input_us);
        
        // Calculate desired change
        float error = servo->target_output_us - servo->current_output_us;
        
        // If no speed/accel limits, jump to target immediately
        if (servo->config.max_speed_us_per_sec <= 0.0f && servo->config.max_accel_us_per_sec2 <= 0.0f) {
            servo->current_output_us = servo->target_output_us;
            servo->current_velocity_us_per_sec = 0.0f;
            pthread_mutex_unlock(&servo->mutex);
            
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
        
        pthread_mutex_unlock(&servo->mutex);
        
        // Calculate sleep time based on update rate
        int update_interval_ms = 1000 / (servo->config.update_rate_hz > 0 ? servo->config.update_rate_hz : 50);
        usleep(update_interval_ms * 1000);
    }
    
    printf("[SERVO] Processing thread stopped\n");
    return NULL;
}

Servo* servo_create(const ServoConfig *config) {
    if (!config) {
        fprintf(stderr, "[SERVO] Error: Config is NULL\n");
        return NULL;
    }
    
    if (config->input_min_us >= config->input_max_us) {
        fprintf(stderr, "[SERVO] Error: Invalid input range\n");
        return NULL;
    }
    
    if (config->output_min_us >= config->output_max_us) {
        fprintf(stderr, "[SERVO] Error: Invalid output range\n");
        return NULL;
    }
    
    Servo *servo = (Servo *)calloc(1, sizeof(Servo));
    if (!servo) {
        fprintf(stderr, "[SERVO] Error: Cannot allocate memory\n");
        return NULL;
    }
    
    memcpy(&servo->config, config, sizeof(ServoConfig));
    
    // Set default update rate if not specified
    if (servo->config.update_rate_hz <= 0) {
        servo->config.update_rate_hz = 50;
    }
    
    // Initialize to center position
    int center_input = (config->input_min_us + config->input_max_us) / 2;
    servo->input_us = center_input;
    servo->target_output_us = map_input_to_output(config, center_input);
    servo->current_output_us = servo->target_output_us;
    servo->current_velocity_us_per_sec = 0.0f;
    
    pthread_mutex_init(&servo->mutex, NULL);
    servo->running = true;
    
    // Start processing thread
    if (pthread_create(&servo->thread, NULL, servo_thread_func, servo) != 0) {
        fprintf(stderr, "[SERVO] Error: Failed to create processing thread\n");
        pthread_mutex_destroy(&servo->mutex);
        free(servo);
        return NULL;
    }
    
    printf("[SERVO] Created (input: %d-%d us, output: %d-%d us, speed: %.0f us/s, accel: %.0f us/s², rate: %dHz)\n",
           config->input_min_us, config->input_max_us,
           config->output_min_us, config->output_max_us,
           config->max_speed_us_per_sec, config->max_accel_us_per_sec2,
           servo->config.update_rate_hz);
    
    return servo;
}

void servo_destroy(Servo *servo) {
    if (!servo) return;
    
    // Stop thread
    servo->running = false;
    pthread_join(servo->thread, NULL);
    
    pthread_mutex_destroy(&servo->mutex);
    free(servo);
    
    printf("[SERVO] Destroyed\n");
}

void servo_set_input(Servo *servo, int input_us) {
    if (!servo) return;
    
    pthread_mutex_lock(&servo->mutex);
    servo->input_us = input_us;
    pthread_mutex_unlock(&servo->mutex);
}

int servo_get_output(const Servo *servo) {
    if (!servo) return 0;
    
    int output;
    pthread_mutex_lock((pthread_mutex_t *)&servo->mutex);
    output = (int)servo->current_output_us;
    pthread_mutex_unlock((pthread_mutex_t *)&servo->mutex);
    
    return output;
}

int servo_get_target(const Servo *servo) {
    if (!servo) return 0;
    
    int target;
    pthread_mutex_lock((pthread_mutex_t *)&servo->mutex);
    target = servo->target_output_us;
    pthread_mutex_unlock((pthread_mutex_t *)&servo->mutex);
    
    return target;
}

float servo_get_velocity(const Servo *servo) {
    if (!servo) return 0.0f;
    
    float velocity;
    pthread_mutex_lock((pthread_mutex_t *)&servo->mutex);
    velocity = servo->current_velocity_us_per_sec;
    pthread_mutex_unlock((pthread_mutex_t *)&servo->mutex);
    
    return velocity;
}

void servo_reset(Servo *servo, int position_us) {
    if (!servo) return;
    
    pthread_mutex_lock(&servo->mutex);
    servo->current_output_us = clamp_float(position_us,
                                          servo->config.output_min_us,
                                          servo->config.output_max_us);
    servo->target_output_us = servo->current_output_us;
    servo->current_velocity_us_per_sec = 0.0f;
    pthread_mutex_unlock(&servo->mutex);
}

int servo_set_config(Servo *servo, const ServoConfig *config) {
    if (!servo || !config) return -1;
    
    if (config->input_min_us >= config->input_max_us) return -1;
    if (config->output_min_us >= config->output_max_us) return -1;
    
    pthread_mutex_lock(&servo->mutex);
    
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
    
    pthread_mutex_unlock(&servo->mutex);
    
    return 0;
}

int servo_get_config(const Servo *servo, ServoConfig *config) {
    if (!servo || !config) return -1;
    
    pthread_mutex_lock((pthread_mutex_t *)&servo->mutex);
    memcpy(config, &servo->config, sizeof(ServoConfig));
    pthread_mutex_unlock((pthread_mutex_t *)&servo->mutex);
    
    return 0;
}
