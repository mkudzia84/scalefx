/**
 * Helicopter FX Main Application
 * 
 * Integrated engine and gun effects for KA-50 helicopter simulation
 * Reads configuration from YAML file and manages all effects
 * 
 * Usage: helifx <config.yaml>
 */

#include "engine_fx.h"
#include "gun_fx.h"
#include "audio_player.h"
#include "gpio.h"
#include "config_loader.h"
#include "logging.h"
#ifdef ENABLE_JETIEX
#include "helifx_jetiex.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static volatile bool running = true;

void signal_handler(int signum) {
    (void)signum;
    printf("\n[HELIFX] Shutting down...\n");
    running = false;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
        return 1;
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Parse configuration
    HeliFXConfig config;
    if (config_load(argv[1], &config) != 0) {
        fprintf(stderr, "[HELIFX] Failed to load configuration file\n");
        return 1;
    }
    
    // Validate configuration
    if (config_validate(&config) != 0) {
        fprintf(stderr, "[HELIFX] Configuration validation failed\n");
        return 1;
    }
    
    // Print configuration
    config_print(&config);
    
    // Initialize GPIO
    if (gpio_init() < 0) {
        fprintf(stderr, "[HELIFX] Error: Failed to initialize GPIO\n");
        fprintf(stderr, "[HELIFX] Note: Try running with sudo for GPIO access\n");
        return 1;
    }
    
    // Create audio mixer (8 channels)
    AudioMixer *mixer = audio_mixer_create(8);
    if (!mixer) {
        fprintf(stderr, "[HELIFX] Error: Failed to create audio mixer\n");
        gpio_cleanup();
        return 1;
    }
    
    // Initialize Engine FX
    EngineFX *engine = NULL;
    Sound *engine_starting = NULL;
    Sound *engine_running = NULL;
    Sound *engine_stopping = NULL;
    
    if (config.engine.enabled) {
        printf("[HELIFX] Initializing Engine FX...\n");
        
        // Load engine sounds
        if (config.engine.starting_file[0]) {
            engine_starting = sound_load(config.engine.starting_file);
            if (!engine_starting) {
                fprintf(stderr, "[HELIFX] Warning: Failed to load engine starting sound\n");
            }
        }
        
        if (config.engine.running_file[0]) {
            engine_running = sound_load(config.engine.running_file);
            if (!engine_running) {
                fprintf(stderr, "[HELIFX] Warning: Failed to load engine running sound\n");
            }
        }
        
        if (config.engine.stopping_file[0]) {
            engine_stopping = sound_load(config.engine.stopping_file);
            if (!engine_stopping) {
                fprintf(stderr, "[HELIFX] Warning: Failed to load engine stopping sound\n");
            }
        }
        
        // Create engine FX controller (audio channel 0)
        engine = engine_fx_create(mixer, 0, &config.engine);
        if (!engine) {
            fprintf(stderr, "[HELIFX] Error: Failed to create engine FX controller\n");
        } else {
            engine_fx_load_sounds(engine, engine_starting, engine_running, engine_stopping);
            printf("[HELIFX] Engine FX initialized\n");
        }
    }
    
    // Initialize Gun FX
    GunFX *gun = NULL;
    Sound **gun_sounds = NULL;
    
    if (config.gun.enabled) {
        printf("[HELIFX] Initializing Gun FX...\n");
        
        // Allocate array for gun sounds
        gun_sounds = calloc(config.gun.rate_count, sizeof(Sound*));
        if (!gun_sounds) {
            fprintf(stderr, "[HELIFX] Error: Failed to allocate gun sounds array\n");
            if (engine) engine_fx_destroy(engine);
            if (engine_starting) sound_destroy(engine_starting);
            if (engine_running) sound_destroy(engine_running);
            if (engine_stopping) sound_destroy(engine_stopping);
            audio_mixer_destroy(mixer);
            gpio_cleanup();
            config_free(&config);
            return 1;
        }
        
        // Load gun sounds
        for (int i = 0; i < config.gun.rate_count; i++) {
            if (config.gun.rates[i].sound_file[0]) {
                gun_sounds[i] = sound_load(config.gun.rates[i].sound_file);
                if (!gun_sounds[i]) {
                    fprintf(stderr, "[HELIFX] Warning: Failed to load gun sound for rate %d\n", i + 1);
                }
            }
        }
        
        // Create gun FX controller (audio channel 1)
        gun = gun_fx_create(mixer, 1, &config.gun);
        
        if (!gun) {
            fprintf(stderr, "[HELIFX] Error: Failed to create gun FX controller\n");
        } else {
            // Set rates of fire
            RateOfFire *rates = malloc(config.gun.rate_count * sizeof(RateOfFire));
            if (!rates) {
                fprintf(stderr, "[HELIFX] Error: Failed to allocate rates array\n");
                gun_fx_destroy(gun);
                gun = NULL;
            } else {
                for (int i = 0; i < config.gun.rate_count; i++) {
                    rates[i].rounds_per_minute = config.gun.rates[i].rpm;
                    rates[i].pwm_threshold_us = config.gun.rates[i].pwm_threshold_us;
                    rates[i].sound = gun_sounds[i];
                }
                
                if (gun_fx_set_rates_of_fire(gun, rates, config.gun.rate_count) != 0) {
                    fprintf(stderr, "[HELIFX] Error: Failed to set rates of fire\n");
                }
                
                free(rates);
                
                // Set smoke fan off delay
                gun_fx_set_smoke_fan_off_delay(gun, config.gun.smoke_fan_off_delay_ms);
                
                printf("[HELIFX] Gun FX initialized with %d rates\n", config.gun.rate_count);
            }
        }
    }
    
#ifdef ENABLE_JETIEX
    // Initialize JetiEX telemetry
    JetiEX *jetiex = helifx_jetiex_init(&config, argv[1], gun, engine);
#endif
    
    printf("\n[HELIFX] System ready. Press Ctrl+C to exit.\n\n");
    
    // Main loop - update telemetry
    while (running) {
#ifdef ENABLE_JETIEX
        helifx_jetiex_update(jetiex, gun, engine);
#endif
        sleep(1);
    }
    
    // Cleanup
    printf("[HELIFX] Cleaning up...\n");
    
#ifdef ENABLE_JETIEX
    helifx_jetiex_cleanup(jetiex);
#endif
    
    if (engine) {
        engine_fx_destroy(engine);
    }
    if (gun) {
        gun_fx_destroy(gun);
    }
    
    // Free sounds
    if (engine_starting) sound_destroy(engine_starting);
    if (engine_running) sound_destroy(engine_running);
    if (engine_stopping) sound_destroy(engine_stopping);
    
    if (gun_sounds) {
        for (int i = 0; i < config.gun.rate_count; i++) {
            if (gun_sounds[i]) sound_destroy(gun_sounds[i]);
        }
        free(gun_sounds);
    }
    
    config_free(&config);
    audio_mixer_destroy(mixer);
    gpio_cleanup();
    
    printf("[HELIFX] Shutdown complete.\n");
    return 0;
}
