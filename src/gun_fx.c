#include "gun_fx.h"
#include "config_loader.h"
#include "gpio.h"
#include "audio_player.h"
#include "lights.h"
#include "smoke_generator.h"
#include "servo.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
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
    thrd_t processing_thread;
    bool processing_running;
    mtx_t mutex;
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

// Update servo positions from averaged PWM inputs
static void update_servos(GunFX *gun) {
    if (gun->pitch_servo && gun->pitch_pwm_monitor) {
        int pitch_avg_us;
        if (pwm_monitor_get_average(gun->pitch_pwm_monitor, &pitch_avg_us)) {
            servo_set_input(gun->pitch_servo, pitch_avg_us);
        } else {
            static int pitch_warn_count = 0;
            if (pitch_warn_count < 5) {
                LOG_WARN(LOG_GUN, "No PWM average from pitch servo input (GPIO %d)", gun->pitch_pwm_pin);
                pitch_warn_count++;
            }
        }
    }
    
    if (gun->yaw_servo && gun->yaw_pwm_monitor) {
        int yaw_avg_us;
        if (pwm_monitor_get_average(gun->yaw_pwm_monitor, &yaw_avg_us)) {
            servo_set_input(gun->yaw_servo, yaw_avg_us);
        } else {
            static int yaw_warn_count = 0;
            if (yaw_warn_count < 5) {
                LOG_WARN(LOG_GUN, "No PWM average from yaw servo input (GPIO %d)", gun->yaw_pwm_pin);
                yaw_warn_count++;
            }
        }
    }
}

// Handle trigger input and rate selection
static int handle_trigger_input(GunFX *gun, int previous_rate_index, struct timespec *last_pwm_debug_time) {
    int trigger_avg_us;
    if (!gun->trigger_pwm_monitor || !pwm_monitor_get_average(gun->trigger_pwm_monitor, &trigger_avg_us)) {
        return previous_rate_index;
    }
    
    int new_rate_index = select_rate_of_fire(gun, trigger_avg_us, previous_rate_index);
    
    // Debug output every 10 seconds
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - last_pwm_debug_time->tv_sec) * 1000 + 
                     (now.tv_nsec - last_pwm_debug_time->tv_nsec) / 1000000;
    if (elapsed_ms >= 10000) {
        LOG_DEBUG(LOG_GUN, "Trigger PWM avg: %d µs, rate_index: %d", trigger_avg_us, new_rate_index);
        *last_pwm_debug_time = now;
    }
    
    // Log rate changes
    if (new_rate_index != previous_rate_index) {
        if (new_rate_index >= 0) {
            LOG_STATE(LOG_GUN, "IDLE", "RATE_SELECTED");
            LOG_INFO(LOG_GUN, "Rate selected: %d (%d RPM) @ %d µs",
                   new_rate_index + 1,
                   gun->rates[new_rate_index].rounds_per_minute,
                   trigger_avg_us);
        } else {
            LOG_STATE(LOG_GUN, "FIRING", "IDLE");
            LOG_DEBUG(LOG_GUN, "Trigger OFF (PWM avg: %d µs)", trigger_avg_us);
        }
    }
    
    return new_rate_index;
}

// Handle smoke heater toggle
static void handle_smoke_heater(GunFX *gun) {
    int heater_avg_us;
    if (!gun->smoke_heater_toggle_monitor || !pwm_monitor_get_average(gun->smoke_heater_toggle_monitor, &heater_avg_us)) {
        return;
    }
    
    bool heater_toggle_on = (heater_avg_us >= gun->smoke_heater_threshold);
    LOG_DEBUG(LOG_GUN, "Heater toggle PWM avg: %d µs (threshold: %d µs) -> %s",
             heater_avg_us, gun->smoke_heater_threshold, heater_toggle_on ? "ON" : "OFF");
    
    mtx_lock(&gun->mutex);
    if (heater_toggle_on && !gun->smoke_heater_on) {
        gun->smoke_heater_on = true;
        LOG_INFO(LOG_GUN, "Smoke heater ON (PWM avg: %d µs)", heater_avg_us);
        if (gun->smoke) {
            smoke_generator_heater_on(gun->smoke);
        }
    } else if (!heater_toggle_on && gun->smoke_heater_on) {
        gun->smoke_heater_on = false;
        LOG_INFO(LOG_GUN, "Smoke heater OFF (PWM avg: %d µs)", heater_avg_us);
        if (gun->smoke) {
            smoke_generator_heater_off(gun->smoke);
        }
    }
    mtx_unlock(&gun->mutex);
}

// Handle firing rate changes
static void handle_rate_change(GunFX *gun, int new_rate_index, int previous_rate_index) {
    if (new_rate_index == previous_rate_index) {
        return;
    }
    
    mtx_lock(&gun->mutex);
    gun->current_rate_index = new_rate_index;
    gun->is_firing = (new_rate_index >= 0);
    gun->current_rpm = (new_rate_index >= 0) ? gun->rates[new_rate_index].rounds_per_minute : 0;
    mtx_unlock(&gun->mutex);
    
    if (new_rate_index >= 0) {
        // Start or change firing rate
        int shot_interval_ms = (60 * 1000) / gun->rates[new_rate_index].rounds_per_minute;
        
        if (gun->nozzle_flash) {
            led_blink(gun->nozzle_flash, shot_interval_ms);
        }
        
        if (gun->smoke) {
            smoke_generator_fan_on(gun->smoke);
            gun->smoke_fan_pending_off = false;
        }
        
        if (gun->mixer && gun->rates[new_rate_index].sound) {
            PlaybackOptions opts = {.loop = true, .volume = 1.0f};
            audio_mixer_play(gun->mixer, gun->audio_channel, gun->rates[new_rate_index].sound, &opts);
        }
        
        if (previous_rate_index < 0) {
            LOG_STATE(LOG_GUN, "IDLE", "FIRING");
            LOG_INFO(LOG_GUN, "Firing started at %d RPM (rate %d) | Shot interval: %d ms",
                    gun->rates[new_rate_index].rounds_per_minute,
                    new_rate_index + 1,
                    shot_interval_ms);
        } else {
            LOG_STATE(LOG_GUN, "FIRING", "RATE_CHANGED");
            LOG_INFO(LOG_GUN, "Rate changed to %d RPM (rate %d) | Shot interval: %d ms",
                    gun->rates[new_rate_index].rounds_per_minute,
                    new_rate_index + 1,
                    shot_interval_ms);
        }
    } else {
        // Stop firing
        if (gun->nozzle_flash) {
            led_off(gun->nozzle_flash);
        }
        
        if (gun->mixer) {
            audio_mixer_stop_channel(gun->mixer, gun->audio_channel, STOP_IMMEDIATE);
        }
        
        if (gun->smoke) {
            if (gun->smoke_fan_off_delay_ms > 0) {
                clock_gettime(CLOCK_MONOTONIC, &gun->smoke_stop_time);
                gun->smoke_fan_pending_off = true;
                LOG_STATE(LOG_GUN, "FIRING", "STOPPING");
                LOG_INFO(LOG_GUN, "Firing stopped | Smoke fan will stop in %d ms", gun->smoke_fan_off_delay_ms);
            } else {
                smoke_generator_fan_off(gun->smoke);
                LOG_STATE(LOG_GUN, "FIRING", "IDLE");
                LOG_INFO(LOG_GUN, "Firing stopped immediately");
            }
        } else {
            LOG_STATE(LOG_GUN, "FIRING", "IDLE");
            LOG_INFO(LOG_GUN, "Firing stopped (no smoke)");
        }
    }
}

// Check and handle delayed smoke fan shutdown
static void handle_smoke_fan_delay(GunFX *gun) {
    if (!gun->smoke_fan_pending_off || !gun->smoke) {
        return;
    }
    
    struct timespec current_time_smoke;
    clock_gettime(CLOCK_MONOTONIC, &current_time_smoke);
    double elapsed_ms = ((current_time_smoke.tv_sec - gun->smoke_stop_time.tv_sec) * 1000.0) +
                       ((current_time_smoke.tv_nsec - gun->smoke_stop_time.tv_nsec) / 1e6);
    
    if (elapsed_ms >= gun->smoke_fan_off_delay_ms) {
        smoke_generator_fan_off(gun->smoke);
        gun->smoke_fan_pending_off = false;
        LOG_DEBUG(LOG_GUN, "Smoke fan stopped after %d ms delay (actual: %.1f ms)",
                 gun->smoke_fan_off_delay_ms, elapsed_ms);
    }
}

// Print periodic status output
static void print_status(GunFX *gun, struct timespec *last_status_time) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);
    double elapsed = (current_time.tv_sec - last_status_time->tv_sec) + 
                    (current_time.tv_nsec - last_status_time->tv_nsec) / 1e9;
    
    if (elapsed < 10.0) {
        return;
    }
    
    mtx_lock(&gun->mutex);
    
    int current_pwm = -1;
    if (gun->trigger_pwm_monitor) {
        int avg;
        if (pwm_monitor_get_average(gun->trigger_pwm_monitor, &avg)) {
            current_pwm = avg;
        }
    }
    
    int pitch_pwm = -1;
    if (gun->pitch_pwm_monitor) {
        int avg_pitch;
        if (pwm_monitor_get_average(gun->pitch_pwm_monitor, &avg_pitch)) {
            pitch_pwm = avg_pitch;
        }
    }
    
    int yaw_pwm = -1;
    if (gun->yaw_pwm_monitor) {
        int avg_yaw;
        if (pwm_monitor_get_average(gun->yaw_pwm_monitor, &avg_yaw)) {
            yaw_pwm = avg_yaw;
        }
    }
    
    int heater_toggle_pwm = -1;
    if (gun->smoke_heater_toggle_monitor) {
        int avg_heater;
        if (pwm_monitor_get_average(gun->smoke_heater_toggle_monitor, &avg_heater)) {
            heater_toggle_pwm = avg_heater;
        }
    }
    
    // ANSI color codes
    #define COLOR_RESET   "\033[0m"
    #define COLOR_BOLD    "\033[1m"
    #define COLOR_GREEN   "\033[32m"
    #define COLOR_RED     "\033[31m"
    #define COLOR_YELLOW  "\033[33m"
    #define COLOR_CYAN    "\033[36m"
    #define COLOR_MAGENTA "\033[35m"
    #define COLOR_BLUE    "\033[34m"
    
    // Pretty single-record status with colors
    LOG_STATUS(
        "\n"
        COLOR_CYAN COLOR_BOLD "╔═══════════════════════════════════════════════════════════════════════════╗\n" COLOR_RESET
        COLOR_CYAN COLOR_BOLD "║                          🎯 GUN STATUS @ %.1fs                            ║\n" COLOR_RESET
        COLOR_CYAN COLOR_BOLD "╠═══════════════════════════════════════════════════════════════════════════╣\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET " " COLOR_BOLD "Firing:" COLOR_RESET " %s%-4s" COLOR_RESET "  │  " COLOR_BOLD "Rate:" COLOR_RESET " %d  │  " COLOR_BOLD "RPM:" COLOR_RESET " %-4d                             " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "╠═══════════════════════════════════════════════════════════════════════════╣\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET " " COLOR_MAGENTA COLOR_BOLD "📍 GPIO PINS" COLOR_RESET "                                                              " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Trigger:      GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs                               " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Heater Tog:   GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs  [%s%-4s" COLOR_RESET "]                      " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Pitch Servo:  GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs  [%s%-9s" COLOR_RESET "]                 " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Yaw Servo:    GPIO %2d  " COLOR_BOLD "→" COLOR_RESET "  %s%-6s" COLOR_RESET " µs  [%s%-9s" COLOR_RESET "]                 " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Nozzle Flash: GPIO %2d                                                " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Smoke Fan:    GPIO %2d                                                " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN "║" COLOR_RESET "   • Smoke Heater: GPIO %2d                                                " COLOR_CYAN "║\n" COLOR_RESET
        COLOR_CYAN COLOR_BOLD "╚═══════════════════════════════════════════════════════════════════════════╝\n" COLOR_RESET,
        elapsed,
        gun->is_firing ? COLOR_GREEN : COLOR_RED, gun->is_firing ? "YES" : "NO",
        gun->current_rate_index >= 0 ? gun->current_rate_index + 1 : 0,
        gun->current_rpm,
        gun->trigger_pin,
        current_pwm >= 0 ? COLOR_YELLOW : COLOR_RED,
        current_pwm >= 0 ? (char[]){current_pwm/1000 + '0', (current_pwm/100)%10 + '0', (current_pwm/10)%10 + '0', current_pwm%10 + '0', 0} : "n/a",
        gun->smoke_heater_toggle_pin,
        heater_toggle_pwm >= 0 ? COLOR_YELLOW : COLOR_RED,
        heater_toggle_pwm >= 0 ? (char[]){heater_toggle_pwm/1000 + '0', (heater_toggle_pwm/100)%10 + '0', (heater_toggle_pwm/10)%10 + '0', heater_toggle_pwm%10 + '0', 0} : "n/a",
        gun->smoke_heater_on ? COLOR_GREEN : COLOR_RED,
        gun->smoke_heater_on ? "ON" : "OFF",
        gun->pitch_pwm_pin,
        pitch_pwm >= 0 ? COLOR_YELLOW : COLOR_RED,
        pitch_pwm >= 0 ? (char[]){pitch_pwm/1000 + '0', (pitch_pwm/100)%10 + '0', (pitch_pwm/10)%10 + '0', pitch_pwm%10 + '0', 0} : "n/a",
        gun->pitch_servo ? COLOR_GREEN : COLOR_RED,
        gun->pitch_servo ? "ACTIVE" : "DISABLED",
        gun->yaw_pwm_pin,
        yaw_pwm >= 0 ? COLOR_YELLOW : COLOR_RED,
        yaw_pwm >= 0 ? (char[]){yaw_pwm/1000 + '0', (yaw_pwm/100)%10 + '0', (yaw_pwm/10)%10 + '0', yaw_pwm%10 + '0', 0} : "n/a",
        gun->yaw_servo ? COLOR_GREEN : COLOR_RED,
        gun->yaw_servo ? "ACTIVE" : "DISABLED",
        gun->nozzle_flash_pin,
        gun->smoke_fan_pin,
        gun->smoke_heater_pin
    );
    
    mtx_unlock(&gun->mutex);
    *last_status_time = current_time;
}

// Processing thread to monitor PWM and handle firing
static int gun_fx_processing_thread(void *arg) {
    GunFX *gun = (GunFX *)arg;
    
    LOG_INFO(LOG_GUN, "Processing thread started");
    
    struct timespec last_status_time;
    clock_gettime(CLOCK_MONOTONIC, &last_status_time);
    
    struct timespec last_pwm_debug_time;
    clock_gettime(CLOCK_MONOTONIC, &last_pwm_debug_time);
    
    while (gun->processing_running) {
        // Update servos
        update_servos(gun);
        
        // Get current rate and handle trigger
        mtx_lock(&gun->mutex);
        int previous_rate_index = gun->current_rate_index;
        mtx_unlock(&gun->mutex);
        
        int new_rate_index = handle_trigger_input(gun, previous_rate_index, &last_pwm_debug_time);
        
        // Handle smoke heater
        handle_smoke_heater(gun);
        
        // Handle rate changes and firing logic
        handle_rate_change(gun, new_rate_index, previous_rate_index);
        
        // Check smoke fan delay
        handle_smoke_fan_delay(gun);
        
        // Periodic status output
        print_status(gun, &last_status_time);
        
        usleep(10000); // 10ms loop
    }
    
    LOG_INFO(LOG_GUN, "Processing thread stopped");
    return thrd_success;
}

GunFX* gun_fx_create(AudioMixer *mixer, int audio_channel,
                     const GunFXConfig *config) {
    if (!config) {
        LOG_ERROR(LOG_GUN, "Config is nullptr");
        return nullptr;
    }
    
    GunFX *gun = malloc(sizeof(GunFX));
    if (!gun) {
        LOG_ERROR(LOG_GUN, "Cannot allocate memory for gun FX");
        return nullptr;
    }
    
    gun->mixer = mixer;
    gun->audio_channel = audio_channel;
    gun->trigger_pwm_pin = config->trigger.pin;
    gun->smoke_heater_toggle_pin = config->smoke.heater_toggle_pin;
    gun->smoke_heater_threshold = config->smoke.heater_pwm_threshold_us;
    gun->smoke_fan_off_delay_ms = config->smoke.fan_off_delay_ms;
    gun->rates = nullptr;
    gun->rate_count = 0;
    gun->is_firing = false;
    gun->current_rpm = 0;
    gun->current_rate_index = -1;  // Not firing initially
    gun->smoke_heater_on = false;
    gun->processing_running = false;
    gun->pitch_servo = nullptr;
    gun->yaw_servo = nullptr;
    gun->pitch_pwm_monitor = nullptr;
    gun->yaw_pwm_monitor = nullptr;
    mtx_init(&gun->mutex, mtx_plain);
    
    // Create nozzle flash LED if pin specified
    if (config->nozzle_flash.pin >= 0) {
        gun->nozzle_flash = led_create(config->nozzle_flash.pin);
        if (!gun->nozzle_flash) {
            LOG_WARN(LOG_GUN, "Failed to create nozzle flash LED on GPIO %d", config->nozzle_flash.pin);
        } else {
            LOG_DEBUG(LOG_GUN, "Nozzle flash LED initialized on GPIO %d", config->nozzle_flash.pin);
        }
    }
    
    // Create smoke generator if pins specified
    // Create smoke generator if pins specified
    if (config->smoke.fan_pin >= 0 && config->smoke.heater_pin >= 0) {
        gun->smoke = smoke_generator_create(config->smoke.heater_pin, config->smoke.fan_pin);
        if (!gun->smoke) {
            LOG_WARN(LOG_GUN, "Failed to create smoke generator (heater GPIO %d, fan GPIO %d)",
                    config->smoke.heater_pin, config->smoke.fan_pin);
        } else {
            LOG_DEBUG(LOG_GUN, "Smoke generator initialized (heater GPIO %d, fan GPIO %d)",
                     config->smoke.heater_pin, config->smoke.fan_pin);
        }
    }
    
    // Create trigger PWM monitor if pin specified
    if (config->trigger.pin >= 0) {
        gun->trigger_pwm_monitor = pwm_monitor_create_with_name(config->trigger.pin, "Gun Trigger", nullptr, nullptr);
        if (!gun->trigger_pwm_monitor) {
            LOG_WARN(LOG_GUN, "Failed to create trigger PWM monitor on GPIO %d", config->trigger.pin);
        } else {
            pwm_monitor_start(gun->trigger_pwm_monitor);
            LOG_DEBUG(LOG_GUN, "Trigger PWM monitoring started on GPIO %d", config->trigger.pin);
        }
    }
    
    // Create smoke heater toggle PWM monitor if pin specified
    if (config->smoke.heater_toggle_pin >= 0) {
        gun->smoke_heater_toggle_monitor = pwm_monitor_create_with_name(config->smoke.heater_toggle_pin, "Smoke Heater Toggle", nullptr, nullptr);
        if (!gun->smoke_heater_toggle_monitor) {
            LOG_WARN(LOG_GUN, "Failed to create smoke heater toggle monitor on GPIO %d",
                    config->smoke.heater_toggle_pin);
        } else {
            pwm_monitor_start(gun->smoke_heater_toggle_monitor);
            LOG_DEBUG(LOG_GUN, "Smoke heater toggle monitoring started on GPIO %d (threshold: %d µs)",
                     config->smoke.heater_toggle_pin, config->smoke.heater_pwm_threshold_us);
        }
    }
    
    // Create pitch servo if enabled
    if (config->turret_control.pitch.enabled) {
        gun->pitch_servo = servo_create(&config->turret_control.pitch);
        if (!gun->pitch_servo) {
            LOG_WARN(LOG_GUN, "Failed to create pitch servo");
        } else {
            gun->pitch_pwm_monitor = pwm_monitor_create_with_name(config->turret_control.pitch.pwm_pin, "Turret Pitch Servo", nullptr, nullptr);
            if (!gun->pitch_pwm_monitor) {
                LOG_WARN(LOG_GUN, "Failed to create pitch PWM monitor on GPIO %d",
                        config->turret_control.pitch.pwm_pin);
                servo_destroy(gun->pitch_servo);
                gun->pitch_servo = nullptr;
            } else {
                pwm_monitor_start(gun->pitch_pwm_monitor);
                gun->pitch_pwm_pin = config->turret_control.pitch.pwm_pin;
                LOG_DEBUG(LOG_GUN, "Pitch servo initialized (input GPIO %d, output GPIO %d)",
                         config->turret_control.pitch.pwm_pin, config->turret_control.pitch.output_pin);
            }
        }
    }
    
    // Create yaw servo if enabled
    if (config->turret_control.yaw.enabled) {
        gun->yaw_servo = servo_create(&config->turret_control.yaw);
        if (!gun->yaw_servo) {
            LOG_WARN(LOG_GUN, "Failed to create yaw servo");
        } else {
            gun->yaw_pwm_monitor = pwm_monitor_create_with_name(config->turret_control.yaw.pwm_pin, "Turret Yaw Servo", nullptr, nullptr);
            if (!gun->yaw_pwm_monitor) {
                LOG_WARN(LOG_GUN, "Failed to create yaw PWM monitor on GPIO %d",
                        config->turret_control.yaw.pwm_pin);
                servo_destroy(gun->yaw_servo);
                gun->yaw_servo = nullptr;
            } else {
                pwm_monitor_start(gun->yaw_pwm_monitor);
                gun->yaw_pwm_pin = config->turret_control.yaw.pwm_pin;
                LOG_DEBUG(LOG_GUN, "Yaw servo initialized (input GPIO %d, output GPIO %d)",
                         config->turret_control.yaw.pwm_pin, config->turret_control.yaw.output_pin);
            }
        }
    }
    
    // Start processing thread
    gun->processing_running = true;
    if (thrd_create(&gun->processing_thread, gun_fx_processing_thread, gun) != thrd_success) {
        LOG_ERROR(LOG_GUN, "Failed to create processing thread");
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
        mtx_destroy(&gun->mutex);
        free(gun);
        return nullptr;
    }
    
    LOG_INIT(LOG_GUN, "Gun FX system ready");
    return gun;
}

void gun_fx_destroy(GunFX *gun) {
    if (!gun) return;
    
    // Stop processing thread
    if (gun->processing_running) {
        gun->processing_running = false;
        thrd_join(gun->processing_thread, nullptr);
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
    
    mtx_destroy(&gun->mutex);
    free(gun);
    
    LOG_SHUTDOWN(LOG_GUN, "Gun FX system");
}

int gun_fx_set_rates_of_fire(GunFX *gun, const RateOfFire *rates, int count) {
    if (!gun || !rates || count <= 0) return -1;
    
    mtx_lock(&gun->mutex);
    
    // Free existing rates
    if (gun->rates) {
        free(gun->rates);
    }
    
    // Allocate and copy new rates
    gun->rates = (RateOfFire *)malloc(sizeof(RateOfFire) * count);
    if (!gun->rates) {
        LOG_ERROR(LOG_GUN, "Cannot allocate memory for rates array (%d entries)", count);
        gun->rate_count = 0;
        mtx_unlock(&gun->mutex);
        return -1;
    }
    
    memcpy(gun->rates, rates, sizeof(RateOfFire) * count);
    gun->rate_count = count;
    
    mtx_unlock(&gun->mutex);
    
    LOG_INFO(LOG_GUN, "Configured %d rate(s) of fire", count);
    LOG_DEBUG(LOG_GUN, "Rates: %s", count > 0 ? "updated" : "empty");
    return 0;
}

int gun_fx_get_current_rpm(GunFX *gun) {
    if (!gun) return 0;
    
    mtx_lock(&gun->mutex);
    int rpm = gun->current_rpm;
    mtx_unlock(&gun->mutex);
    
    return rpm;
}

int gun_fx_get_current_rate_index(GunFX *gun) {
    if (!gun) return -1;
    
    mtx_lock(&gun->mutex);
    int rate = gun->current_rate_index;
    mtx_unlock(&gun->mutex);
    
    return rate;
}

bool gun_fx_is_firing(GunFX *gun) {
    if (!gun) return false;
    
    mtx_lock(&gun->mutex);
    bool firing = gun->is_firing;
    mtx_unlock(&gun->mutex);
    
    return firing;
}

/* Removed duplicate simple setter; keep mutex-protected version below */

Servo* gun_fx_get_pitch_servo(GunFX *gun) {
    if (!gun) return nullptr;
    return gun->pitch_servo;
}

Servo* gun_fx_get_yaw_servo(GunFX *gun) {
    if (!gun) return nullptr;
    return gun->yaw_servo;
}

void gun_fx_set_smoke_fan_off_delay(GunFX *gun, int delay_ms) {
    if (!gun) return;
    
    mtx_lock(&gun->mutex);
    gun->smoke_fan_off_delay_ms = delay_ms;
    mtx_unlock(&gun->mutex);
    
    LOG_INFO(LOG_GUN, "Smoke fan off delay set to %d ms", delay_ms);
}
