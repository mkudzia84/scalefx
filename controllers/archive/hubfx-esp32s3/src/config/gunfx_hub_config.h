/*
 * Gun FX Hub Configuration — Data Struct + Schema for /gunfx.yaml
 *
 * Per Rule 26, gun FX lives in its own top-level file on the hub flash.
 *
 * This config controls local audio playback of gun sounds on the hub.
 * Slave GunFX controllers handle their own firing logic; this config
 * determines how the hub plays gun audio effects (channel routing, etc.).
 *
 * YAML structure (at root of /gunfx.yaml):
 *   output_channels: all           # all | ch1 | ch2 | ch1+ch2
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

// ============================================================================
// ConfigStore Schema Adapter — owns /gunfx.yaml
// ============================================================================

struct GunFxHubConfigSchema {
    using DataType = GunFxHubConfig;

    template<typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        return gunfx_hub_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return gunfx_hub_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/gunfx.yaml"; }
};

#endif // GUNFX_HUB_CONFIG_H
