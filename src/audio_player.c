#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_player.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>  // C23 standard threads
#include <unistd.h>   // For home directory expansion
#include <pwd.h>      // For getpwnam (user lookup)

// ============================================================================
// SOUND IMPLEMENTATION - For Loading Audio Files
// ============================================================================

struct Sound {
    ma_decoder decoder;
    char *filename;
    bool is_loaded;
};

// Expand tilde (~) in path to home directory
// Supports:
//   ~ -> current user's home
//   ~username -> specific user's home
//   Regular paths -> returned as-is
static char* expand_path(const char *path) {
    if (!path) return nullptr;
    
    // If path doesn't start with ~, return a copy as-is
    if (path[0] != '~') {
        return strdup(path);
    }
    
    // Handle ~username or just ~
    const char *rest_of_path = nullptr;
    const char *home = nullptr;
    
    if (path[1] == '/' || path[1] == '\0') {
        // Just ~ or ~/... -> use current user's home
        home = getenv("HOME");
        if (!home) home = ".";
        rest_of_path = path + 1;  // Skip just the ~
    } else {
        // ~username or ~username/... -> look up user's home
        const char *slash = strchr(path, '/');
        if (slash) {
            rest_of_path = slash;  // Keep the slash
        } else {
            rest_of_path = "";  // No slash, just username
        }
        
        // Extract username
        size_t username_len = (slash ? (size_t)(slash - path) : strlen(path)) - 1;  // -1 to skip ~
        char username[256];
        if (username_len >= sizeof(username)) {
            return nullptr;  // Username too long
        }
        strncpy(username, path + 1, username_len);
        username[username_len] = '\0';
        
        // Look up user's home directory
        struct passwd *pw = getpwnam(username);
        if (!pw) {
            // User not found, fallback to current home
            home = getenv("HOME");
            if (!home) home = ".";
            rest_of_path = path + 1;  // Reset to ~username/...
        } else {
            home = pw->pw_dir;
        }
    }
    
    // Calculate buffer size: home length + rest of path
    size_t home_len = strlen(home);
    size_t rest_len = strlen(rest_of_path);
    char *expanded = malloc(home_len + rest_len + 1);
    
    if (!expanded) {
        return nullptr;
    }
    
    strcpy(expanded, home);
    strcat(expanded, rest_of_path);
    
    return expanded;
}

Sound* sound_load(const char *filename) {
    if (!filename) {
        LOG_ERROR(LOG_AUDIO, "Filename is nullptr");
        return nullptr;
    }
    
    Sound *sound = calloc(1, sizeof(Sound));
    if (!sound) {
        LOG_ERROR(LOG_AUDIO, "Cannot allocate memory for sound");
        return nullptr;
    }
    
    // Expand path if it contains tilde
    char *expanded_path = expand_path(filename);
    if (!expanded_path) {
        LOG_ERROR(LOG_AUDIO, "Cannot expand path: %s", filename);
        free(sound);
        return nullptr;
    }
    
    // Initialize decoder
    ma_result result = ma_decoder_init_file(expanded_path, nullptr, &sound->decoder);
    if (result != MA_SUCCESS) {
        LOG_ERROR(LOG_AUDIO, "Failed to load audio file: %s (expanded: %s)", filename, expanded_path);
        free(expanded_path);
        free(sound);
        return nullptr;
    }
    
    sound->filename = strdup(filename);
    if (!sound->filename) {
        LOG_ERROR(LOG_AUDIO, "Cannot allocate memory for filename");
        ma_decoder_uninit(&sound->decoder);
        free(expanded_path);
        free(sound);
        return nullptr;
    }
    
    sound->is_loaded = true;
    
    LOG_INFO(LOG_AUDIO, "Loaded sound: %s", filename);
    free(expanded_path);  // Clean up expanded path after loading
    return sound;
}

void sound_destroy(Sound *sound) {
    if (!sound) return;
    
    if (sound->is_loaded) {
        ma_decoder_uninit(&sound->decoder);
    }
    
    free(sound->filename);
    free(sound);
}

// ============================================================================
// AUDIO MIXER IMPLEMENTATION - For Parallel Playback
// ============================================================================

#define MAX_MIXER_CHANNELS 8

struct AudioMixer {
    ma_engine engine;
    ma_sound *sounds[MAX_MIXER_CHANNELS];
    
    mtx_t mixer_mutex;  // C23 standard mutex
    int max_channels;
    
    bool active[MAX_MIXER_CHANNELS];
    bool loop[MAX_MIXER_CHANNELS];
    float volume[MAX_MIXER_CHANNELS];
    
    bool engine_initialized;
    float master_volume;
};

AudioMixer* audio_mixer_create(int max_channels) {
    if (max_channels <= 0 || max_channels > MAX_MIXER_CHANNELS) {
        LOG_ERROR(LOG_AUDIO, "Invalid channel count (max: %d)", MAX_MIXER_CHANNELS);
        return nullptr;
    }
    
    AudioMixer *mixer = calloc(1, sizeof(AudioMixer));
    if (!mixer) {
        LOG_ERROR(LOG_AUDIO, "Cannot allocate memory for audio mixer");
        return nullptr;
    }
    
    // Initialize miniaudio engine with proper channel configuration (C23 designated initializer)
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;  // Force stereo output for WM8960
    
    ma_result result = ma_engine_init(&engineConfig, &mixer->engine);
    if (result != MA_SUCCESS) {
        LOG_ERROR(LOG_AUDIO, "Failed to initialize mixer engine");
        free(mixer);
        return nullptr;
    }
    
    mixer->max_channels = max_channels;
    mixer->master_volume = 1.0f;
    mixer->engine_initialized = true;
    
    mtx_init(&mixer->mixer_mutex, mtx_plain);  // C23 standard mutex initialization
    
    for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
        mixer->active[i] = false;
        mixer->volume[i] = 1.0f;
        mixer->sounds[i] = nullptr;
    }
    
    // Set master volume on engine
    ma_engine_set_volume(&mixer->engine, mixer->master_volume);
    
    LOG_INFO(LOG_AUDIO, "Created mixer with %d channels", max_channels);
    return mixer;
}

void audio_mixer_destroy(AudioMixer *mixer) {
    if (!mixer) return;
    
    // Stop all playback
    audio_mixer_stop_channel(mixer, -1, STOP_IMMEDIATE);
    
    mtx_lock(&mixer->mixer_mutex);
    
    // Uninit all sounds
    for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
        if (mixer->sounds[i]) {
            ma_sound_uninit(mixer->sounds[i]);
            free(mixer->sounds[i]);
            mixer->sounds[i] = nullptr;
        }
    }
    
    // Uninit engine
    if (mixer->engine_initialized) {
        ma_engine_uninit(&mixer->engine);
    }
    
    mtx_unlock(&mixer->mixer_mutex);
    mtx_destroy(&mixer->mixer_mutex);
    
    free(mixer);
}

int audio_mixer_play(AudioMixer *mixer, int channel_id, Sound *sound, const PlaybackOptions *options) {
    if (!mixer || !sound || channel_id < 0 || channel_id >= mixer->max_channels) {
        return -1;
    }
    
    mtx_lock(&mixer->mixer_mutex);
    
    // Uninit existing sound if any
    if (mixer->sounds[channel_id]) {
        ma_sound_uninit(mixer->sounds[channel_id]);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = nullptr;
    }
    
    // Allocate and initialize new sound
    mixer->sounds[channel_id] = malloc(sizeof(ma_sound));
    if (!mixer->sounds[channel_id]) {
        LOG_ERROR(LOG_AUDIO, "Cannot allocate memory for sound on channel %d", channel_id);
        mtx_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Seek decoder back to start for reusability
    ma_decoder_seek_to_pcm_frame(&sound->decoder, 0);
    
    ma_result result = ma_sound_init_from_data_source(&mixer->engine, &sound->decoder,
                                                       MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                       nullptr, mixer->sounds[channel_id]);
    if (result != MA_SUCCESS) {
        LOG_ERROR(LOG_AUDIO, "Failed to initialize sound on channel %d", channel_id);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = nullptr;
        mtx_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Set channel properties
    mixer->active[channel_id] = true;
    
    if (options) {
        mixer->loop[channel_id] = options->loop;
        mixer->volume[channel_id] = options->volume;
        ma_sound_set_looping(mixer->sounds[channel_id], options->loop);
        ma_sound_set_volume(mixer->sounds[channel_id], options->volume);
    } else {
        mixer->loop[channel_id] = false;
        mixer->volume[channel_id] = 1.0f;
        ma_sound_set_looping(mixer->sounds[channel_id], MA_FALSE);
        ma_sound_set_volume(mixer->sounds[channel_id], 1.0f);
    }
    
    // Start playing immediately
    ma_sound_start(mixer->sounds[channel_id]);
    LOG_INFO(LOG_AUDIO, "Channel %d: Playing %s", channel_id, sound->filename);
    
    mtx_unlock(&mixer->mixer_mutex);
    return 0;
}

int audio_mixer_play_from(AudioMixer *mixer, int channel_id, Sound *sound, int start_ms, const PlaybackOptions *options) {
    if (!mixer || !sound || channel_id < 0 || channel_id >= mixer->max_channels) {
        return -1;
    }
    
    mtx_lock(&mixer->mixer_mutex);
    
    // Uninit existing sound if any
    if (mixer->sounds[channel_id]) {
        ma_sound_uninit(mixer->sounds[channel_id]);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = nullptr;
    }
    
    // Allocate and initialize new sound
    mixer->sounds[channel_id] = malloc(sizeof(ma_sound));
    if (!mixer->sounds[channel_id]) {
        LOG_ERROR(LOG_AUDIO, "Cannot allocate memory for sound on channel %d", channel_id);
        mtx_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Get sample rate to convert milliseconds to frames
    ma_uint32 sample_rate;
    ma_data_source_get_data_format(&sound->decoder, nullptr, nullptr, &sample_rate, nullptr, 0);
    
    // Calculate frame position from milliseconds
    ma_uint64 start_frame = (ma_uint64)((start_ms / 1000.0) * sample_rate);
    
    // Seek decoder to start position
    ma_decoder_seek_to_pcm_frame(&sound->decoder, start_frame);
    
    ma_result result = ma_sound_init_from_data_source(&mixer->engine, &sound->decoder,
                                                       MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                       nullptr, mixer->sounds[channel_id]);
    if (result != MA_SUCCESS) {
        LOG_ERROR(LOG_AUDIO, "Failed to initialize sound on channel %d", channel_id);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = nullptr;
        mtx_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Set channel properties
    mixer->active[channel_id] = true;
    
    if (options) {
        mixer->loop[channel_id] = options->loop;
        mixer->volume[channel_id] = options->volume;
        ma_sound_set_looping(mixer->sounds[channel_id], options->loop);
        ma_sound_set_volume(mixer->sounds[channel_id], options->volume);
    } else {
        mixer->loop[channel_id] = false;
        mixer->volume[channel_id] = 1.0f;
        ma_sound_set_looping(mixer->sounds[channel_id], MA_FALSE);
        ma_sound_set_volume(mixer->sounds[channel_id], 1.0f);
    }
    
    // Start playing immediately
    ma_sound_start(mixer->sounds[channel_id]);
    LOG_INFO(LOG_AUDIO, "Channel %d: Playing %s from %dms", channel_id, sound->filename, start_ms);
    
    mtx_unlock(&mixer->mixer_mutex);
    return 0;
}

int audio_mixer_start_channel(AudioMixer *mixer, int channel_id) {
    if (!mixer || channel_id < 0 || channel_id >= mixer->max_channels) {
        return -1;
    }
    
    mtx_lock(&mixer->mixer_mutex);
    
    if (!mixer->active[channel_id] || !mixer->sounds[channel_id]) {
        LOG_ERROR(LOG_AUDIO, "Channel %d has no track loaded", channel_id);
        mtx_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Seek to start and play
    ma_sound_seek_to_pcm_frame(mixer->sounds[channel_id], 0);
    ma_sound_start(mixer->sounds[channel_id]);
    
    LOG_INFO(LOG_AUDIO, "Started channel %d", channel_id);
    mtx_unlock(&mixer->mixer_mutex);
    
    return 0;
}

int audio_mixer_stop_channel(AudioMixer *mixer, int channel_id, StopMode mode) {
    if (!mixer) return -1;
    
    mtx_lock(&mixer->mixer_mutex);
    
    if (channel_id == -1) {
        // Stop all channels
        for (int i = 0; i < mixer->max_channels; i++) {
            if (mixer->sounds[i]) {
                if (mode == STOP_AFTER_FINISH) {
                    // Disable looping so it stops after finish
                    mixer->loop[i] = false;
                    ma_sound_set_looping(mixer->sounds[i], MA_FALSE);
                } else {
                    ma_sound_stop(mixer->sounds[i]);
                }
            }
        }
    } else if (channel_id >= 0 && channel_id < mixer->max_channels) {
        if (mixer->sounds[channel_id]) {
            if (mode == STOP_AFTER_FINISH) {
                mixer->loop[channel_id] = false;
                ma_sound_set_looping(mixer->sounds[channel_id], MA_FALSE);
            } else {
                ma_sound_stop(mixer->sounds[channel_id]);
            }
        }
    }
    
    mtx_unlock(&mixer->mixer_mutex);
    
    // If stop after finish for all channels, wait for them
    if (channel_id == -1 && mode == STOP_AFTER_FINISH) {
        for (int i = 0; i < mixer->max_channels; i++) {
            if (mixer->sounds[i]) {
                while (ma_sound_is_playing(mixer->sounds[i])) {
                    ma_sleep(10);
                }
            }
        }
    } else if (channel_id >= 0 && mode == STOP_AFTER_FINISH && mixer->sounds[channel_id]) {
        while (ma_sound_is_playing(mixer->sounds[channel_id])) {
            ma_sleep(10);
        }
    }
    
    return 0;
}

int audio_mixer_set_volume(AudioMixer *mixer, int channel_id, float volume) {
    if (!mixer) return -1;
    
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    
    mtx_lock(&mixer->mixer_mutex);
    
    if (channel_id == -1) {
        mixer->master_volume = volume;
        ma_engine_set_volume(&mixer->engine, volume);
    } else if (channel_id >= 0 && channel_id < mixer->max_channels) {
        mixer->volume[channel_id] = volume;
        if (mixer->sounds[channel_id]) {
            ma_sound_set_volume(mixer->sounds[channel_id], volume);
        }
    }
    
    mtx_unlock(&mixer->mixer_mutex);
    return 0;
}

bool audio_mixer_is_playing(AudioMixer *mixer) {
    if (!mixer) return false;
    
    mtx_lock(&mixer->mixer_mutex);
    
    bool playing = false;
    for (int i = 0; i < mixer->max_channels; i++) {
        if (mixer->sounds[i] && ma_sound_is_playing(mixer->sounds[i])) {
            playing = true;
            break;
        }
    }
    
    mtx_unlock(&mixer->mixer_mutex);
    return playing;
}

bool audio_mixer_is_channel_playing(AudioMixer *mixer, int channel_id) {
    if (!mixer || channel_id < 0 || channel_id >= mixer->max_channels) {
        return false;
    }
    
    mtx_lock(&mixer->mixer_mutex);
    
    bool playing = false;
    if (mixer->sounds[channel_id]) {
        playing = ma_sound_is_playing(mixer->sounds[channel_id]);
    }
    
    mtx_unlock(&mixer->mixer_mutex);
    return playing;
}

int audio_mixer_stop_looping(AudioMixer *mixer, int channel_id) {
    if (!mixer) return -1;
    
    mtx_lock(&mixer->mixer_mutex);
    
    if (channel_id == -1) {
        // Disable looping on all channels
        for (int i = 0; i < mixer->max_channels; i++) {
            if (mixer->sounds[i]) {
                mixer->loop[i] = false;
                ma_sound_set_looping(mixer->sounds[i], MA_FALSE);
            }
        }
        LOG_INFO(LOG_AUDIO, "Looping disabled on all channels - will finish current iterations");
    } else if (channel_id >= 0 && channel_id < mixer->max_channels) {
        if (mixer->sounds[channel_id]) {
            mixer->loop[channel_id] = false;
            ma_sound_set_looping(mixer->sounds[channel_id], MA_FALSE);
            LOG_INFO(LOG_AUDIO, "Looping disabled on channel %d - will finish current iteration", channel_id);
        }
    }
    
    mtx_unlock(&mixer->mixer_mutex);
    return 0;
}

// ============================================================================
// SOUND MANAGER IMPLEMENTATION - For Managing Sound Collections
// ============================================================================

struct SoundManager {
    Sound *sounds[SOUND_ID_COUNT];
};

SoundManager* sound_manager_create(void) {
    SoundManager *manager = calloc(1, sizeof(SoundManager));
    if (!manager) {
        LOG_ERROR(LOG_AUDIO, "Cannot allocate memory for sound manager");
        return nullptr;
    }
    
    LOG_INFO(LOG_AUDIO, "Sound manager created");
    return manager;
}

void sound_manager_destroy(SoundManager *manager) {
    if (!manager) return;
    
    // Destroy all sounds
    for (int i = 0; i < SOUND_ID_COUNT; i++) {
        if (manager->sounds[i]) {
            sound_destroy(manager->sounds[i]);
            manager->sounds[i] = nullptr;
        }
    }
    
    free(manager);
    LOG_INFO(LOG_AUDIO, "Sound manager destroyed");
}

int sound_manager_load_sound(SoundManager *manager, SoundID id, const char *filename) {
    if (!manager) return -1;
    if (id < 0 || id >= SOUND_ID_COUNT) return -1;
    
    // Skip if filename is nullptr
    if (!filename) return 0;
    
    // Destroy existing sound if any
    if (manager->sounds[id]) {
        sound_destroy(manager->sounds[id]);
        manager->sounds[id] = nullptr;
    }
    
    // Load new sound
    manager->sounds[id] = sound_load(filename);
    if (!manager->sounds[id]) {
        LOG_ERROR(LOG_AUDIO, "Failed to load sound %d from %s", id, filename);
        return -1;
    }
    
    return 0;
}

Sound* sound_manager_get_sound(SoundManager *manager, SoundID id) {
    if (!manager) return nullptr;
    if (id < 0 || id >= SOUND_ID_COUNT) return nullptr;
    return manager->sounds[id];
}
