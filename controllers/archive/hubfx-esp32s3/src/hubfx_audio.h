/*
 * HubFX Audio — Mixer Type Alias & Channel Assignments
 *
 * Central definitions for the audio subsystem on HubFX ESP32-S3.
 * All feature modules reference this header for channel indices to avoid
 * hard-coded magic numbers and ensure channel assignments are visible in
 * one place.
 *
 * Channel allocation (8 available, 0-7):
 *   0  — System sounds (boot chime, alerts, UI feedback)
 *   1  — Engine A (crossfade pair, primary)
 *   2  — Engine B (crossfade pair, secondary)
 *   3  — Gun FX
 *   4  — Reserved (future: rotor FX / ambient)
 *   5  — Reserved
 *   6  — Reserved
 *   7  — Reserved
 */

#ifndef HUBFX_AUDIO_H
#define HUBFX_AUDIO_H

#include <cstring>
#include <audio/esp_i2s_output.h>
#include <codec/tas5825_m_codec.h>
#include <audio/audio_mixer.h>

// ============================================================================
// Concrete Mixer Type for this Platform
// ============================================================================

/// AudioMixer instantiation for ESP32-S3 + TAS5825M codec.
/// HubFX silicon is the M-variant (smart-amp / inductor-less Class-D).
/// For the P-variant (Class-H + Hybrid-Pro) use TAS5825PCodec.
using Mixer = AudioMixer<EspI2SOutput, TAS5825MCodec>;

// ============================================================================
// Channel Assignments
// ============================================================================

namespace HubFxChannel {
    constexpr int SYSTEM    = 0;   // Boot chime, alerts, UI feedback
    constexpr int ENGINE_A  = 1;   // Engine startup/running (crossfade primary)
    constexpr int ENGINE_B  = 2;   // Engine crossfade (secondary)
    constexpr int GUN       = 3;   // Gun fire / reload sounds
    constexpr int COUNT     = 8;   // Total mixer channels (AudioMixer MAX_CHANNELS)
}

// ============================================================================
// Output Channel Helpers
// ============================================================================

/**
 * @brief Convert a channel bitmask to human-readable string.
 *
 * Examples: "CH1", "CH2", "CH1+CH2", "none", "CH1+CH2+CH3" (future).
 *
 * @param channels  Bitmask of AudioChannel::CH1, CH2, etc.
 * @return Static string describing the active channels.
 */
inline const char* outputChannelsString(uint8_t channels) {
    if (channels == AudioChannel::ALL)  return "CH1+CH2";
    if (channels == AudioChannel::CH1)  return "CH1";
    if (channels == AudioChannel::CH2)  return "CH2";
    if (channels == 0)                  return "none";
    return "custom";  // Future: multi-channel bitmask beyond CH1+CH2
}

#endif // HUBFX_AUDIO_H
