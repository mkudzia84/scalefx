/*
 * HubFX Configuration — Top-Level Config Schema
 *
 * Encapsulates all section configs (engine, gun, light, gear, etc.)
 * into a single HubFxConfig struct with a composable declarative schema.
 *
 * Section schemas are embedded via asGroup() — each section maintains
 * its own data struct and field definitions independently, while the
 * hub schema composes them into the full document layout.
 *
 * YAML structure (top-level keys):
 *   engine_fx:   → EngineConfig      (engine_config.h)
 *   gun_fx:      → GunFxHubConfig    (gunfx_hub_config.h)
 *   # Future sections:
 *   # light_fx:  → LightFxConfig
 *   # gear_ctrl: → GearControlConfig
 */

#ifndef HUBFX_CONFIG_H
#define HUBFX_CONFIG_H

#include "audio_settings.h"
#include "engine_config.h"
#include "gunfx_hub_config.h"

// ============================================================================
// Data Struct
// ============================================================================

struct HubFxConfig {
    AudioConfig      audio;
    EngineConfig     engineFx;
    GunFxHubConfig   gunFx;

    // Future sections:
    // LightFxConfig     lightFx;
    // GearControlConfig gearCtrl;
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace hubfx_config {

using namespace sfx;

/// Hub-level schema — composes section schemas via asGroup().
inline const auto fields = schema<HubFxConfig>(
    audio_config::fields.asGroup<&HubFxConfig::audio>("audio"),
    engine_config::fields.asGroup<&HubFxConfig::engineFx>("engine_fx"),
    gunfx_hub_config::fields.asGroup<&HubFxConfig::gunFx>("gun_fx")

    // Future sections:
    // light_config::fields.asGroup<&HubFxConfig::lightFx>("light_fx"),
    // gear_config::fields.asGroup<&HubFxConfig::gearCtrl>("gear_ctrl")
);

} // namespace hubfx_config

// ============================================================================
// ConfigStore Schema Adapter
// ============================================================================

/**
 * @brief Schema adapter for ConfigStore<HubFxConfigSchema>.
 *
 * Populates the full HubFxConfig from the document root.
 * Each section schema navigates to its own YAML key automatically.
 */
struct HubFxConfigSchema {
    using DataType = HubFxConfig;

    static bool populate(DataType& d, const YamlParser<>& p) {
        return hubfx_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return hubfx_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/config.yaml"; }
};

#endif // HUBFX_CONFIG_H
