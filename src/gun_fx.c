#include "gun_fx.h"
#include "config_loader.h"
#include "gpio.h"
#include "audio_player.h"
#include "lights.h"
#include "smoke_generator.h"
#include "servo.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

struct GunFX {
    // Audio
    AudioMixer *mixer;
    int audio_channel;
    
    // PWM monitoring for trigger
    PWMMonitor *trigger_pwm_monitor;
    int trigger_pwm_pin;
    
    // PWM monitoring for smoke heater toggle
    PWMMonitor *smoke_heater_toggle_monitor;
    int smoke_heater_toggle_pin;
    int smoke_heater_threshold;
    
    // Servo control
    Servo *pitch_servo;
    Servo *yaw_servo;
    PWMMonitor *pitch_pwm_monitor;
    PWMMonitor *yaw_pwm_monitor;
    int pitch_pwm_pin;
    int yaw_pwm_pin;
    
    // Components (optional)
    Led *nozzle_flash;
    SmokeGenerator *smoke;
    
    // Rates of fire
    RateOfFire *rates;
    int rate_count;
    
    // Current state
    bool is_firing;
    int current_rpm;
    int current_rate_index;  // Currently active rate (-1 = not firing)
    bool smoke_heater_on;
    
    // Smoke fan control
    int smoke_fan_off_delay_ms;  // Delay before turning smoke fan off after firing stops
    struct timespec smoke_stop_time;  // Time when firing stopped
    bool smoke_fan_pending_off;  // True if smoke fan is waiting to turn off
    
    // Processing thread
    pthread_t processing_thread;
    bool processing_running;
    pthread_mutex_t mutex;
};

// Select rate of fire from PWM reading with hysteresis
// Returns -1 if not firing, or index of active rate (0 to rate_count-1)
// Finds the highest matching rate (rates don't need to be ordered)
static int select_rate_of_fire(GunFX *gun, int pwm_duration_us, int previous_rate_index) {
    int hysteresis_us = 50;  // Deadzone for rate switching
    
    // Find the best matching rate
    // Apply hysteresis: current rate gets advantage to prevent oscillation
    int best_match = -1;
    int best_threshold = -1;
    
    for (int i = 0; i < gun->rate_count; i++) {
        int threshold = gun->rates[i].pwm_threshold_us;
        int effective_threshold;
        
        // Current rate: apply negative hysteresis (easier to stay)
        // Other rates: apply positive hysteresis (harder to switch)
        if (i == previous_rate_index) {
            effective_threshold = threshold - hysteresis_us;
        } else {
            effective_threshold = threshold + hysteresis_us;
        }
        
        // Check if PWM meets this rate's threshold
        if (pwm_duration_us >= effective_threshold) {
            // Select the highest threshold that matches
            if (threshold > best_threshold) {
                best_threshold = threshold;
                best_match = i;
            }
        } else {
            // Optimization: if rates are ordered low to high and we didn't match,
            // we won't match any higher rates either (when not the current rate)
            if (i != previous_rate_index && i < gun->rate_count - 1) {
                int next_threshold = gun->rates[i + 1].pwm_threshold_us;
                if (next_threshold > threshold) {
                    // Rates are ordered, can break early
                    break;
                }
            }
        }
    }
    
    return best_match;  // Returns -1 if no match found (not firing)
}

// Processing thread to monitor PWM and handle firing
static void* gun_fx_processing_thread(void *arg) {
    GunFX *gun = (GunFX *)arg;
    PWMReading trigger_reading;
    PWMReading heater_reading;
    PWMReading pitch_reading;
    PWMReading yaw_reading;
    
    printf("[GUN] Processing thread started\n");
    
    // For periodic status output
    struct timespec last_status_time;
    clock_gettime(CLOCK_MONOTONIC, &last_status_time);
    
    while (gun->processing_running) {
        // Update servos from PWM inputs
        if (gun->pitch_servo && gun->pitch_pwm_monitor && 
            pwm_monitor_get_reading(gun->pitch_pwm_monitor, &pitch_reading)) {
            servo_set_input(gun->pitch_servo, pitch_reading.duration_us);
        }
        
        if (gun->yaw_servo && gun->yaw_pwm_monitor && 
            pwm_monitor_get_reading(gun->yaw_pwm_monitor, &yaw_reading)) {
            servo_set_input(gun->yaw_servo, yaw_reading.duration_us);
        }
        
        // Get current rate index
        pthread_mutex_lock(&gun->mutex);
        int previous_rate_index = gun->current_rate_index;
        pthread_mutex_unlock(&gun->mutex);
        
        // Determine new rate based on PWM input
        int new_rate_index;
        if (gun->trigger_pwm_monitor && pwm_monitor_get_reading(gun->trigger_pwm_monitor, &trigger_reading)) {
            new_rate_index = select_rate_of_fire(gun, trigger_reading.duration_us, previous_rate_index);
            
            // Log rate changes
            if (new_rate_index != previous_rate_index) {
                if (new_rate_index >= 0) {
                    printf("[GUN] Rate changed: Rate %d (%d RPM)\n",
                           new_rate_index + 1,
                           gun->rates[new_rate_index].rounds_per_minute);
                } else {
                    printf("[GUN] Trigger OFF\n");
                }
            }
        } else {
            // No new PWM reading available, keep current rate
            new_rate_index = previous_rate_index;
        }
        
        // Handle smoke heater toggle
        if (gun->smoke_heater_toggle_monitor && pwm_monitor_get_reading(gun->smoke_heater_toggle_monitor, &heater_reading)) {
            bool heater_toggle_on = (heater_reading.duration_us >= gun->smoke_heater_threshold);
            
            pthread_mutex_lock(&gun->mutex);
            if (heater_toggle_on && !gun->smoke_heater_on) {
                gun->smoke_heater_on = true;
                printf("[GUN] Smoke heater ON\n");
                if (gun->smoke) {
                    smoke_generator_heater_on(gun->smoke);
                }
            } else if (!heater_toggle_on && gun->smoke_heater_on) {
                gun->smoke_heater_on = false;
                printf("[GUN] Smoke heater OFF\n");
                if (gun->smoke) {
                    smoke_generator_heater_off(gun->smoke);
                }
            }
            pthread_mutex_unlock(&gun->mutex);
        }
        
        // Update state if rate changed
        if (new_rate_index != previous_rate_index) {
            pthread_mutex_lock(&gun->mutex);
            gun->current_rate_index = new_rate_index;
            gun->is_firing = (new_rate_index >= 0);
            gun->current_rpm = (new_rate_index >= 0) ? gun->rates[new_rate_index].rounds_per_minute : 0;
            pthread_mutex_unlock(&gun->mutex);
            
            if (new_rate_index >= 0) {
                // Start or change firing rate
                int shot_interval_ms = (60 * 1000) / gun->rates[new_rate_index].rounds_per_minute;
                
                if (gun->nozzle_flash) {
                    led_blink(gun->nozzle_flash, shot_interval_ms);
                }
                
                if (gun->smoke) {
                    smoke_generator_fan_on(gun->smoke);
                    // Cancel any pending smoke fan off
                    gun->smoke_fan_pending_off = false;
                }
                
                if (gun->mixer && gun->rates[new_rate_index].sound) {
                    PlaybackOptions opts = {.loop = true, .volume = 1.0f};
                    audio_mixer_play(gun->mixer, gun->audio_channel, gun->rates[new_rate_index].sound, &opts);
                }
                
                if (previous_rate_index < 0) {
                    printf("[GUN] Firing started (%d RPM)\n", gun->rates[new_rate_index].rounds_per_minute);
                } else {
                    printf("[GUN] Rate changed to %d RPM\n", gun->rates[new_rate_index].rounds_per_minute);
                }
            } else {
                // Stop firing
                if (gun->nozzle_flash) {
                    led_off(gun->nozzle_flash);
                }
                
                if (gun->mixer) {
                    audio_mixer_stop_channel(gun->mixer, gun->audio_channel, STOP_IMMEDIATE);
                }
                
                // Handle smoke fan with optional delay
                if (gun->smoke) {
                    if (gun->smoke_fan_off_delay_ms > 0) {
                        // Start delay timer
                        clock_gettime(CLOCK_MONOTONIC, &gun->smoke_stop_time);
                        gun->smoke_fan_pending_off = true;
                        printf("[GUN] Firing stopped (smoke fan will stop in %d ms)\n", gun->smoke_fan_off_delay_ms);
                    } else {
                        // Immediate off
                        smoke_generator_fan_off(gun->smoke);
                        printf("[GUN] Firing stopped\n");
                    }
                } else {
                    printf("[GUN] Firing stopped\n");
                }
            }
        }
        
        // Check if smoke fan needs to be turned off after delay
        if (gun->smoke_fan_pending_off && gun->smoke) {
            struct timespec current_time_smoke;
            clock_gettime(CLOCK_MONOTONIC, &current_time_smoke);
            double elapsed_ms = ((current_time_smoke.tv_sec - gun->smoke_stop_time.tv_sec) * 1000.0) +
                               ((current_time_smoke.tv_nsec - gun->smoke_stop_time.tv_nsec) / 1e6);
            
            if (elapsed_ms >= gun->smoke_fan_off_delay_ms) {
                smoke_generator_fan_off(gun->smoke);
                gun->smoke_fan_pending_off = false;
                printf("[GUN] Smoke fan stopped (after %d ms delay)\n", gun->smoke_fan_off_delay_ms);
            }
        }
        
        // Periodic status output (every 10 seconds)
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double elapsed = (current_time.tv_sec - last_status_time.tv_sec) + 
                        (current_time.tv_nsec - last_status_time.tv_nsec) / 1e9;
        
        if (elapsed >= 10.0) {
            pthread_mutex_lock(&gun->mutex);
            int current_pwm = -1;
            if (gun->trigger_pwm_monitor && pwm_monitor_get_reading(gun->trigger_pwm_monitor, &trigger_reading)) {
                current_pwm = trigger_reading.duration_us;
            }
            
            int pitch_pwm = -1;
            if (gun->pitch_pwm_monitor && pwm_monitor_get_reading(gun->pitch_pwm_monitor, &pitch_reading)) {
                pitch_pwm = pitch_reading.duration_us;
            }
            
            int yaw_pwm = -1;
            if (gun->yaw_pwm_monitor && pwm_monitor_get_reading(gun->yaw_pwm_monitor, &yaw_reading)) {
                yaw_pwm = yaw_reading.duration_us;
            }
            
            printf("[GUN STATUS @ %.1fs] Firing: %s | Rate: %d | RPM: %d | Trigger PWM: %d µs | Heater: %s\n",
                   elapsed,
                   gun->is_firing ? "YES" : "NO",
                   gun->current_rate_index >= 0 ? gun->current_rate_index + 1 : 0,
                   gun->current_rpm,
                   current_pwm,
                   gun->smoke_heater_on ? "ON" : "OFF");
            printf("[GUN SERVOS] Pitch: %d µs | Yaw: %d µs | Pitch Servo: %s | Yaw Servo: %s\n",
                   pitch_pwm,
                   yaw_pwm,
                   gun->pitch_servo ? "ACTIVE" : "DISABLED",
                   gun->yaw_servo ? "ACTIVE" : "DISABLED");
            pthread_mutex_unlock(&gun->mutex);
            
            last_status_time = current_time;
        }
        
        usleep(10000); // 10ms loop
    }
    
    printf("[GUN] Processing thread stopped\n");
    return NULL;
}

GunFX* gun_fx_create(AudioMixer *mixer, int audio_channel,
                     const GunFXConfig *config) {
    if (!config) {
        fprintf(stderr, "[GUN] Error: Config is NULL\n");
        return NULL;
    }
    
    GunFX *gun = (GunFX *)calloc(1, sizeof(GunFX));
    if (!gun) {
        fprintf(stderr, "[GUN] Error: Cannot allocate memory for gun FX\n");
        return NULL;
    }
    
    gun->mixer = mixer;
    gun->audio_channel = audio_channel;
    gun->trigger_pwm_pin = config->trigger_pin;
    gun->smoke_heater_toggle_pin = config->smoke_heater_toggle_pin;
    gun->smoke_heater_threshold = config->smoke_heater_pwm_threshold_us;
    gun->smoke_fan_off_delay_ms = config->smoke_fan_off_delay_ms;
    gun->rates = NULL;
    gun->rate_count = 0;
    gun->is_firing = false;
    gun->current_rpm = 0;
    gun->current_rate_index = -1;  // Not firing initially
    gun->smoke_heater_on = false;
    gun->processing_running = false;
    gun->pitch_servo = NULL;
    gun->yaw_servo = NULL;
    gun->pitch_pwm_monitor = NULL;
    gun->yaw_pwm_monitor = NULL;
    pthread_mutex_init(&gun->mutex, NULL);
    
    // Create nozzle flash LED if pin specified
    if (config->nozzle_flash_pin >= 0) {
        gun->nozzle_flash = led_create(config->nozzle_flash_pin);
        if (!gun->nozzle_flash) {
            fprintf(stderr, "[GUN] Warning: Failed to create nozzle flash LED\n");
        } else {
            printf("[GUN] Nozzle flash LED on pin %d\n", config->nozzle_flash_pin);
        }
    }
    
    // Create smoke generator if pins specified
    if (config->smoke_fan_pin >= 0 && config->smoke_heater_pin >= 0) {
        gun->smoke = smoke_generator_create(config->smoke_heater_pin, config->smoke_fan_pin);
        if (!gun->smoke) {
            fprintf(stderr, "[GUN] Warning: Failed to create smoke generator\n");
        } else {
            printf("[GUN] Smoke generator (heater: pin %d, fan: pin %d)\n", 
                   config->smoke_heater_pin, config->smoke_fan_pin);
        }
    }
    
    // Create trigger PWM monitor if pin specified
    if (config->trigger_pin >= 0) {
        gun->trigger_pwm_monitor = pwm_monitor_create(config->trigger_pin, NULL, NULL);
        if (!gun->trigger_pwm_monitor) {
            fprintf(stderr, "[GUN] Warning: Failed to create trigger PWM monitor\n");
        } else {
            pwm_monitor_start(gun->trigger_pwm_monitor);
            printf("[GUN] Trigger PWM monitoring on pin %d\n", config->trigger_pin);
        }
    }
    
    // Create smoke heater toggle PWM monitor if pin specified
    if (config->smoke_heater_toggle_pin >= 0) {
        gun->smoke_heater_toggle_monitor = pwm_monitor_create(config->smoke_heater_toggle_pin, NULL, NULL);
        if (!gun->smoke_heater_toggle_monitor) {
            fprintf(stderr, "[GUN] Warning: Failed to create smoke heater toggle monitor\n");
        } else {
            pwm_monitor_start(gun->smoke_heater_toggle_monitor);
            printf("[GUN] Smoke heater toggle monitoring on pin %d\n", config->smoke_heater_toggle_pin);
        }
    }
    
    // Create pitch servo if enabled
    if (config->pitch_servo.enabled) {
        gun->pitch_servo = servo_create(&config->pitch_servo);
        if (!gun->pitch_servo) {
            fprintf(stderr, "[GUN] Warning: Failed to create pitch servo\n");
        } else {
            gun->pitch_pwm_monitor = pwm_monitor_create(config->pitch_servo.pwm_pin, NULL, NULL);
            if (!gun->pitch_pwm_monitor) {
                fprintf(stderr, "[GUN] Warning: Failed to create pitch PWM monitor\n");
                servo_destroy(gun->pitch_servo);
                gun->pitch_servo = NULL;
            } else {
                pwm_monitor_start(gun->pitch_pwm_monitor);
                gun->pitch_pwm_pin = config->pitch_servo.pwm_pin;
                printf("[GUN] Pitch servo (input pin: %d, output pin: %d)\n", 
                       config->pitch_servo.pwm_pin, config->pitch_servo.output_pin);
            }
        }
    }
    
    // Create yaw servo if enabled
    if (config->yaw_servo.enabled) {
        gun->yaw_servo = servo_create(&config->yaw_servo);
        if (!gun->yaw_servo) {
            fprintf(stderr, "[GUN] Warning: Failed to create yaw servo\n");
        } else {
            gun->yaw_pwm_monitor = pwm_monitor_create(config->yaw_servo.pwm_pin, NULL, NULL);
            if (!gun->yaw_pwm_monitor) {
                fprintf(stderr, "[GUN] Warning: Failed to create yaw PWM monitor\n");
                servo_destroy(gun->yaw_servo);
                gun->yaw_servo = NULL;
            } else {
                pwm_monitor_start(gun->yaw_pwm_monitor);
                gun->yaw_pwm_pin = config->yaw_servo.pwm_pin;
                printf("[GUN] Yaw servo (input pin: %d, output pin: %d)\n", 
                       config->yaw_servo.pwm_pin, config->yaw_servo.output_pin);
            }
        }
    }
    
    // Start processing thread
    gun->processing_running = true;
    if (pthread_create(&gun->processing_thread, NULL, gun_fx_processing_thread, gun) != 0) {
        fprintf(stderr, "[GUN] Error: Failed to create processing thread\n");
        gun->processing_running = false;
        
        if (gun->trigger_pwm_monitor) {
            pwm_monitor_stop(gun->trigger_pwm_monitor);
            pwm_monitor_destroy(gun->trigger_pwm_monitor);
        }
        if (gun->smoke_heater_toggle_monitor) {
            pwm_monitor_stop(gun->smoke_heater_toggle_monitor);
            pwm_monitor_destroy(gun->smoke_heater_toggle_monitor);
        }
        if (gun->nozzle_flash) led_destroy(gun->nozzle_flash);
        if (gun->smoke) smoke_generator_destroy(gun->smoke);
        pthread_mutex_destroy(&gun->mutex);
        free(gun);
        return NULL;
    }
    
    printf("[GUN] Gun FX created\n");
    return gun;
}

void gun_fx_destroy(GunFX *gun) {
    if (!gun) return;
    
    // Stop processing thread
    if (gun->processing_running) {
        gun->processing_running = false;
        pthread_join(gun->processing_thread, NULL);
    }
    
    // Stop and destroy PWM monitors
    if (gun->trigger_pwm_monitor) {
        pwm_monitor_stop(gun->trigger_pwm_monitor);
        pwm_monitor_destroy(gun->trigger_pwm_monitor);
    }
    if (gun->smoke_heater_toggle_monitor) {
        pwm_monitor_stop(gun->smoke_heater_toggle_monitor);
        pwm_monitor_destroy(gun->smoke_heater_toggle_monitor);
    }
    if (gun->pitch_pwm_monitor) {
        pwm_monitor_stop(gun->pitch_pwm_monitor);
        pwm_monitor_destroy(gun->pitch_pwm_monitor);
    }
    if (gun->yaw_pwm_monitor) {
        pwm_monitor_stop(gun->yaw_pwm_monitor);
        pwm_monitor_destroy(gun->yaw_pwm_monitor);
    }
    
    // Destroy components
    if (gun->nozzle_flash) led_destroy(gun->nozzle_flash);
    if (gun->smoke) smoke_generator_destroy(gun->smoke);
    if (gun->pitch_servo) servo_destroy(gun->pitch_servo);
    if (gun->yaw_servo) servo_destroy(gun->yaw_servo);
    
    // Free rates array
    if (gun->rates) free(gun->rates);
    
    pthread_mutex_destroy(&gun->mutex);
    free(gun);
    
    printf("[GUN] Gun FX destroyed\n");
}

int gun_fx_set_rates_of_fire(GunFX *gun, const RateOfFire *rates, int count) {
    if (!gun || !rates || count <= 0) return -1;
    
    pthread_mutex_lock(&gun->mutex);
    
    // Free existing rates
    if (gun->rates) {
        free(gun->rates);
    }
    
    // Allocate and copy new rates
    gun->rates = (RateOfFire *)malloc(sizeof(RateOfFire) * count);
    if (!gun->rates) {
        fprintf(stderr, "[GUN] Error: Cannot allocate memory for rates\n");
        gun->rate_count = 0;
        pthread_mutex_unlock(&gun->mutex);
        return -1;
    }
    
    memcpy(gun->rates, rates, sizeof(RateOfFire) * count);
    gun->rate_count = count;
    
    pthread_mutex_unlock(&gun->mutex);
    
    printf("[GUN] Configured %d rate(s) of fire\n", count);
    return 0;
}

int gun_fx_get_current_rpm(GunFX *gun) {
    if (!gun) return 0;
    
    pthread_mutex_lock(&gun->mutex);
    int rpm = gun->current_rpm;
    pthread_mutex_unlock(&gun->mutex);
    
    return rpm;
}

bool gun_fx_is_firing(GunFX *gun) {
    if (!gun) return false;
    
    pthread_mutex_lock(&gun->mutex);
    bool firing = gun->is_firing;
    pthread_mutex_unlock(&gun->mutex);
    
    return firing;
}

/* Removed duplicate simple setter; keep mutex-protected version below */

Servo* gun_fx_get_pitch_servo(GunFX *gun) {
    if (!gun) return NULL;
    return gun->pitch_servo;
}

Servo* gun_fx_get_yaw_servo(GunFX *gun) {
    if (!gun) return NULL;
    return gun->yaw_servo;
}

void gun_fx_set_smoke_fan_off_delay(GunFX *gun, int delay_ms) {
    if (!gun) return;
    
    pthread_mutex_lock(&gun->mutex);
    gun->smoke_fan_off_delay_ms = delay_ms;
    pthread_mutex_unlock(&gun->mutex);
    
    printf("[GUN] Smoke fan off delay set to %d ms\n", delay_ms);
}
