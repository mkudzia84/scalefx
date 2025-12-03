#include "engine_fx.h"
#include "audio_player.h"
#include "gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

static volatile bool running = true;

void signal_handler(int sig) {
    (void)sig;
    running = false;
    printf("\nShutting down...\n");
}

void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --pwm-pin <pin>              GPIO pin for PWM input (default: 17)\n");
    printf("  --threshold <us>             PWM threshold in microseconds (default: 1500)\n");
    printf("  --channel <n>                Audio channel to use (default: 0)\n");
    printf("  --starting <file>            Starting sound file (optional)\n");
    printf("  --running <file>             Running sound file (optional)\n");
    printf("  --stopping <file>            Stopping sound file (optional)\n");
    printf("  --starting-offset <ms>       Offset in ms to play starting track from when restarting from stopping (default: 0)\n");
    printf("  --stopping-offset <ms>       Offset in ms to play stopping track from when stopping from starting (default: 0)\n");
    printf("  --help                       Show this help message\n");
}

int main(int argc, char *argv[]) {
    int pwm_pin = 17;
    int threshold = 1500;
    int audio_channel = 0;
    int starting_offset_ms = 0;
    int stopping_offset_ms = 0;
    char *starting_file = NULL;
    char *running_file = NULL;
    char *stopping_file = NULL;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pwm-pin") == 0 && i + 1 < argc) {
            pwm_pin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            audio_channel = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--starting-offset") == 0 && i + 1 < argc) {
            starting_offset_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stopping-offset") == 0 && i + 1 < argc) {
            stopping_offset_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--starting") == 0 && i + 1 < argc) {
            starting_file = argv[++i];
        } else if (strcmp(argv[i], "--running") == 0 && i + 1 < argc) {
            running_file = argv[++i];
        } else if (strcmp(argv[i], "--stopping") == 0 && i + 1 < argc) {
            stopping_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    
    // Initialize GPIO
    if (gpio_init() < 0) {
        fprintf(stderr, "Error: Failed to initialize GPIO\n");
        fprintf(stderr, "Note: Try running with sudo for GPIO access\n");
        return 1;
    }
    
    // Create audio mixer
    AudioMixer *mixer = audio_mixer_create(8);
    if (!mixer) {
        fprintf(stderr, "Error: Failed to create audio mixer\n");
        gpio_cleanup();
        return 1;
    }
    
    printf("Engine FX Demo\n");
    printf("==============\n");
    printf("PWM Pin: %d\n", pwm_pin);
    printf("PWM Threshold: %d us\n", threshold);
    printf("Audio Channel: %d\n", audio_channel);
    printf("Starting Offset: %d ms\n", starting_offset_ms);
    printf("Stopping Offset: %d ms\n", stopping_offset_ms);
    printf("\n");
    
    // Create engine FX controller
    EngineFX *engine = engine_fx_create(mixer, audio_channel, pwm_pin, threshold, starting_offset_ms, stopping_offset_ms);
    if (!engine) {
        fprintf(stderr, "Error: Failed to create engine FX controller\n");
        audio_mixer_destroy(mixer);
        gpio_cleanup();
        return 1;
    }
    
    // Load sound files if provided
    Sound *sound_starting = NULL;
    Sound *sound_running = NULL;
    Sound *sound_stopping = NULL;
    
    if (starting_file) {
        sound_starting = sound_load(starting_file);
        if (!sound_starting) {
            fprintf(stderr, "Warning: Failed to load starting sound: %s\n", starting_file);
        } else {
            printf("Loaded starting sound: %s\n", starting_file);
        }
    }
    
    if (running_file) {
        sound_running = sound_load(running_file);
        if (!sound_running) {
            fprintf(stderr, "Warning: Failed to load running sound: %s\n", running_file);
        } else {
            printf("Loaded running sound: %s\n", running_file);
        }
    }
    
    if (stopping_file) {
        sound_stopping = sound_load(stopping_file);
        if (!sound_stopping) {
            fprintf(stderr, "Warning: Failed to load stopping sound: %s\n", stopping_file);
        } else {
            printf("Loaded stopping sound: %s\n", stopping_file);
        }
    }
    
    // Load sounds into engine
    engine_fx_load_sounds(engine, sound_starting, sound_running, sound_stopping);
    
    printf("\nEngine FX is running. Press Ctrl+C to exit.\n");
    printf("Monitoring PWM signal on pin %d...\n\n", pwm_pin);
    
    EngineState last_state = ENGINE_STOPPED;
    
    // Main loop - monitor and display state changes
    while (running) {
        EngineState current_state = engine_fx_get_state(engine);
        
        if (current_state != last_state) {
            printf("[STATE CHANGE] %s -> %s\n", 
                   engine_fx_state_to_string(last_state),
                   engine_fx_state_to_string(current_state));
            last_state = current_state;
        }
        
        usleep(100000); // 100ms
    }
    
    printf("\nCleaning up...\n");
    
    // Cleanup
    engine_fx_destroy(engine);
    
    if (sound_starting) sound_destroy(sound_starting);
    if (sound_running) sound_destroy(sound_running);
    if (sound_stopping) sound_destroy(sound_stopping);
    
    audio_mixer_destroy(mixer);
    gpio_cleanup();
    
    printf("Engine FX demo exited\n");
    return 0;
}
