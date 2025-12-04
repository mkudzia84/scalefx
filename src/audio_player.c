#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// ============================================================================
// SOUND IMPLEMENTATION - For Loading Audio Files
// ============================================================================

struct Sound {
    ma_decoder decoder;
    char *filename;
    bool is_loaded;
};

Sound* sound_load(const char *filename) {
    if (!filename) {
        fprintf(stderr, "[AUDIO] Error: Filename is NULL\n");
        return NULL;
    }
    
    Sound *sound = (Sound*)calloc(1, sizeof(Sound));
    if (!sound) {
        fprintf(stderr, "[AUDIO] Error: Cannot allocate memory for sound\n");
        return NULL;
    }
    
    // Initialize decoder
    ma_result result = ma_decoder_init_file(filename, NULL, &sound->decoder);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[AUDIO] Error: Failed to load audio file: %s\n", filename);
        free(sound);
        return NULL;
    }
    
    sound->filename = strdup(filename);
    if (!sound->filename) {
        fprintf(stderr, "[AUDIO] Error: Cannot allocate memory for filename\n");
        ma_decoder_uninit(&sound->decoder);
        free(sound);
        return NULL;
    }
    
    sound->is_loaded = true;
    
    printf("[AUDIO] Loaded sound: %s\n", filename);
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
    
    pthread_mutex_t mixer_mutex;
    int max_channels;
    
    bool active[MAX_MIXER_CHANNELS];
    bool loop[MAX_MIXER_CHANNELS];
    float volume[MAX_MIXER_CHANNELS];
    
    bool engine_initialized;
    float master_volume;
};

AudioMixer* audio_mixer_create(int max_channels) {
    if (max_channels <= 0 || max_channels > MAX_MIXER_CHANNELS) {
        fprintf(stderr, "[AUDIO] Error: Invalid channel count (max: %d)\n", MAX_MIXER_CHANNELS);
        return NULL;
    }
    
    AudioMixer *mixer = (AudioMixer*)calloc(1, sizeof(AudioMixer));
    if (!mixer) {
        fprintf(stderr, "[AUDIO] Error: Cannot allocate memory for audio mixer\n");
        return NULL;
    }
    
    // Initialize miniaudio engine with proper channel configuration
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;  // Force stereo output for WM8960
    
    ma_result result = ma_engine_init(&engineConfig, &mixer->engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[AUDIO] Error: Failed to initialize mixer engine\n");
        free(mixer);
        return NULL;
    }
    
    mixer->max_channels = max_channels;
    mixer->master_volume = 1.0f;
    mixer->engine_initialized = true;
    
    pthread_mutex_init(&mixer->mixer_mutex, NULL);
    
    for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
        mixer->active[i] = false;
        mixer->volume[i] = 1.0f;
        mixer->sounds[i] = NULL;
    }
    
    // Set master volume on engine
    ma_engine_set_volume(&mixer->engine, mixer->master_volume);
    
    printf("Audio mixer created with %d channels\n", max_channels);
    return mixer;
}

void audio_mixer_destroy(AudioMixer *mixer) {
    if (!mixer) return;
    
    // Stop all playback
    audio_mixer_stop_channel(mixer, -1, STOP_IMMEDIATE);
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    // Uninit all sounds
    for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
        if (mixer->sounds[i]) {
            ma_sound_uninit(mixer->sounds[i]);
            free(mixer->sounds[i]);
            mixer->sounds[i] = NULL;
        }
    }
    
    // Uninit engine
    if (mixer->engine_initialized) {
        ma_engine_uninit(&mixer->engine);
    }
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    pthread_mutex_destroy(&mixer->mixer_mutex);
    
    free(mixer);
}

int audio_mixer_play(AudioMixer *mixer, int channel_id, Sound *sound, const PlaybackOptions *options) {
    if (!mixer || !sound || channel_id < 0 || channel_id >= mixer->max_channels) {
        return -1;
    }
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    // Uninit existing sound if any
    if (mixer->sounds[channel_id]) {
        ma_sound_uninit(mixer->sounds[channel_id]);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = NULL;
    }
    
    // Allocate and initialize new sound
    mixer->sounds[channel_id] = (ma_sound*)malloc(sizeof(ma_sound));
    if (!mixer->sounds[channel_id]) {
        fprintf(stderr, "[AUDIO] Error: Cannot allocate memory for sound on channel %d\n", channel_id);
        pthread_mutex_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Seek decoder back to start for reusability
    ma_decoder_seek_to_pcm_frame(&sound->decoder, 0);
    
    ma_result result = ma_sound_init_from_data_source(&mixer->engine, &sound->decoder,
                                                       MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                       NULL, mixer->sounds[channel_id]);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[AUDIO] Error: Failed to initialize sound on channel %d\n", channel_id);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = NULL;
        pthread_mutex_unlock(&mixer->mixer_mutex);
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
    printf("[AUDIO] Channel %d: Playing %s\n", channel_id, sound->filename);
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    return 0;
}

int audio_mixer_play_from(AudioMixer *mixer, int channel_id, Sound *sound, int start_ms, const PlaybackOptions *options) {
    if (!mixer || !sound || channel_id < 0 || channel_id >= mixer->max_channels) {
        return -1;
    }
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    // Uninit existing sound if any
    if (mixer->sounds[channel_id]) {
        ma_sound_uninit(mixer->sounds[channel_id]);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = NULL;
    }
    
    // Allocate and initialize new sound
    mixer->sounds[channel_id] = (ma_sound*)malloc(sizeof(ma_sound));
    if (!mixer->sounds[channel_id]) {
        fprintf(stderr, "[AUDIO] Error: Cannot allocate memory for sound on channel %d\n", channel_id);
        pthread_mutex_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Get sample rate to convert milliseconds to frames
    ma_uint32 sample_rate;
    ma_data_source_get_data_format(&sound->decoder, NULL, NULL, &sample_rate, NULL, 0);
    
    // Calculate frame position from milliseconds
    ma_uint64 start_frame = (ma_uint64)((start_ms / 1000.0) * sample_rate);
    
    // Seek decoder to start position
    ma_decoder_seek_to_pcm_frame(&sound->decoder, start_frame);
    
    ma_result result = ma_sound_init_from_data_source(&mixer->engine, &sound->decoder,
                                                       MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                       NULL, mixer->sounds[channel_id]);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[AUDIO] Error: Failed to initialize sound on channel %d\n", channel_id);
        free(mixer->sounds[channel_id]);
        mixer->sounds[channel_id] = NULL;
        pthread_mutex_unlock(&mixer->mixer_mutex);
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
    printf("[AUDIO] Channel %d: Playing %s from %dms\n", channel_id, sound->filename, start_ms);
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    return 0;
}

int audio_mixer_start_channel(AudioMixer *mixer, int channel_id) {
    if (!mixer || channel_id < 0 || channel_id >= mixer->max_channels) {
        return -1;
    }
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    if (!mixer->active[channel_id] || !mixer->sounds[channel_id]) {
        fprintf(stderr, "[AUDIO] Error: Channel %d has no track loaded\n", channel_id);
        pthread_mutex_unlock(&mixer->mixer_mutex);
        return -1;
    }
    
    // Seek to start and play
    ma_sound_seek_to_pcm_frame(mixer->sounds[channel_id], 0);
    ma_sound_start(mixer->sounds[channel_id]);
    
    printf("[AUDIO] Started channel %d\n", channel_id);
    pthread_mutex_unlock(&mixer->mixer_mutex);
    
    return 0;
}

int audio_mixer_stop_channel(AudioMixer *mixer, int channel_id, StopMode mode) {
    if (!mixer) return -1;
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
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
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    
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
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    if (channel_id == -1) {
        mixer->master_volume = volume;
        ma_engine_set_volume(&mixer->engine, volume);
    } else if (channel_id >= 0 && channel_id < mixer->max_channels) {
        mixer->volume[channel_id] = volume;
        if (mixer->sounds[channel_id]) {
            ma_sound_set_volume(mixer->sounds[channel_id], volume);
        }
    }
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    return 0;
}

bool audio_mixer_is_playing(AudioMixer *mixer) {
    if (!mixer) return false;
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    bool playing = false;
    for (int i = 0; i < mixer->max_channels; i++) {
        if (mixer->sounds[i] && ma_sound_is_playing(mixer->sounds[i])) {
            playing = true;
            break;
        }
    }
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    return playing;
}

bool audio_mixer_is_channel_playing(AudioMixer *mixer, int channel_id) {
    if (!mixer || channel_id < 0 || channel_id >= mixer->max_channels) {
        return false;
    }
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    bool playing = false;
    if (mixer->sounds[channel_id]) {
        playing = ma_sound_is_playing(mixer->sounds[channel_id]);
    }
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    return playing;
}

int audio_mixer_stop_looping(AudioMixer *mixer, int channel_id) {
    if (!mixer) return -1;
    
    pthread_mutex_lock(&mixer->mixer_mutex);
    
    if (channel_id == -1) {
        // Disable looping on all channels
        for (int i = 0; i < mixer->max_channels; i++) {
            if (mixer->sounds[i]) {
                mixer->loop[i] = false;
                ma_sound_set_looping(mixer->sounds[i], MA_FALSE);
            }
        }
        printf("[AUDIO] Looping disabled on all channels - will finish current iterations\n");
    } else if (channel_id >= 0 && channel_id < mixer->max_channels) {
        if (mixer->sounds[channel_id]) {
            mixer->loop[channel_id] = false;
            ma_sound_set_looping(mixer->sounds[channel_id], MA_FALSE);
            printf("[AUDIO] Looping disabled on channel %d - will finish current iteration\n", channel_id);
        }
    }
    
    pthread_mutex_unlock(&mixer->mixer_mutex);
    return 0;
}
