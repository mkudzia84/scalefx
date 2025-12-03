/**
 * Gun FX Demo
 * 
 * Demonstrates helicopter gun effects with multiple rates of fire,
 * nozzle flash LED, and smoke generator integration.
 * 
 * Usage: 
 *   gun_fx_demo [options]
 * 
 * Options:
 *   --trigger-pin=N         GPIO pin for trigger PWM input (default: 27)
 *   --heater-toggle-pin=N   GPIO pin for heater toggle PWM (default: 22)
 *   --led-pin=N             GPIO pin for nozzle flash LED (default: 23)
 *   --smoke-fan-pin=N       GPIO pin for smoke fan (default: 24)
 *   --smoke-heater-pin=N    GPIO pin for smoke heater (default: 25)
 *   --smoke-fan-off-delay=N Smoke fan off delay in ms (default: 2000)
 *   --audio-channel=N       Audio mixer channel (default: 0)
 *   --rate1-rpm=N           Rate 1 rounds per minute (default: 600)
 *   --rate1-pwm=N           Rate 1 PWM threshold µs (default: 1400)
 *   --rate1-sound=FILE      Rate 1 sound file (default: sounds/cannon_slow.wav)
 *   --rate2-rpm=N           Rate 2 rounds per minute (default: 900)
 *   --rate2-pwm=N           Rate 2 PWM threshold µs (default: 1600)
 *   --rate2-sound=FILE      Rate 2 sound file (default: sounds/cannon_fast.wav)
 *   --help                  Show this help message
 * 
 * Examples:
 *   gun_fx_demo
 *   gun_fx_demo --trigger-pin=5 --led-pin=6
 *   gun_fx_demo --rate1-rpm=450 --rate1-pwm=1300
 * 
 * NOTES:
 *   - 100µs hysteresis prevents rapid rate switching
 *   - LED blinks at firing rate (synchronized with shots)
 *   - Sound loops continuously while firing
 *   - Smoke fan runs during firing, heater independent
 */

#include "gun_fx.h"
#include "audio_player.h"
#include "gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

// Configuration structure
typedef struct {
    int trigger_pin;
    int heater_toggle_pin;
    int led_pin;
    int smoke_fan_pin;
    int smoke_heater_pin;
    int audio_channel;
    int smoke_fan_off_delay_ms;
    
    struct {
        int rpm;
        int pwm_threshold;
        char sound_file[256];
    } rates[3];
    int rate_count;
} Config;

static volatile bool running = true;

void signal_handler(int signum) {
    (void)signum;  // Unused parameter
    printf("\n[DEMO] Shutting down...\n");
    running = false;
}

// Load default configuration
void load_default_config(Config *config) {
    config->trigger_pin = 27;
    config->heater_toggle_pin = 22;
    config->led_pin = 23;
    config->smoke_fan_pin = 24;
    config->smoke_heater_pin = 25;
    config->audio_channel = 0;
    config->smoke_fan_off_delay_ms = 2000;  // 2 seconds default
    
    config->rate_count = 2;
    
    // Rate 1: Slow
    config->rates[0].rpm = 600;
    config->rates[0].pwm_threshold = 1400;
    strncpy(config->rates[0].sound_file, "sounds/cannon_slow.wav", sizeof(config->rates[0].sound_file) - 1);
    
    // Rate 2: Fast
    config->rates[1].rpm = 900;
    config->rates[1].pwm_threshold = 1600;
    strncpy(config->rates[1].sound_file, "sounds/cannon_fast.wav", sizeof(config->rates[1].sound_file) - 1);
}

void print_help(const char *progname) {
    printf("Usage: %s [options]\n\n", progname);
    printf("Options:\n");
    printf("  --trigger-pin=N         GPIO pin for trigger PWM input (default: 27)\n");
    printf("  --heater-toggle-pin=N   GPIO pin for heater toggle PWM (default: 22)\n");
    printf("  --led-pin=N             GPIO pin for nozzle flash LED (default: 23)\n");
    printf("  --smoke-fan-pin=N       GPIO pin for smoke fan (default: 24)\n");
    printf("  --smoke-heater-pin=N    GPIO pin for smoke heater (default: 25)\n");
    printf("  --smoke-fan-off-delay=N Smoke fan off delay in ms (default: 2000)\n");
    printf("  --audio-channel=N       Audio mixer channel (default: 0)\n");
    printf("  --rate1-rpm=N           Rate 1 rounds per minute (default: 600)\n");
    printf("  --rate1-pwm=N           Rate 1 PWM threshold µs (default: 1400)\n");
    printf("  --rate1-sound=FILE      Rate 1 sound file (default: sounds/cannon_slow.wav)\n");
    printf("  --rate2-rpm=N           Rate 2 rounds per minute (default: 900)\n");
    printf("  --rate2-pwm=N           Rate 2 PWM threshold µs (default: 1600)\n");
    printf("  --rate2-sound=FILE      Rate 2 sound file (default: sounds/cannon_fast.wav)\n");
    printf("  --help                  Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s\n", progname);
    printf("  %s --trigger-pin=5 --led-pin=6\n", progname);
    printf("  %s --rate1-rpm=450 --rate1-pwm=1300\n", progname);
}

// Parse command line arguments
int parse_args(int argc, char *argv[], Config *config) {
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_help(argv[0]);
            return -1;
        }
        
        // Parse --key=value format
        if (strncmp(arg, "--", 2) != 0) {
            fprintf(stderr, "Error: Invalid argument '%s'\n", arg);
            fprintf(stderr, "Use --help for usage information\n");
            return -1;
        }
        
        char *equals = strchr(arg, '=');
        if (!equals) {
            fprintf(stderr, "Error: Invalid argument format '%s' (expected --key=value)\n", arg);
            return -1;
        }
        
        *equals = '\0';
        char *key = arg + 2;  // Skip --
        char *value = equals + 1;
        
        // Parse settings
        if (strcmp(key, "trigger-pin") == 0) {
            config->trigger_pin = atoi(value);
        } else if (strcmp(key, "heater-toggle-pin") == 0) {
            config->heater_toggle_pin = atoi(value);
        } else if (strcmp(key, "led-pin") == 0) {
            config->led_pin = atoi(value);
        } else if (strcmp(key, "smoke-fan-pin") == 0) {
            config->smoke_fan_pin = atoi(value);
        } else if (strcmp(key, "smoke-heater-pin") == 0) {
            config->smoke_heater_pin = atoi(value);
        } else if (strcmp(key, "smoke-fan-off-delay") == 0) {
            config->smoke_fan_off_delay_ms = atoi(value);
        } else if (strcmp(key, "audio-channel") == 0) {
            config->audio_channel = atoi(value);
        } else if (strncmp(key, "rate", 4) == 0) {
            int rate_num = atoi(key + 4);
            if (rate_num >= 1 && rate_num <= 2) {
                int idx = rate_num - 1;
                
                if (strstr(key, "-rpm")) {
                    config->rates[idx].rpm = atoi(value);
                } else if (strstr(key, "-pwm")) {
                    config->rates[idx].pwm_threshold = atoi(value);
                } else if (strstr(key, "-sound")) {
                    strncpy(config->rates[idx].sound_file, value, 
                           sizeof(config->rates[idx].sound_file) - 1);
                } else {
                    fprintf(stderr, "Error: Unknown rate option '%s'\n", key);
                    return -1;
                }
            } else {
                fprintf(stderr, "Error: Invalid rate number %d (must be 1-2)\n", rate_num);
                return -1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", key);
            fprintf(stderr, "Use --help for usage information\n");
            return -1;
        }
    }
    
    return 0;
}

// Parse config file
int load_config_file(const char *filename, Config *config) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "[DEMO] Error: Could not open config file '%s'\n", filename);
        return -1;
    }
    
    char line[512];
    int rate_index = -1;
    
    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\r\n")] = 0;
        
        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        
        // Parse key=value
        char *equals = strchr(line, '=');
        if (!equals) {
            continue;
        }
        
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        
        // Trim whitespace
        while (*key == ' ' || *key == '\t') key++;
        while (*value == ' ' || *value == '\t') value++;
        
        // Parse settings
        if (strcmp(key, "trigger_pin") == 0) {
            config->trigger_pin = atoi(value);
        } else if (strcmp(key, "heater_toggle_pin") == 0) {
            config->heater_toggle_pin = atoi(value);
        } else if (strcmp(key, "led_pin") == 0) {
            config->led_pin = atoi(value);
        } else if (strcmp(key, "smoke_fan_pin") == 0) {
            config->smoke_fan_pin = atoi(value);
        } else if (strcmp(key, "smoke_heater_pin") == 0) {
            config->smoke_heater_pin = atoi(value);
        } else if (strcmp(key, "audio_channel") == 0) {
            config->audio_channel = atoi(value);
        } else if (strncmp(key, "rate", 4) == 0) {
            // Parse rate number (rate1_rpm, rate2_pwm, etc.)
            int rate_num = atoi(key + 4);
            if (rate_num >= 1 && rate_num <= 3) {
                rate_index = rate_num - 1;
                
                if (strstr(key, "_rpm")) {
                    config->rates[rate_index].rpm = atoi(value);
                } else if (strstr(key, "_pwm")) {
                    config->rates[rate_index].pwm_threshold = atoi(value);
                } else if (strstr(key, "_sound")) {
                    strncpy(config->rates[rate_index].sound_file, value, 
                           sizeof(config->rates[rate_index].sound_file) - 1);
                }
            }
        }
    }
    
    fclose(file);
    return 0;
}

void print_config(const Config *config) {
    printf("Configuration:\n");
    printf("  GPIO Pins:\n");
    printf("    Trigger PWM:       GPIO %d\n", config->trigger_pin);
    printf("    Heater toggle:     GPIO %d\n", config->heater_toggle_pin);
    printf("    Nozzle flash LED:  GPIO %d\n", config->led_pin);
    printf("    Smoke fan:         GPIO %d\n", config->smoke_fan_pin);
    printf("    Smoke heater:      GPIO %d\n", config->smoke_heater_pin);
    printf("  Audio:\n");
    printf("    Channel:           %d\n", config->audio_channel);
    printf("  Rates of Fire:\n");
    for (int i = 0; i < config->rate_count; i++) {
        printf("    Rate %d: %d RPM @ %dµs PWM - %s\n", 
               i + 1,
               config->rates[i].rpm,
               config->rates[i].pwm_threshold,
               config->rates[i].sound_file);
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    Config config;
    
    // Load default configuration
    load_default_config(&config);
    
    // Parse command line arguments
    if (parse_args(argc, argv, &config) != 0) {
        return 1;
    }
    
    printf("=== Gun FX Demo ===\n\n");
    print_config(&config);
    printf("Press Ctrl+C to exit\n\n");
    
    signal(SIGINT, signal_handler);
    
    // Initialize GPIO subsystem
    if (gpio_init() < 0) {
        fprintf(stderr, "[DEMO] Failed to initialize GPIO subsystem\n");
        return 1;
    }
    
    // Initialize audio system
    AudioMixer *mixer = audio_mixer_create(4);  // 4 channels
    if (!mixer) {
        fprintf(stderr, "[DEMO] Failed to create audio mixer\n");
        gpio_cleanup();
        return 1;
    }
    
    // Load gun sounds
    Sound *sounds[2] = {NULL, NULL};
    for (int i = 0; i < config.rate_count; i++) {
        sounds[i] = sound_load(config.rates[i].sound_file);
        if (!sounds[i]) {
            fprintf(stderr, "[DEMO] Warning: Failed to load sound '%s'\n", 
                   config.rates[i].sound_file);
        }
    }
    
    // Create gun FX controller
    GunFX *gun = gun_fx_create(mixer, config.audio_channel, 
                                config.trigger_pin, 
                                config.heater_toggle_pin,
                                config.led_pin, 
                                config.smoke_fan_pin, 
                                config.smoke_heater_pin);
    if (!gun) {
        fprintf(stderr, "[DEMO] Failed to create gun FX\n");
        audio_mixer_destroy(mixer);
        return 1;
    }
    
    // Configure rates of fire (sorted by PWM threshold, highest first)
    RateOfFire rates[2];
    for (int i = 0; i < config.rate_count; i++) {
        rates[i].rounds_per_minute = config.rates[i].rpm;
        rates[i].pwm_threshold_us = config.rates[i].pwm_threshold;
        rates[i].sound = sounds[i];
    }
    
    if (gun_fx_set_rates_of_fire(gun, rates, config.rate_count) != 0) {
        fprintf(stderr, "[DEMO] Failed to set rates of fire\n");
        gun_fx_destroy(gun);
        audio_mixer_destroy(mixer);
        return 1;
    }
    
    // Set smoke fan off delay
    gun_fx_set_smoke_fan_off_delay(gun, config.smoke_fan_off_delay_ms);
    
    printf("[DEMO] Gun FX initialized with %d rates of fire\n", config.rate_count);
    printf("[DEMO] Monitoring PWM inputs...\n\n");
    
    // Main loop - monitor status
    int last_rpm = 0;
    while (running) {
        int current_rpm = gun_fx_get_current_rpm(gun);
        
        // Print status when rate changes
        if (current_rpm != last_rpm) {
            if (current_rpm > 0) {
                printf("[DEMO] Firing: %d RPM\n", current_rpm);
            } else {
                printf("[DEMO] Ceased fire\n");
            }
            last_rpm = current_rpm;
        }
        
        sleep(1);
    }
    
    // Cleanup
    printf("\n[DEMO] Cleaning up...\n");
    gun_fx_destroy(gun);
    
    for (int i = 0; i < config.rate_count; i++) {
        if (sounds[i]) {
            sound_destroy(sounds[i]);
        }
    }
    
    audio_mixer_destroy(mixer);
    gpio_cleanup();
    
    printf("[DEMO] Demo complete\n");
    return 0;
}
