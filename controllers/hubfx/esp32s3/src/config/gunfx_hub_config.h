/*
 * Gun FX Hub Configuration — Data Struct + Declarative Schema
 *
 * Maps the gun_fx section of config.yaml to a C++ struct.
 * Uses sfx::schema DSL for automatic populate() / validate().
 *
 * This config controls local audio playback of gun sounds on the hub.
 * Slave GunFX controllers handle their own firing logic; this config
 * determines how the hub plays gun audio effects (channel routing, etc.).
 *
 * YAML structure:
 *   gun_fx:
 *     output_channels: all           # all | ch1 | ch2 | ch1+ch2
 */

#ifndef GUNFX_HUB_CONFIG_H
#define GUNFX_HUB_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>
#include "audio_output_convert.h"

// ============================================================================
// Data Struct
// ============================================================================

struct GunFxHubConfig {
    OutputChannelMask outputChannels;              // all | ch1 | ch2 | ch1+ch2
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace gunfx_hub_config {

using namespace sfx;

/// Field-level schema for the gun_fx YAML section.
inline const auto fields = schema<GunFxHubConfig>(
    prop<&GunFxHubConfig::outputChannels>("output_channels", OutputChannelMask{AudioChannel::ALL})
);

} // namespace gunfx_hub_config

#endif // GUNFX_HUB_CONFIG_H
