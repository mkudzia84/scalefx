#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>

// Forward declarations
typedef struct AudioMixer AudioMixer;
typedef struct Sound Sound;

// Playback options with C23 designated initializers
typedef struct {
    bool loop;              // Loop playback
    float volume;           // Volume level (0.0 to 1.0)
} PlaybackOptions;

// Default playback options
#define PLAYBACK_DEFAULTS (PlaybackOptions){ .loop = false, .volume = 1.0f }

// Stop options with explicit values (C23 compatible)
typedef enum {
    STOP_IMMEDIATE = 0,         // Stop immediately
    STOP_AFTER_FINISH = 1       // Wait until current track finishes
} StopMode;

// ============================================================================
// SOUND API - For Loading Audio Files
// ============================================================================

/**
 * Create a new sound by loading from file
 * @param filename Path to audio file
 * @return Sound handle or nullptr on error
 */
Sound* sound_load(const char *filename);

/**
 * Destroy sound and free resources
 * @param sound Sound handle
 */
void sound_destroy(Sound *sound);

// ============================================================================
// AUDIO MIXER API - For Parallel Playback
// ============================================================================
// ============================================================================
/**
 * Create a new audio mixer for parallel playback
 * @param max_channels Maximum number of simultaneous audio channels
 * @return AudioMixer handle or nullptr on error
 */
AudioMixer* audio_mixer_create(int max_channels);

/**
 * Destroy audio mixer and free resources
 * @param mixer Audio mixer handle
 */
void audio_mixer_destroy(AudioMixer *mixer);

/**
 * Play a sound on a channel immediately, stopping any current track on that channel
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (0 to max_channels-1)
 * @param sound Sound handle
 * @param options Playback options (or nullptr for defaults)
 * @return 0 on success, -1 on error
 */
int audio_mixer_play(AudioMixer *mixer, int channel_id, Sound *sound, const PlaybackOptions *options);

/**
 * Play a sound on a channel from a specific time position
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (0 to max_channels-1)
 * @param sound Sound handle
 * @param start_ms Start position in milliseconds from beginning of track
 * @param options Playback options (or nullptr for defaults)
 * @return 0 on success, -1 on error
 */
int audio_mixer_play_from(AudioMixer *mixer, int channel_id, Sound *sound, int start_ms, const PlaybackOptions *options);

/**
 * Start playback on a specific channel (channel must have a loaded track)
 * @param mixer Audio mixer handle
 * @param channel_id Channel number
 * @return 0 on success, -1 on error
 */
int audio_mixer_start_channel(AudioMixer *mixer, int channel_id);

/**
 * Stop a specific channel
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (-1 for all channels)
 * @param mode Stop mode
 * @return 0 on success, -1 on error
 */
int audio_mixer_stop_channel(AudioMixer *mixer, int channel_id, StopMode mode);

/**
 * Disable looping on a channel, letting current loop iteration finish
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (-1 for all channels)
 * @return 0 on success, -1 on error
 */
int audio_mixer_stop_looping(AudioMixer *mixer, int channel_id);

/**
 * Set volume for a specific channel
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (-1 for master volume)
 * @param volume Volume level (0.0 to 1.0)
 * @return 0 on success, -1 on error
 */
int audio_mixer_set_volume(AudioMixer *mixer, int channel_id, float volume);

/**
 * Check if mixer is currently playing
 * @param mixer Audio mixer handle
 * @return true if any channel is playing, false otherwise
 */
bool audio_mixer_is_playing(AudioMixer *mixer);

/**
 * Check if a specific channel is currently playing
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (0 to max_channels-1)
 * @return true if channel is playing, false otherwise
 */
bool audio_mixer_is_channel_playing(AudioMixer *mixer, int channel_id);

/**
 * Get remaining time in milliseconds for a channel
 * @param mixer Audio mixer handle
 * @param channel_id Channel number (0 to max_channels-1)
 * @return remaining time in milliseconds, or -1 if channel not active or looping
 */
int audio_mixer_get_channel_remaining_ms(AudioMixer *mixer, int channel_id);

// ============================================================================
// SOUND MANAGER API - For Managing Sound Collections
// ============================================================================

// Sound identifier enum
typedef enum {
    // Engine sounds
    SOUND_ENGINE_STARTING = 0,
    SOUND_ENGINE_RUNNING,
    SOUND_ENGINE_STOPPING,
    
    // Gun sounds
    SOUND_GUN_RATE_1,
    SOUND_GUN_RATE_2,
    SOUND_GUN_RATE_3,
    SOUND_GUN_RATE_4,
    SOUND_GUN_RATE_5,
    SOUND_GUN_RATE_6,
    SOUND_GUN_RATE_7,
    SOUND_GUN_RATE_8,
    SOUND_GUN_RATE_9,
    SOUND_GUN_RATE_10,
    
    SOUND_ID_COUNT
} SoundID;

// Sound manager
typedef struct SoundManager SoundManager;

/**
 * Create a new sound manager
 * @return SoundManager handle or nullptr on error
 */
SoundManager* sound_manager_create(void);

/**
 * Destroy sound manager and all loaded sounds
 * @param manager SoundManager handle
 */
void sound_manager_destroy(SoundManager *manager);

/**
 * Load a sound
 * @param manager SoundManager handle
 * @param id Sound identifier
 * @param filename Path to audio file (can be nullptr to skip loading)
 * @return 0 on success, -1 on error
 */
int sound_manager_load_sound(SoundManager *manager, SoundID id, const char *filename);

/**
 * Get a sound
 * @param manager SoundManager handle
 * @param id Sound identifier
 * @return Sound handle or nullptr if not loaded
 */
Sound* sound_manager_get_sound(SoundManager *manager, SoundID id);

#endif // AUDIO_PLAYER_H
