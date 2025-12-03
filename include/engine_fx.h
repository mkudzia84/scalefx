#ifndef ENGINE_FX_H
#define ENGINE_FX_H

#include <stdbool.h>

// Forward declarations
typedef struct AudioMixer AudioMixer;
typedef struct Sound Sound;

// Engine states
typedef enum {
    ENGINE_STOPPED,     // Engine is completely stopped
    ENGINE_STARTING,    // Engine is spooling up (starting sound playing)
    ENGINE_RUNNING,     // Engine is running
    ENGINE_STOPPING     // Engine is shutting down
} EngineState;

// Forward declaration
typedef struct EngineFX EngineFX;

/**
 * Callback function for engine state changes
 * @param old_state Previous engine state
 * @param new_state New engine state
 * @param user_data User-provided data pointer
 */
typedef void (*EngineStateCallback)(EngineState old_state, EngineState new_state, void *user_data);

/**
 * Create a new engine FX controller
 * @param mixer Audio mixer handle (can be NULL if no audio)
 * @param audio_channel Audio channel to use for engine sounds
 * @param engine_toggle_pwm_pin GPIO pin for PWM input monitoring (-1 to disable)
 * @param engine_toggle_pwm_threshold PWM threshold value to consider engine "on" (microseconds)
 * @param starting_offset_from_stopping_ms Offset in milliseconds to play starting track from when restarting from stopping state (0 for beginning)
 * @param stopping_offset_from_starting_ms Offset in milliseconds to play stopping track from when stopping from starting state (0 for beginning)
 * @return Engine FX handle, or NULL on failure
 */
EngineFX* engine_fx_create(AudioMixer *mixer, int audio_channel, 
                           int engine_toggle_pwm_pin,
                           int engine_toggle_pwm_threshold,
                           int starting_offset_from_stopping_ms,
                           int stopping_offset_from_starting_ms);

/**
 * Destroy engine FX controller
 * @param engine Engine FX handle
 */
void engine_fx_destroy(EngineFX *engine);

/**
 * Load sound tracks for engine (all optional)
 * @param engine Engine FX handle
 * @param starting_sound Sound for starting/transition (NULL to skip)
 * @param running_sound Sound for running state (NULL to skip)
 * @param stopping_sound Sound for stopping transition (NULL to skip)
 * @return 0 on success, -1 on failure
 */
int engine_fx_load_sounds(EngineFX *engine, Sound *starting_sound, Sound *running_sound, Sound *stopping_sound);

/**
 * Get current engine state
 * @param engine Engine FX handle
 * @return Current engine state
 */
EngineState engine_fx_get_state(EngineFX *engine);

/**
 * Get engine state as string
 * @param state Engine state
 * @return String representation of state
 */
const char* engine_fx_state_to_string(EngineState state);

/**
 * Check if engine is in a transitional state
 * @param engine Engine FX handle
 * @return true if starting or stopping, false otherwise
 */
bool engine_fx_is_transitioning(EngineFX *engine);

#endif // ENGINE_FX_H
