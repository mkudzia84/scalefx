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

#include <audio/esp_i2s_output.h>
#include <codec/tas5825_codec.h>
#include <audio/audio_mixer.h>

// ============================================================================
// Concrete Mixer Type for this Platform
// ============================================================================

/// AudioMixer instantiation for ESP32-S3 + TAS5825M codec.
using Mixer = AudioMixer<EspI2SOutput, TAS5825Codec>;

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

#endif // HUBFX_AUDIO_H
