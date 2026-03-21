/*
 * Audio Configuration — Data Struct + Declarative Schema
 *
 * Maps the audio section of config.yaml to a C++ struct.
 * Uses sfx::schema DSL for automatic populate() / validate().
 *
 * YAML structure:
 *   audio:
 *     codec_supply_voltage: "12v"    # 12v | 15v | 20v | 24v
 */

#ifndef AUDIO_SETTINGS_H
#define AUDIO_SETTINGS_H

#include <cstdint>
#include <cstring>
#include <config/yaml_schema.h>

// ============================================================================
// Data Struct
// ============================================================================

struct AudioConfig {
    /// TAS5825M supply voltage: "12v", "15v", "20v", "24v"
    /// Must match the physical PVDD supply — controls analog gain (output swing).
    /// Default: "12v" (3S LiPo). Changing this at runtime reconfigures analog gain.
    char codecSupplyVoltage[4] = "12v";
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace audio_config {

using namespace sfx;

/// Field-level schema for the audio YAML section.
inline const auto fields = schema<AudioConfig>(
    prop<&AudioConfig::codecSupplyVoltage>("codec_supply_voltage", "12v")
);

} // namespace audio_config

#endif // AUDIO_SETTINGS_H
