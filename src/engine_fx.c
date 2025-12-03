#include "engine_fx.h"
#include "gpio.h"
#include "audio_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

struct EngineFX {
    EngineState state;
    pthread_mutex_t mutex;
    
    // PWM monitoring for engine toggle
    PWMMonitor *engine_toggle_pwm_monitor;
    int engine_toggle_pwm_pin;
    int engine_toggle_pwm_threshold;  // PWM threshold to consider engine "on"
    
    // Processing thread
    pthread_t processing_thread;
    bool processing_running;
    
    // Audio tracks (optional)
    Sound *track_starting;  // Track for starting transition
    Sound *track_running;   // Track for running state
    Sound *track_stopping;  // Track for stopping transition
    
    // Audio configuration
    AudioMixer *mixer;
    int audio_channel;
    
    // Flag to track if we need to start the running sound after starting finishes
    bool pending_running_sound;
    
    // Offset in milliseconds to play starting track from when restarting from stopping state
    int starting_offset_from_stopping_ms;
    
    // Offset in milliseconds to play stopping track from when stopping from starting state
    int stopping_offset_from_starting_ms;
};

// Processing thread to monitor PWM and manage engine state
static void* engine_fx_processing_thread(void *arg) {
    EngineFX *engine = (EngineFX *)arg;
    PWMReading reading;
    bool engine_switch_on = false;
    bool previous_switch_state = false;
    
    // Noise filtering: require consecutive successful readings before changing state
    const int REQUIRED_CONSECUTIVE_READINGS = 3;
    int consecutive_on_count = 0;
    int consecutive_off_count = 0;
    
    printf("[ENGINE] Processing thread started\n");
    
    while (engine->processing_running) {
        // Get PWM reading - maintain previous state if reading fails
        if (engine->engine_toggle_pwm_monitor && pwm_monitor_get_reading(engine->engine_toggle_pwm_monitor, &reading)) {
            // Apply hysteresis/deadzone to prevent noise from causing rapid state changes
            // Deadzone: +/- 100us from threshold
            const int DEADZONE_US = 100;
            bool pwm_above_threshold;
            
            if (engine_switch_on) {
                // Currently ON: needs to drop below (threshold - deadzone) to turn OFF
                pwm_above_threshold = (reading.duration_us >= (engine->engine_toggle_pwm_threshold - DEADZONE_US));
            } else {
                // Currently OFF: needs to rise above (threshold + deadzone) to turn ON
                pwm_above_threshold = (reading.duration_us >= (engine->engine_toggle_pwm_threshold + DEADZONE_US));
            }
            
            // Count consecutive readings in new state
            if (pwm_above_threshold && !engine_switch_on) {
                // Signal wants to turn ON
                consecutive_on_count++;
                consecutive_off_count = 0;
                
                if (consecutive_on_count >= REQUIRED_CONSECUTIVE_READINGS) {
                    engine_switch_on = true;
                    printf("[ENGINE] PWM toggle changed: ON (duration=%d us, threshold=%d us)\n",
                           reading.duration_us, engine->engine_toggle_pwm_threshold);
                    consecutive_on_count = 0;
                }
            } else if (!pwm_above_threshold && engine_switch_on) {
                // Signal wants to turn OFF
                consecutive_off_count++;
                consecutive_on_count = 0;
                
                if (consecutive_off_count >= REQUIRED_CONSECUTIVE_READINGS) {
                    engine_switch_on = false;
                    printf("[ENGINE] PWM toggle changed: OFF (duration=%d us, threshold=%d us)\n",
                           reading.duration_us, engine->engine_toggle_pwm_threshold);
                    consecutive_off_count = 0;
                }
            } else {
                // Signal stable in current state
                consecutive_on_count = 0;
                consecutive_off_count = 0;
            }
            
            previous_switch_state = engine_switch_on;
        } else {
            // PWM reading failed or unavailable - maintain previous state and don't reset counters
            engine_switch_on = previous_switch_state;
        }
        
        pthread_mutex_lock(&engine->mutex);
        EngineState current_state = engine->state;
        pthread_mutex_unlock(&engine->mutex);
        
        // STOPPED + switch ON → start engine (STARTING or RUNNING)
        if (current_state == ENGINE_STOPPED && engine_switch_on) {
            pthread_mutex_lock(&engine->mutex);
            engine->pending_running_sound = (engine->track_running != NULL);
            
            if (engine->mixer && engine->track_starting) {
                engine->state = ENGINE_STARTING;
                printf("[ENGINE] Transitioning to STARTING\n");
                
                PlaybackOptions opts = {.loop = false, .volume = 1.0f};
                audio_mixer_play(engine->mixer, engine->audio_channel, engine->track_starting, &opts);
                printf("[ENGINE] Playing starting sound\n");
            } else {
                engine->state = ENGINE_RUNNING;
                printf("[ENGINE] Transitioning to RUNNING (no starting sound)\n");
            }
            
            pthread_mutex_unlock(&engine->mutex);
            continue;
        }
        
        // (STARTING or RUNNING) + pending sound → play running sound when ready
        if ((current_state == ENGINE_STARTING || current_state == ENGINE_RUNNING) && 
            engine->pending_running_sound &&
            !audio_mixer_is_channel_playing(engine->mixer, engine->audio_channel)) {
            pthread_mutex_lock(&engine->mutex);
            engine->state = ENGINE_RUNNING;
            printf("[ENGINE] Transitioning to RUNNING\n");
            
            PlaybackOptions opts = {.loop = true, .volume = 1.0f};
            audio_mixer_play(engine->mixer, engine->audio_channel, engine->track_running, &opts);
            printf("[ENGINE] Playing running sound (looping)\n");
            engine->pending_running_sound = false;
            pthread_mutex_unlock(&engine->mutex);
            continue;
        }
        
        // (STARTING or RUNNING) + switch OFF → stop engine (STOPPING or STOPPED)
        if ((current_state == ENGINE_STARTING || current_state == ENGINE_RUNNING) && !engine_switch_on) {
            pthread_mutex_lock(&engine->mutex);
            
            if (engine->mixer && engine->track_stopping) {
                engine->state = ENGINE_STOPPING;
                printf("[ENGINE] Transitioning to STOPPING\n");
                
                PlaybackOptions opts = {.loop = false, .volume = 1.0f};
                
                // Use stopping offset if transitioning from STARTING state
                if (current_state == ENGINE_STARTING && engine->stopping_offset_from_starting_ms > 0) {
                    audio_mixer_play_from(engine->mixer, engine->audio_channel, engine->track_stopping,
                                         engine->stopping_offset_from_starting_ms, &opts);
                    printf("[ENGINE] Playing stopping sound from %dms\n", engine->stopping_offset_from_starting_ms);
                } else {
                    audio_mixer_play(engine->mixer, engine->audio_channel, engine->track_stopping, &opts);
                    printf("[ENGINE] Playing stopping sound\n");
                }
            } else {
                engine->state = ENGINE_STOPPED;
                printf("[ENGINE] Transitioning to STOPPED (no stopping sound)\n");
                
                if (engine->mixer) {
                    audio_mixer_stop_channel(engine->mixer, engine->audio_channel, STOP_IMMEDIATE);
                }
            }
            
            pthread_mutex_unlock(&engine->mutex);
            continue;
        }
        
        // STOPPING + switch ON → restart engine (STARTING or RUNNING)
        if (current_state == ENGINE_STOPPING && engine_switch_on) {
            pthread_mutex_lock(&engine->mutex);
            engine->pending_running_sound = (engine->track_running != NULL);
            
            if (engine->mixer && engine->track_starting) {
                engine->state = ENGINE_STARTING;
                printf("[ENGINE] Pre-empting STOPPING, transitioning to STARTING\n");
                
                PlaybackOptions opts = {.loop = false, .volume = 1.0f};
                
                // Use restart offset if specified
                if (engine->starting_offset_from_stopping_ms > 0) {
                    audio_mixer_play_from(engine->mixer, engine->audio_channel, engine->track_starting, 
                                         engine->starting_offset_from_stopping_ms, &opts);
                    printf("[ENGINE] Playing starting sound from %dms\n", engine->starting_offset_from_stopping_ms);
                } else {
                    audio_mixer_play(engine->mixer, engine->audio_channel, engine->track_starting, &opts);
                    printf("[ENGINE] Playing starting sound\n");
                }
            } else {
                engine->state = ENGINE_RUNNING;
                printf("[ENGINE] Pre-empting STOPPING, transitioning to RUNNING (no starting sound)\n");
            }
            
            pthread_mutex_unlock(&engine->mutex);
            continue;
        }
        
        // STOPPING + sound finished → STOPPED
        if (current_state == ENGINE_STOPPING && 
            !audio_mixer_is_channel_playing(engine->mixer, engine->audio_channel)) {
            pthread_mutex_lock(&engine->mutex);
            engine->state = ENGINE_STOPPED;
            printf("[ENGINE] Transitioning to STOPPED\n");
            pthread_mutex_unlock(&engine->mutex);
            continue;
        }
        
        usleep(10000); // 10ms loop delay for smooth audio transitions
    }
    
    printf("[ENGINE] Processing thread stopped\n");
    return NULL;
}

EngineFX* engine_fx_create(AudioMixer *mixer, int audio_channel, 
                           int engine_toggle_pwm_pin,
                           int engine_toggle_pwm_threshold,
                           int starting_offset_from_stopping_ms,
                           int stopping_offset_from_starting_ms) {
    EngineFX *engine = (EngineFX *)calloc(1, sizeof(EngineFX));
    if (!engine) {
        fprintf(stderr, "[ENGINE] Error: Cannot allocate memory for engine\n");
        return NULL;
    }
    
    engine->state = ENGINE_STOPPED;
    engine->mixer = mixer;
    engine->audio_channel = audio_channel;
    engine->engine_toggle_pwm_pin = engine_toggle_pwm_pin;
    engine->engine_toggle_pwm_threshold = engine_toggle_pwm_threshold;
    engine->starting_offset_from_stopping_ms = starting_offset_from_stopping_ms;
    engine->stopping_offset_from_starting_ms = stopping_offset_from_starting_ms;
    engine->track_starting = NULL;
    engine->track_running = NULL;
    engine->track_stopping = NULL;
    engine->engine_toggle_pwm_monitor = NULL;
    engine->processing_running = false;
    pthread_mutex_init(&engine->mutex, NULL);
    
    // Create PWM monitor if pin specified
    if (engine_toggle_pwm_pin >= 0) {
        engine->engine_toggle_pwm_monitor = pwm_monitor_create(engine_toggle_pwm_pin, NULL, NULL);
        if (!engine->engine_toggle_pwm_monitor) {
            fprintf(stderr, "[ENGINE] Warning: Failed to create PWM monitor for pin %d\n", engine_toggle_pwm_pin);
        } else {
            pwm_monitor_start(engine->engine_toggle_pwm_monitor);
            printf("[ENGINE] PWM monitoring started on pin %d (threshold: %d us)\n",
                   engine_toggle_pwm_pin, engine_toggle_pwm_threshold);
        }
    }
    
    // Start processing thread
    engine->processing_running = true;
    if (pthread_create(&engine->processing_thread, NULL, engine_fx_processing_thread, engine) != 0) {
        fprintf(stderr, "[ENGINE] Error: Failed to create processing thread\n");
        engine->processing_running = false;
        if (engine->engine_toggle_pwm_monitor) {
            pwm_monitor_stop(engine->engine_toggle_pwm_monitor);
            pwm_monitor_destroy(engine->engine_toggle_pwm_monitor);
        }
        pthread_mutex_destroy(&engine->mutex);
        free(engine);
        return NULL;
    }
    
    printf("[ENGINE] Engine FX created (channel: %d)\n", audio_channel);
    return engine;
}

void engine_fx_destroy(EngineFX *engine) {
    if (!engine) return;
    
    // Stop processing thread
    if (engine->processing_running) {
        engine->processing_running = false;
        pthread_join(engine->processing_thread, NULL);
    }
    
    // Stop and destroy PWM monitor
    if (engine->engine_toggle_pwm_monitor) {
        pwm_monitor_stop(engine->engine_toggle_pwm_monitor);
        pwm_monitor_destroy(engine->engine_toggle_pwm_monitor);
    }
    
    pthread_mutex_destroy(&engine->mutex);
    free(engine);
    
    printf("[ENGINE] Engine FX destroyed\n");
}

int engine_fx_load_sounds(EngineFX *engine, Sound *starting_sound, Sound *running_sound, Sound *stopping_sound) {
    if (!engine) return -1;
    
    pthread_mutex_lock(&engine->mutex);
    
    engine->track_starting = starting_sound;
    engine->track_running = running_sound;
    engine->track_stopping = stopping_sound;
    
    pthread_mutex_unlock(&engine->mutex);
    
    printf("[ENGINE] Sounds loaded (starting: %s, running: %s, stopping: %s)\n",
           starting_sound ? "yes" : "no",
           running_sound ? "yes" : "no",
           stopping_sound ? "yes" : "no");
    
    return 0;
}

EngineState engine_fx_get_state(EngineFX *engine) {
    if (!engine) return ENGINE_STOPPED;
    
    pthread_mutex_lock(&engine->mutex);
    EngineState state = engine->state;
    pthread_mutex_unlock(&engine->mutex);
    
    return state;
}

const char* engine_fx_state_to_string(EngineState state) {
    switch (state) {
        case ENGINE_STOPPED: return "STOPPED";
        case ENGINE_STARTING: return "STARTING";
        case ENGINE_RUNNING: return "RUNNING";
        case ENGINE_STOPPING: return "STOPPING";
        default: return "UNKNOWN";
    }
}

bool engine_fx_is_transitioning(EngineFX *engine) {
    if (!engine) return false;
    
    pthread_mutex_lock(&engine->mutex);
    bool transitioning = (engine->state == ENGINE_STOPPING);
    pthread_mutex_unlock(&engine->mutex);
    
    return transitioning;
}
