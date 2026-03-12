/**
 * Audio Channel Assignments
 * 
 * Defines the channel allocation for the 8-channel audio mixer.
 * 
 * Channel Layout:
 *   0-1: Engine FX (two channels for crossfading)
 *   2:   Gun FX (single channel with queuing)
 *   3:   Reserved
 *   4:   Reserved
 *   5:   Reserved
 *   6:   Reserved
 *   7:   System sounds
 */

#ifndef AUDIO_CHANNELS_H
#define AUDIO_CHANNELS_H

namespace AudioChannels {
    // ---- Engine FX (two channels for crossfading) ----
    constexpr int ENGINE_A         = 0;  // Primary engine channel
    constexpr int ENGINE_B         = 1;  // Secondary for crossfade
    
    // ---- Gun FX (single channel with queuing) ----
    constexpr int GUN              = 2;
    
    // ---- Reserved ----
    constexpr int RESERVED_1       = 3;
    constexpr int RESERVED_2       = 4;
    constexpr int RESERVED_3       = 5;
    constexpr int RESERVED_4       = 6;
    
    // ---- System Sounds ----
    constexpr int SYSTEM           = 7;
}

#endif // AUDIO_CHANNELS_H
