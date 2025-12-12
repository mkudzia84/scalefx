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
#include "status.h"
#ifdef ENABLE_JETIEX
#include "helifx_jetiex.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile bool running = true;

void signal_handler(int signum) {
    (void)signum;
    printf("\n[HELIFX] Shutting down...\n");
    running = false;
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s [--interactive] <config.yaml>\n", argv[0]);
        fprintf(stderr, "  --interactive  Enable interactive status display (stdout) with file logging\n");
        fprintf(stderr, "                 Without this flag, logging goes to console only\n");
        return 1;
    }
    
    // Parse command line arguments
    bool interactive_mode = false;
    const char *config_file = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interactive") == 0) {
            interactive_mode = true;
        } else {
            config_file = argv[i];
        }
    }
    
    if (!config_file) {
        fprintf(stderr, "Error: Configuration file not specified\n");
        fprintf(stderr, "Usage: %s [--interactive] <config.yaml>\n", argv[0]);
        return 1;
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize logging system based on mode
    if (interactive_mode) {
        // Interactive mode: log to file, status display on stdout
        if (logging_init("/var/log/helifx.log", 10, 5) != 0) {
            fprintf(stderr, "[HELIFX] Warning: Failed to initialize file logging\n");
            fprintf(stderr, "[HELIFX] Falling back to console logging\n");
            logging_init(NULL, 0, 0);
            interactive_mode = false;  // Disable interactive mode if file logging fails
        } else {
            fprintf(stderr, "[HELIFX] Interactive mode: Status display on stdout, logging to /var/log/helifx.log\n");
        }
    } else {
        // Non-interactive mode: log to console only
        logging_init(NULL, 0, 0);
        fprintf(stderr, "[HELIFX] Console mode: Logging to stdout/stderr\n");
    }
    
    LOG_INFO(LOG_HELIFX, "Starting HeliFX system...");
    
    // Parse configuration
    HeliFXConfig *config = config_load(config_file);
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
        LOG_INFO(LOG_HELIFX, "Initializing Engine FX...");
        
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
            LOG_INFO(LOG_HELIFX, "Engine FX initialized");
        }
    }
    
    // Initialize Gun FX
    GunFX *gun = nullptr;
    
    if (config->gun.enabled) {
        LOG_INFO(LOG_HELIFX, "Initializing Gun FX...");
        
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
                
                LOG_INFO(LOG_HELIFX, "Gun FX initialized with %d rates", config->gun.rate_count);
            }
        }
    }
    
#ifdef ENABLE_JETIEX
    // Initialize JetiEX telemetry
    JetiEX *jetiex = helifx_jetiex_init(config, config_file, gun, engine);
#endif
    
    // Create status display if in interactive mode
    StatusDisplay *status = NULL;
    if (interactive_mode) {
        status = status_display_create(gun, engine, 100);  // 100ms refresh
        if (!status) {
            fprintf(stderr, "[HELIFX] Warning: Failed to create status display\n");
        }
    }
    
    LOG_INFO(LOG_HELIFX, "System ready. Press Ctrl+C to exit.");
    if (!interactive_mode) {
        printf("\n[HELIFX] System ready. Press Ctrl+C to exit.\n\n");
    }
    
    // Main loop - update telemetry
    while (running) {
#ifdef ENABLE_JETIEX
        helifx_jetiex_update(jetiex, gun, engine);
#endif
        sleep(1);
    }
    
    // Cleanup
    LOG_INFO(LOG_HELIFX, "Cleaning up...");
    if (!interactive_mode) {
        printf("[HELIFX] Cleaning up...\n");
    }
    
    // Stop all threads first
    if (status) {
        status_display_destroy(status);
        LOG_INFO(LOG_HELIFX, "Status display stopped");
    }
    
#ifdef ENABLE_JETIEX
    helifx_jetiex_cleanup(jetiex);
    LOG_INFO(LOG_HELIFX, "JetiEX telemetry stopped");
#endif
    
    if (engine) {
        engine_fx_destroy(engine);
        LOG_INFO(LOG_HELIFX, "Engine FX thread stopped");
    }
    if (gun) {
        gun_fx_destroy(gun);
        LOG_INFO(LOG_HELIFX, "Gun FX thread stopped");
    }
    
    // Cleanup resources
    sound_manager_destroy(sound_mgr);
    config_free(config);
    audio_mixer_destroy(mixer);
    gpio_cleanup();
    
    LOG_INFO(LOG_HELIFX, "Shutdown complete");
    logging_shutdown();
    
    printf("[HELIFX] Shutdown complete.\n");
    return 0;
}
