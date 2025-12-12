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
    HeliFXConfig *config = config_load(argv[1]);
    if (!config) {
        fprintf(stderr, "[HELIFX] Failed to load configuration file\n");
        return 1;
    }
    
    // Validate configuration
    if (config_validate(config) != 0) {
        fprintf(stderr, "[HELIFX] Configuration validation failed\n");
        config_free(config);
        return 1;
    }
    
    // Print configuration
    config_print(config);
    
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
    
    // Create sound manager
    SoundManager *sound_mgr = sound_manager_create();
    if (!sound_mgr) {
        fprintf(stderr, "[HELIFX] Error: Failed to create sound manager\n");
        audio_mixer_destroy(mixer);
        gpio_cleanup();
        return 1;
    }
    
    // Initialize Engine FX
    EngineFX *engine = nullptr;
    
    if (config->engine.enabled) {
        printf("[HELIFX] Initializing Engine FX...\n");
        
        // Load engine sounds
        sound_manager_load_sound(sound_mgr, SOUND_ENGINE_STARTING, config->engine.sounds.starting);
        sound_manager_load_sound(sound_mgr, SOUND_ENGINE_RUNNING, config->engine.sounds.running);
        sound_manager_load_sound(sound_mgr, SOUND_ENGINE_STOPPING, config->engine.sounds.stopping);
        
        // Create engine FX controller (audio channel 0)
        engine = engine_fx_create(mixer, 0, &config->engine);
        if (!engine) {
            fprintf(stderr, "[HELIFX] Error: Failed to create engine FX controller\n");
        } else {
            engine_fx_load_sounds(engine, 
                sound_manager_get_sound(sound_mgr, SOUND_ENGINE_STARTING),
                sound_manager_get_sound(sound_mgr, SOUND_ENGINE_RUNNING),
                sound_manager_get_sound(sound_mgr, SOUND_ENGINE_STOPPING));
            printf("[HELIFX] Engine FX initialized\n");
        }
    }
    
    // Initialize Gun FX
    GunFX *gun = nullptr;
    
    if (config->gun.enabled) {
        printf("[HELIFX] Initializing Gun FX...\n");
        
        // Load gun sounds (up to 10 rates supported)
        for (int i = 0; i < config->gun.rate_count && i < 10; i++) {
            sound_manager_load_sound(sound_mgr, SOUND_GUN_RATE_1 + i, 
                config->gun.rates[i].sound_file);
        }
        
        // Create gun FX controller (audio channel 1)
        gun = gun_fx_create(mixer, 1, &config->gun);
        
        if (!gun) {
            fprintf(stderr, "[HELIFX] Error: Failed to create gun FX controller\n");
        } else {
            // Set rates of fire
            RateOfFire *rates = (RateOfFire*)malloc(config->gun.rate_count * sizeof(RateOfFire));
            if (rates == nullptr) {
                fprintf(stderr, "[HELIFX] Error: Failed to allocate rates array\n");
                gun_fx_destroy(gun);
                gun = nullptr;
            } else {
                for (int i = 0; i < config->gun.rate_count; i++) {
                    rates[i].rounds_per_minute = config->gun.rates[i].rpm;
                    rates[i].pwm_threshold_us = config->gun.rates[i].pwm_threshold_us;
                    rates[i].sound = (i < 10) ? sound_manager_get_sound(sound_mgr, SOUND_GUN_RATE_1 + i) : nullptr;
                }
                
                if (gun_fx_set_rates_of_fire(gun, rates, config->gun.rate_count) != 0) {
                    fprintf(stderr, "[HELIFX] Error: Failed to set rates of fire\n");
                }
                
                free(rates);
                
                // Set smoke fan off delay
                gun_fx_set_smoke_fan_off_delay(gun, config->gun.smoke.fan_off_delay_ms);
                
                printf("[HELIFX] Gun FX initialized with %d rates\n", config->gun.rate_count);
            }
        }
    }
    
#ifdef ENABLE_JETIEX
    // Initialize JetiEX telemetry
    JetiEX *jetiex = helifx_jetiex_init(config, argv[1], gun, engine);
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
    
    // Stop all threads first
#ifdef ENABLE_JETIEX
    helifx_jetiex_cleanup(jetiex);
    printf("[HELIFX] JetiEX telemetry stopped\n");
#endif
    
    if (engine) {
        engine_fx_destroy(engine);
        printf("[HELIFX] Engine FX thread stopped\n");
    }
    if (gun) {
        gun_fx_destroy(gun);
        printf("[HELIFX] Gun FX thread stopped\n");
    }
    
    // Cleanup resources
    sound_manager_destroy(sound_mgr);
    config_free(config);
    audio_mixer_destroy(mixer);
    gpio_cleanup();
    
    printf("[HELIFX] Shutdown complete.\n");
    return 0;
}
