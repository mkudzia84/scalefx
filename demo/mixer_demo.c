#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include "audio_player.h"

static AudioMixer *g_mixer = NULL;

void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\nReceived interrupt signal, stopping playback...\n");
        if (g_mixer) {
            audio_mixer_stop_channel(g_mixer, -1, STOP_IMMEDIATE);
        }
        exit(0);
    }
}

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] --channel <ch> <file> [--channel <ch> <file> ...]\n", program_name);
    printf("\nOptions:\n");
    printf("  --channel <N> <file>  Add audio file to channel N (0-7)\n");
    printf("  --loop <N>            Loop channel N indefinitely\n");
    printf("  --volume <N> <0-1>    Set volume for channel N\n");
    printf("  --master-volume <0-1> Set master volume\n");
    printf("\nExamples:\n");
    printf("  Play two tracks simultaneously:\n");
    printf("    %s --channel 0 music.wav --channel 1 effects.wav\n", program_name);
    printf("\n  Background music with looping:\n");
    printf("    %s --channel 0 background.wav --loop 0 --channel 1 sfx.wav\n", program_name);
    printf("\n  Adjust individual volumes:\n");
    printf("    %s --channel 0 music.wav --volume 0 0.7 --channel 1 voice.wav --volume 1 1.0\n", program_name);
    printf("\n  Three parallel tracks:\n");
    printf("\nInteractive Commands:\n");
    printf("  Press 's <N>' to stop channel N (or 's -1' for all)\n");
    printf("  Press 'l <N>' to stop looping on channel N (or 'l -1' for all)\n");
    printf("  Press 'v <N> <vol>' to set volume on channel N (0.0-1.0)\n");
    printf("  Press 'm <vol>' to set master volume (0.0-1.0)\n");
    printf("  Press 'i <N>' to check if channel N is playing\n");
    printf("  Press 'q' to quit\n");
    printf("  Press 'h' or '?' for help\n");
    printf("  Press Ctrl+C to exit\n");
}

int main(int argc, char *argv[]) {
    typedef struct {
        const char *filename;
        bool loop;
        float volume;
    } ChannelConfig;
    
    ChannelConfig channels[8] = {0};
    int channel_count = 0;
    float master_volume = 1.0f;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--channel") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "Error: --channel requires channel number and filename\n");
                return 1;
            }
            int ch = atoi(argv[++i]);
            const char *file = argv[++i];
            
            if (ch < 0 || ch >= 8) {
                fprintf(stderr, "Error: Channel must be 0-7\n");
                return 1;
            }
            
            channels[ch].filename = file;
            channels[ch].volume = 1.0f;
            if (ch >= channel_count) {
                channel_count = ch + 1;
            }
        } else if (strcmp(argv[i], "--loop") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --loop requires channel number\n");
                return 1;
            }
            int ch = atoi(argv[++i]);
            if (ch >= 0 && ch < 8) {
                channels[ch].loop = true;
            }
        } else if (strcmp(argv[i], "--volume") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "Error: --volume requires channel number and value\n");
                return 1;
            }
            int ch = atoi(argv[++i]);
            float vol = atof(argv[++i]);
            if (ch >= 0 && ch < 8) {
                channels[ch].volume = vol;
            }
        } else if (strcmp(argv[i], "--master-volume") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --master-volume requires a value\n");
                return 1;
            }
            master_volume = atof(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Check if any channels were configured
    bool has_channels = false;
    for (int i = 0; i < 8; i++) {
        if (channels[i].filename) {
            has_channels = true;
            break;
        }
    }
    
    if (!has_channels) {
        fprintf(stderr, "Error: No audio channels specified\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    
    // Create audio mixer
    g_mixer = audio_mixer_create(8);
    if (!g_mixer) {
        fprintf(stderr, "Error: Failed to create audio mixer\n");
        return 1;
    }
    
    // Set master volume
    if (master_volume != 1.0f) {
        audio_mixer_set_volume(g_mixer, -1, master_volume);
        printf("Master volume: %.1f%%\n", master_volume * 100.0f);
    }
    
    // Load and play tracks on channels
    printf("\nStarting parallel playback...\n");
    bool any_playing = false;
    for (int i = 0; i < 8; i++) {
        if (!channels[i].filename) continue;
        
        // Load sound
        Sound *sound = sound_load(channels[i].filename);
        if (!sound) {
            fprintf(stderr, "Warning: Failed to load sound for channel %d\n", i);
            continue;
        }
        
        PlaybackOptions options = {
            .loop = channels[i].loop,
            .volume = channels[i].volume
        };
        
        if (audio_mixer_play(g_mixer, i, sound, &options) == 0) {
            any_playing = true;
        } else {
            fprintf(stderr, "Warning: Failed to play channel %d\n", i);
            sound_destroy(sound);
        }
        // Note: sound is now owned by the mixer, don't destroy here
    }
    
    if (!any_playing) {
        fprintf(stderr, "Error: Failed to start mixer\n");
        audio_mixer_destroy(g_mixer);
        return 1;
    }
    
    printf("\nPlayback started!\n");
    printf("Commands: s <ch> (stop), l <ch> (stop loop), v <ch> <vol> (volume), i <ch> (info), q (quit)\n");
    printf("Use -1 for all channels (e.g., 's -1' stops all)\n\n");
    
    // Interactive command loop
    char cmd[256];
    int ch;
    float vol;
    bool prompt_shown = false;
    
    while (audio_mixer_is_playing(g_mixer)) {
        // Non-blocking check for input with timeout
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout
        
        // Only show prompt once until command is entered
        if (!prompt_shown) {
            printf("> ");
            fflush(stdout);
            prompt_shown = true;
        }
        
        if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) > 0) {
            if (fgets(cmd, sizeof(cmd), stdin)) {
                prompt_shown = false; // Reset for next command
                // Parse command
                char action;
                if (sscanf(cmd, " %c %d", &action, &ch) == 2) {
                    switch (action) {
                        case 's':
                        case 'S':
                            printf("Stopping channel %s\n", ch == -1 ? "all" : "");
                            audio_mixer_stop_channel(g_mixer, ch, STOP_IMMEDIATE);
                            break;
                            
                        case 'l':
                        case 'L':
                            printf("Disabling loop on channel %s\n", ch == -1 ? "all" : "");
                            audio_mixer_stop_looping(g_mixer, ch);
                            break;
                            
                        case 'i':
                        case 'I':
                            if (ch >= 0 && ch < 8) {
                                bool playing = audio_mixer_is_channel_playing(g_mixer, ch);
                                printf("Channel %d: %s\n", ch, playing ? "PLAYING" : "STOPPED");
                            } else {
                                printf("Invalid channel number (must be 0-7)\n");
                            }
                            break;
                            
                        default:
                            printf("Unknown command '%c'\n", action);
                            break;
                    }
                } else if (sscanf(cmd, " v %d %f", &ch, &vol) == 2) {
                    // Volume command: v <ch> <vol>
                    printf("Setting volume on channel %d to %.2f\n", ch, vol);
                    audio_mixer_set_volume(g_mixer, ch, vol);
                } else if (sscanf(cmd, " m %f", &vol) == 1) {
                    // Master volume: m <vol>
                    printf("Setting master volume to %.2f\n", vol);
                    audio_mixer_set_volume(g_mixer, -1, vol);
                } else if (cmd[0] == 'q' || cmd[0] == 'Q') {
                    printf("Quitting...\n");
                    break;
                } else if (cmd[0] == 'h' || cmd[0] == 'H' || cmd[0] == '?') {
                    printf("\nAvailable commands:\n");
                    printf("  s <ch>      - Stop channel (-1 for all)\n");
                    printf("  l <ch>      - Stop looping on channel (-1 for all)\n");
                    printf("  v <ch> <vol> - Set channel volume (0.0-1.0)\n");
                    printf("  m <vol>     - Set master volume (0.0-1.0)\n");
                    printf("  i <ch>      - Check if channel is playing (0-7)\n");
                    printf("  q           - Quit\n");
                    printf("  h or ?      - Show this help\n");
                } else if (strlen(cmd) > 1) {
                    printf("Invalid command. Type 'h' for help.\n");
                }
            }
        }
    }
    
    printf("\nAll channels finished.\n");
    
    // Cleanup
    audio_mixer_destroy(g_mixer);
    g_mixer = NULL;
    
    return 0;
}
