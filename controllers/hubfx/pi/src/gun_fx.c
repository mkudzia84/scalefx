#include "gun_fx.h"
#include "config_loader.h"
#include "gpio.h"
#include "audio_player.h"
#include "serial_bus.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <threads.h>
#include <unistd.h>
#include <string.h>

// ---------------------- Binary Protocol (PC -> Pico) ----------------------
// Packet: [type:u8][len:u8][payload:len][crc8(0x07 over type+len+payload)] then COBS-encoded and terminated with 0x00
// Types (mirror pico firmware):
// 0x01 TRIGGER_ON    payload rpm:u16le
// 0x02 TRIGGER_OFF   payload fan_delay_ms:u16le
// 0x10 SRV_SET       payload servo_id:u8, pulse_us:u16le
// 0x11 SRV_SETTINGS  payload servo_id:u8, min:u16le, max:u16le, max_speed:u16le, accel:u16le, decel:u16le
// 0x12 SRV_RECOIL_JERK payload servo_id:u8, jerk_us:u16le, variance_us:u16le
// 0x20 SMOKE_HEAT    payload on:u8 (0/1)
// 0xF0 INIT          payload none (daemon initialization - reset to safe state)
// 0xF1 SHUTDOWN      payload none (daemon shutdown - enter safe state)
// 0xF2 KEEPALIVE     payload none (periodic keepalive from daemon)
// Telemetry (Pico -> PC):
// 0xF3 INIT_READY    payload module_name:string (sent in response to INIT)
// 0xF4 STATUS        payload flags:u8, fan_off_remaining_ms:u16le, servo_us[3]:u16le each, rate_of_fire_rpm:u16le

static const uint8_t PKT_TRIGGER_ON   = 0x01;
static const uint8_t PKT_TRIGGER_OFF  = 0x02;
static const uint8_t PKT_SRV_SET      = 0x10;
static const uint8_t PKT_SRV_SETTINGS = 0x11;
static const uint8_t PKT_SRV_RECOIL_JERK = 0x12;
static const uint8_t PKT_SMOKE_HEAT   = 0x20;

// Universal protocol packets (high values, used across all modules)
static const uint8_t PKT_INIT         = 0xF0;
static const uint8_t PKT_SHUTDOWN     = 0xF1;
static const uint8_t PKT_KEEPALIVE    = 0xF2;
static const uint8_t PKT_INIT_READY   = 0xF3;
static const uint8_t PKT_STATUS       = 0xF4;


struct GunFX {
    // Audio
    AudioMixer *mixer;
    int audio_channel;

    // Serial bus to Pico
    SerialBus *serial_bus;
    SerialBusConfig serial_bus_config;
    
    // PWM monitoring for trigger and heater toggle
    PWMMonitor *trigger_pwm_monitor;
    int trigger_pwm_pin;
    PWMMonitor *smoke_heater_toggle_monitor;
    int smoke_heater_toggle_pin;
    int smoke_heater_threshold;
    
    // Servo inputs (pitch/yaw) mapped to Pico servo IDs
    PWMMonitor *pitch_pwm_monitor;
    PWMMonitor *yaw_pwm_monitor;
    int pitch_pwm_pin;
    int yaw_pwm_pin;
    ServoConfig pitch_cfg;
    ServoConfig yaw_cfg;
    int pitch_servo_id;
    int yaw_servo_id;
    int last_pitch_output_us;
    int last_yaw_output_us;
    
    // Rates of fire
    RateOfFire *rates;
    int rate_count;
    
    // Current state
    atomic_bool is_firing;
    atomic_int current_rpm;
    atomic_int current_rate_index;  // Currently active rate (-1 = not firing)
    atomic_bool smoke_heater_on;
    
    // Protocol timing
    struct timespec last_keepalive_time;
    
    // Processing thread
    thrd_t processing_thread;
    atomic_bool processing_running;
};

// ---------------------- Protocol helpers ----------------------

static void send_init(GunFX *gun) {
    if (!gun || !gun->serial_bus) return;
    serial_bus_send_packet(gun->serial_bus, PKT_INIT, NULL, 0);
    LOG_DEBUG(LOG_GUN, "Sent INIT to Pico");
}

static void send_shutdown(GunFX *gun) {
    if (!gun || !gun->serial_bus) return;
    serial_bus_send_packet(gun->serial_bus, PKT_SHUTDOWN, NULL, 0);
    LOG_DEBUG(LOG_GUN, "Sent SHUTDOWN to Pico");
}

static void send_keepalive(GunFX *gun) {
    if (!gun || !gun->serial_bus) return;
    serial_bus_send_packet(gun->serial_bus, PKT_KEEPALIVE, NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &gun->last_keepalive_time);
}

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

static int map_input_to_output_us(const ServoConfig *cfg, int input_us) {
    if (input_us < cfg->input_min_us) input_us = cfg->input_min_us;
    if (input_us > cfg->input_max_us) input_us = cfg->input_max_us;

    float input_range = (float)(cfg->input_max_us - cfg->input_min_us);
    float output_range = (float)(cfg->output_max_us - cfg->output_min_us);
    if (input_range <= 0.0f) return cfg->output_min_us;

    float normalized = (float)(input_us - cfg->input_min_us) / input_range;
    int output_us = cfg->output_min_us + (int)(normalized * output_range);
    if (output_us < cfg->output_min_us) output_us = cfg->output_min_us;
    if (output_us > cfg->output_max_us) output_us = cfg->output_max_us;
    return output_us;
}

static void send_servo_command(GunFX *gun, int servo_id, int output_us, int *last_sent) {
    if (last_sent && *last_sent == output_us) return;
    if (!gun->serial_bus) {
        static int warn_count = 0;
        if (warn_count < 5) {
            LOG_ERROR(LOG_GUN, "Serial bus not available; cannot send servo command");
            warn_count++;
        }
        return;
    }

    uint8_t payload[3];
    payload[0] = (uint8_t)servo_id;
    payload[1] = (uint8_t)(output_us & 0xFF);
    payload[2] = (uint8_t)((output_us >> 8) & 0xFF);
    if (serial_bus_send_packet(gun->serial_bus, PKT_SRV_SET, payload, sizeof(payload)) == 0 && last_sent) {
        *last_sent = output_us;
    }
}

// Send servo configuration (settings) to Pico
static void send_servo_setup(GunFX *gun, int servo_id, const ServoConfig *cfg) {
    if (!gun->serial_bus || servo_id <= 0) return;
    
    uint8_t payload[11];
    payload[0] = (uint8_t)servo_id;
    payload[1] = (uint8_t)(cfg->output_min_us & 0xFF);
    payload[2] = (uint8_t)((cfg->output_min_us >> 8) & 0xFF);
    payload[3] = (uint8_t)(cfg->output_max_us & 0xFF);
    payload[4] = (uint8_t)((cfg->output_max_us >> 8) & 0xFF);
    payload[5] = (uint8_t)((int)cfg->max_speed_us_per_sec & 0xFF);
    payload[6] = (uint8_t)(((int)cfg->max_speed_us_per_sec >> 8) & 0xFF);
    payload[7] = (uint8_t)((int)cfg->max_accel_us_per_sec2 & 0xFF);
    payload[8] = (uint8_t)(((int)cfg->max_accel_us_per_sec2 >> 8) & 0xFF);
    payload[9] = (uint8_t)((int)cfg->max_decel_us_per_sec2 & 0xFF);
    payload[10] = (uint8_t)(((int)cfg->max_decel_us_per_sec2 >> 8) & 0xFF);
    serial_bus_send_packet(gun->serial_bus, PKT_SRV_SETTINGS, payload, sizeof(payload));
}

// Send servo recoil jerk settings to Pico
static void send_servo_recoil_jerk(GunFX *gun, int servo_id, const ServoConfig *cfg) {
    if (!gun->serial_bus || servo_id <= 0) return;
    
    uint8_t payload[5];
    payload[0] = (uint8_t)servo_id;
    payload[1] = (uint8_t)(cfg->recoil_jerk_us & 0xFF);
    payload[2] = (uint8_t)((cfg->recoil_jerk_us >> 8) & 0xFF);
    payload[3] = (uint8_t)(cfg->recoil_jerk_variance_us & 0xFF);
    payload[4] = (uint8_t)((cfg->recoil_jerk_variance_us >> 8) & 0xFF);
    serial_bus_send_packet(gun->serial_bus, PKT_SRV_RECOIL_JERK, payload, sizeof(payload));
    
    LOG_DEBUG(LOG_GUN, "Sent recoil jerk for servo %d: jerk=%d, variance=%d", 
             servo_id, cfg->recoil_jerk_us, cfg->recoil_jerk_variance_us);
}

// Update servo positions from averaged PWM inputs
static void update_servos(GunFX *gun) {
    if (gun->pitch_pwm_monitor && gun->pitch_cfg.servo_id > 0) {
        int pitch_avg_us;
        if (pwm_monitor_get_average(gun->pitch_pwm_monitor, &pitch_avg_us)) {
            int output_us = map_input_to_output_us(&gun->pitch_cfg, pitch_avg_us);
            send_servo_command(gun, gun->pitch_servo_id, output_us, &gun->last_pitch_output_us);
        } else {
            static int pitch_warn_count = 0;
            if (pitch_warn_count < 5) {
                LOG_WARN(LOG_GUN, "No PWM average from pitch servo input (GPIO %d)", gun->pitch_pwm_pin);
                pitch_warn_count++;
            }
        }
    }
    
    if (gun->yaw_pwm_monitor && gun->yaw_cfg.servo_id > 0) {
        int yaw_avg_us;
        if (pwm_monitor_get_average(gun->yaw_pwm_monitor, &yaw_avg_us)) {
            int output_us = map_input_to_output_us(&gun->yaw_cfg, yaw_avg_us);
            send_servo_command(gun, gun->yaw_servo_id, output_us, &gun->last_yaw_output_us);
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
    
    bool current_heater_on = atomic_load(&gun->smoke_heater_on);
    if (heater_toggle_on && !current_heater_on) {
        atomic_store(&gun->smoke_heater_on, true);
        LOG_INFO(LOG_GUN, "Smoke heater ON (PWM avg: %d µs)", heater_avg_us);
        if (gun->serial_bus) {
            uint8_t payload = 1;
            serial_bus_send_packet(gun->serial_bus, PKT_SMOKE_HEAT, &payload, 1);
        }
    } else if (!heater_toggle_on && current_heater_on) {
        atomic_store(&gun->smoke_heater_on, false);
        LOG_INFO(LOG_GUN, "Smoke heater OFF (PWM avg: %d µs)");
        if (gun->serial_bus) {
            uint8_t payload = 0;
            serial_bus_send_packet(gun->serial_bus, PKT_SMOKE_HEAT, &payload, 1);
        }
    }
}

// Handle firing rate changes
static void handle_rate_change(GunFX *gun, int new_rate_index, int previous_rate_index) {
    if (new_rate_index == previous_rate_index) {
        return;
    }
    
    atomic_store(&gun->current_rate_index, new_rate_index);
    atomic_store(&gun->is_firing, (new_rate_index >= 0));
    atomic_store(&gun->current_rpm, (new_rate_index >= 0) ? gun->rates[new_rate_index].rounds_per_minute : 0);
    
    if (new_rate_index >= 0) {
        // Start or change firing rate
        int rpm = gun->rates[new_rate_index].rounds_per_minute;
        
        // Send TRIGGER_ON command to Pico
        if (gun->serial_bus) {
            uint8_t payload[2] = { (uint8_t)(rpm & 0xFF), (uint8_t)((rpm >> 8) & 0xFF) };
            serial_bus_send_packet(gun->serial_bus, PKT_TRIGGER_ON, payload, sizeof(payload));
        }
        
        if (gun->mixer && gun->rates[new_rate_index].sound) {
            PlaybackOptions opts = {.loop = true, .volume = 1.0f};
            audio_mixer_play(gun->mixer, gun->audio_channel, gun->rates[new_rate_index].sound, &opts);
        }
        
        if (previous_rate_index < 0) {
            LOG_STATE(LOG_GUN, "IDLE", "FIRING");
            LOG_INFO(LOG_GUN, "Firing started at %d RPM (rate %d)", rpm, new_rate_index + 1);
        } else {
            LOG_STATE(LOG_GUN, "FIRING", "RATE_CHANGED");
            LOG_INFO(LOG_GUN, "Rate changed to %d RPM (rate %d)", rpm, new_rate_index + 1);
        }
    } else {
        // Stop firing - send TRIGGER_OFF; Pico handles fan delay internally
        if (gun->serial_bus) {
            uint8_t payload[1] = { 0 }; // Fan delay handled by Pico
            serial_bus_send_packet(gun->serial_bus, PKT_TRIGGER_OFF, payload, sizeof(payload));
        }
        
        if (gun->mixer) {
            audio_mixer_stop_channel(gun->mixer, gun->audio_channel, STOP_IMMEDIATE);
        }
        
        LOG_STATE(LOG_GUN, "FIRING", "IDLE");
        LOG_INFO(LOG_GUN, "Firing stopped (fan timing handled by Pico)");
    }
}




// Setup servo PWM monitoring and send initial servo settings to Pico
static int setup_servos(GunFX *gun, const GunFXConfig *config) {
    // Create pitch PWM monitor if valid channel specified
    if (gun->pitch_pwm_pin >= 0) {
        gun->pitch_pwm_monitor = pwm_monitor_create_with_name(gun->pitch_pwm_pin, "Turret Pitch Servo", nullptr, nullptr);
        if (!gun->pitch_pwm_monitor) {
            LOG_ERROR(LOG_GUN, "Failed to create pitch PWM monitor on channel %d (GPIO %d)", 
                     config->turret_control.pitch.input_channel, gun->pitch_pwm_pin);
            return -1;
        }
        pwm_monitor_start(gun->pitch_pwm_monitor);
        LOG_DEBUG(LOG_GUN, "Pitch servo input monitoring started on channel %d (GPIO %d, servo_id=%d)",
                 config->turret_control.pitch.input_channel, gun->pitch_pwm_pin, gun->pitch_cfg.servo_id);
    }
    
    // Create yaw PWM monitor if valid channel specified
    if (gun->yaw_pwm_pin >= 0) {
        gun->yaw_pwm_monitor = pwm_monitor_create_with_name(gun->yaw_pwm_pin, "Turret Yaw Servo", nullptr, nullptr);
        if (!gun->yaw_pwm_monitor) {
            LOG_ERROR(LOG_GUN, "Failed to create yaw PWM monitor on channel %d (GPIO %d)", 
                     config->turret_control.yaw.input_channel, gun->yaw_pwm_pin);
            return -1;
        }
        pwm_monitor_start(gun->yaw_pwm_monitor);
        LOG_DEBUG(LOG_GUN, "Yaw servo input monitoring started on channel %d (GPIO %d, servo_id=%d)",
                 config->turret_control.yaw.input_channel, gun->yaw_pwm_pin, gun->yaw_cfg.servo_id);
    }
    
    // Send initial servo settings to Pico
    send_servo_setup(gun, gun->pitch_cfg.servo_id, &gun->pitch_cfg);
    send_servo_setup(gun, gun->yaw_cfg.servo_id, &gun->yaw_cfg);
    
    // Send recoil jerk settings to Pico
    send_servo_recoil_jerk(gun, gun->pitch_cfg.servo_id, &gun->pitch_cfg);
    send_servo_recoil_jerk(gun, gun->yaw_cfg.servo_id, &gun->yaw_cfg);
    
    return 0;
}

// Processing thread to monitor PWM and handle firing
static int gun_fx_processing_thread(void *arg) {
    GunFX *gun = (GunFX *)arg;
    
    LOG_INFO(LOG_GUN, "Processing thread started");
    
    struct timespec last_pwm_debug_time;
    clock_gettime(CLOCK_MONOTONIC, &last_pwm_debug_time);
    
    // Send keepalive every 30 seconds (Pico watchdog timeout is 90s)
    const long KEEPALIVE_INTERVAL_MS = 30000;
    
    while (atomic_load(&gun->processing_running)) {
        // Send periodic keepalive to Pico
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - gun->last_keepalive_time.tv_sec) * 1000 + 
                         (now.tv_nsec - gun->last_keepalive_time.tv_nsec) / 1000000;
        if (elapsed_ms >= KEEPALIVE_INTERVAL_MS) {
            send_keepalive(gun);
        }
        
        // Update servos
        update_servos(gun);
        
        // Get current rate and handle trigger
        int previous_rate_index = atomic_load(&gun->current_rate_index);
        
        int new_rate_index = handle_trigger_input(gun, previous_rate_index, &last_pwm_debug_time);
        
        // Handle smoke heater
        handle_smoke_heater(gun);
        
        // Handle rate changes and firing logic
        handle_rate_change(gun, new_rate_index, previous_rate_index);
        
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
    
    // Convert input channels to GPIO pins
    int trigger_gpio = channel_to_gpio(config->trigger.input_channel);
    int smoke_heater_gpio = channel_to_gpio(config->smoke.heater_toggle_channel);
    
    gun->trigger_pwm_pin = trigger_gpio;
    gun->smoke_heater_toggle_pin = smoke_heater_gpio;
    gun->smoke_heater_threshold = config->smoke.heater_pwm_threshold_us;
    gun->pitch_cfg = config->turret_control.pitch;
    gun->yaw_cfg = config->turret_control.yaw;
    
    // Convert servo input channels to GPIO pins
    int pitch_gpio = channel_to_gpio(config->turret_control.pitch.input_channel);
    int yaw_gpio = channel_to_gpio(config->turret_control.yaw.input_channel);
    gun->pitch_pwm_pin = pitch_gpio;
    gun->yaw_pwm_pin = yaw_gpio;
    gun->pitch_servo_id = config->turret_control.pitch.servo_id;
    gun->yaw_servo_id = config->turret_control.yaw.servo_id;
    gun->last_pitch_output_us = -1;
    gun->last_yaw_output_us = -1;
    gun->rates = nullptr;
    gun->rate_count = 0;
    atomic_init(&gun->is_firing, false);
    atomic_init(&gun->current_rpm, 0);
    atomic_init(&gun->current_rate_index, -1);  // Not firing initially
    atomic_init(&gun->smoke_heater_on, false);
    atomic_init(&gun->processing_running, false);
    gun->pitch_pwm_monitor = nullptr;
    gun->yaw_pwm_monitor = nullptr;
    
    // Open serial bus to gunfx_pico by USB VID/PID
    // Use default configuration: baud_rate=115200, timeout_ms=100
    SerialBusConfig pico_config = {
        .device_path = "",  // Will be populated by serial_bus_open_by_vid_pid
        .baud_rate = 115200,
        .timeout_ms = 100
    };
    
    // USB VID/PID for gunfx_pico device
    const uint16_t GUNFX_PICO_VID = 0x2e8a;  // Raspberry Pi Foundation
    const uint16_t GUNFX_PICO_PID = 0x0180;  // gunfx_pico
    
    gun->serial_bus = serial_bus_open_by_vid_pid(GUNFX_PICO_VID, GUNFX_PICO_PID, &pico_config);
    if (!gun->serial_bus) {
        LOG_ERROR(LOG_GUN, "Failed to detect gunfx_pico (VID=%04x, PID=%04x) - is device plugged in?",
                 GUNFX_PICO_VID, GUNFX_PICO_PID);
        free(gun);
        return nullptr;
    }
    
    gun->serial_bus_config = pico_config;
    
    LOG_DEBUG(LOG_GUN, "gunfx_pico opened on %s", gun->serial_bus_config.device_path);
    
    // Initialize keepalive timer and send INIT
    clock_gettime(CLOCK_MONOTONIC, &gun->last_keepalive_time);
    send_init(gun);
    
    // Small delay to allow Pico to respond with INIT_READY
    usleep(100000); // 100ms

    // Create trigger PWM monitor if valid channel specified
    if (trigger_gpio >= 0) {
        gun->trigger_pwm_monitor = pwm_monitor_create_with_name(trigger_gpio, "Gun Trigger", nullptr, nullptr);
        if (!gun->trigger_pwm_monitor) {
            LOG_ERROR(LOG_GUN, "Failed to create trigger PWM monitor on channel %d (GPIO %d)", 
                     config->trigger.input_channel, trigger_gpio);
        } else {
            pwm_monitor_start(gun->trigger_pwm_monitor);
            LOG_DEBUG(LOG_GUN, "Trigger PWM monitoring started on channel %d (GPIO %d)", 
                     config->trigger.input_channel, trigger_gpio);
        }
    }
    
    // Create smoke heater toggle PWM monitor if valid channel specified
    if (smoke_heater_gpio >= 0) {
        gun->smoke_heater_toggle_monitor = pwm_monitor_create_with_name(smoke_heater_gpio, "Smoke Heater Toggle", nullptr, nullptr);
        if (!gun->smoke_heater_toggle_monitor) {
            LOG_WARN(LOG_GUN, "Failed to create smoke heater toggle monitor on channel %d (GPIO %d)",
                    config->smoke.heater_toggle_channel, smoke_heater_gpio);
        } else {
            pwm_monitor_start(gun->smoke_heater_toggle_monitor);
            LOG_DEBUG(LOG_GUN, "Smoke heater toggle monitoring started on channel %d (GPIO %d, threshold: %d µs)",
                     config->smoke.heater_toggle_channel, smoke_heater_gpio, config->smoke.heater_pwm_threshold_us);
        }
    }
    
    // Setup servo monitoring and configuration
    if (setup_servos(gun, config) != 0) {
        LOG_ERROR(LOG_GUN, "Failed to setup servos");
        
        if (gun->trigger_pwm_monitor) {
            pwm_monitor_stop(gun->trigger_pwm_monitor);
            pwm_monitor_destroy(gun->trigger_pwm_monitor);
        }
        if (gun->smoke_heater_toggle_monitor) {
            pwm_monitor_stop(gun->smoke_heater_toggle_monitor);
            pwm_monitor_destroy(gun->smoke_heater_toggle_monitor);
        }
        if (gun->serial_bus) serial_bus_close(gun->serial_bus);
        free(gun);
        return nullptr;
    }

    // Start processing thread
    atomic_store(&gun->processing_running, true);
    if (thrd_create(&gun->processing_thread, gun_fx_processing_thread, gun) != thrd_success) {
        LOG_ERROR(LOG_GUN, "Failed to create processing thread");
        atomic_store(&gun->processing_running, false);
        
        if (gun->trigger_pwm_monitor) {
            pwm_monitor_stop(gun->trigger_pwm_monitor);
            pwm_monitor_destroy(gun->trigger_pwm_monitor);
        }
        if (gun->smoke_heater_toggle_monitor) {
            pwm_monitor_stop(gun->smoke_heater_toggle_monitor);
            pwm_monitor_destroy(gun->smoke_heater_toggle_monitor);
        }
        if (gun->serial_bus) serial_bus_close(gun->serial_bus);
        free(gun);
        return nullptr;
    }
    
    LOG_INIT(LOG_GUN, "Gun FX system ready");
    return gun;
}

void gun_fx_destroy(GunFX *gun) {
    if (!gun) return;
    
    // Stop processing thread
    if (atomic_load(&gun->processing_running)) {
        atomic_store(&gun->processing_running, false);
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

    if (gun->serial_bus) {
        send_shutdown(gun);
        usleep(50000); // 50ms delay to allow Pico to process shutdown
        serial_bus_close(gun->serial_bus);
    }
    
    // Destroy components
    
    // Free rates array
    if (gun->rates) free(gun->rates);
    
    free(gun);
    
    LOG_SHUTDOWN(LOG_GUN, "Gun FX system");
}

int gun_fx_set_rates_of_fire(GunFX *gun, const RateOfFire *rates, int count) {
    if (!gun || !rates || count <= 0) return -1;
    
    // Free existing rates
    if (gun->rates) {
        free(gun->rates);
    }
    
    // Allocate and copy new rates
    gun->rates = (RateOfFire *)malloc(sizeof(RateOfFire) * count);
    if (!gun->rates) {
        LOG_ERROR(LOG_GUN, "Cannot allocate memory for rates array (%d entries)", count);
        gun->rate_count = 0;
        return -1;
    }
    
    memcpy(gun->rates, rates, sizeof(RateOfFire) * count);
    gun->rate_count = count;
    
    LOG_INFO(LOG_GUN, "Configured %d rate(s) of fire", count);
    LOG_DEBUG(LOG_GUN, "Rates: %s", count > 0 ? "updated" : "empty");
    return 0;
}

int gun_fx_get_current_rpm(GunFX *gun) {
    if (!gun) return 0;
    
    return atomic_load(&gun->current_rpm);
}

int gun_fx_get_current_rate_index(GunFX *gun) {
    if (!gun) return -1;
    
    return atomic_load(&gun->current_rate_index);
}

bool gun_fx_is_firing(GunFX *gun) {
    if (!gun) return false;
    
    return atomic_load(&gun->is_firing);
}

// Getter functions for status display
int gun_fx_get_trigger_pwm(GunFX *gun) {
    if (!gun || !gun->trigger_pwm_monitor) return -1;
    int avg;
    return pwm_monitor_get_average(gun->trigger_pwm_monitor, &avg) ? avg : -1;
}

int gun_fx_get_trigger_pin(GunFX *gun) {
    return gun ? gun->trigger_pwm_pin : -1;
}

int gun_fx_get_heater_toggle_pwm(GunFX *gun) {
    if (!gun || !gun->smoke_heater_toggle_monitor) return -1;
    int avg;
    return pwm_monitor_get_average(gun->smoke_heater_toggle_monitor, &avg) ? avg : -1;
}

int gun_fx_get_heater_toggle_pin(GunFX *gun) {
    return gun ? gun->smoke_heater_toggle_pin : -1;
}

bool gun_fx_get_heater_state(GunFX *gun) {
    return gun ? atomic_load(&gun->smoke_heater_on) : false;
}

int gun_fx_get_pitch_pwm(GunFX *gun) {
    if (!gun || !gun->pitch_pwm_monitor) return -1;
    int avg;
    return pwm_monitor_get_average(gun->pitch_pwm_monitor, &avg) ? avg : -1;
}

int gun_fx_get_pitch_pin(GunFX *gun) {
    return gun ? gun->pitch_pwm_pin : -1;
}

int gun_fx_get_yaw_pwm(GunFX *gun) {
    if (!gun || !gun->yaw_pwm_monitor) return -1;
    int avg;
    return pwm_monitor_get_average(gun->yaw_pwm_monitor, &avg) ? avg : -1;
}

int gun_fx_get_yaw_pin(GunFX *gun) {
    return gun ? gun->yaw_pwm_pin : -1;
}

// Recoil jerk getters
int gun_fx_get_pitch_recoil_jerk(GunFX *gun) {
    return gun ? gun->pitch_cfg.recoil_jerk_us : 0;
}

int gun_fx_get_pitch_recoil_jerk_variance(GunFX *gun) {
    return gun ? gun->pitch_cfg.recoil_jerk_variance_us : 0;
}

int gun_fx_get_yaw_recoil_jerk(GunFX *gun) {
    return gun ? gun->yaw_cfg.recoil_jerk_us : 0;
}

int gun_fx_get_yaw_recoil_jerk_variance(GunFX *gun) {
    return gun ? gun->yaw_cfg.recoil_jerk_variance_us : 0;
}
