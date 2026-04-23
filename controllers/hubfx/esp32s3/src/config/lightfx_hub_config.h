/*
 * Light FX Hub Configuration — Schema for /lightfx.yaml on the hub
 *
 * The hub stores its master copy of LightProgramConfig in /lightfx.yaml on
 * its own flash, then pushes the loaded config to the USB-attached LightFX
 * slave via pushLightFxConfigToSlave(cfg, client) (hub-internal config-apply
 * — see instructions/12-LIGHTFX-MODERNIZATION.md §4.2).
 *
 * The data type is the shared LightProgramConfig (same struct as the
 * standalone LightFX firmware loads from its own /lightfx.yaml). Fields
 * sit at YAML root — no nested key — so the same Studio YAML emitter that
 * drives the standalone slave drives the hub copy too.
 *
 * Battery monitoring is intentionally NOT included here:
 *   - The hub's INA226 rail channel is hardcoded in hubfx_esp32s3.ino;
 *     chemistry / cell count come from the runtime BATTERY_CONFIG (0xEE)
 *     packet, not YAML.
 *   - Slave LightFX boards keep their own battery config in their own
 *     /lightfx.yaml on slave-side flash (LightFxConfig::battery there).
 *   - Battery is hardware-local — never pushed to the slave.
 */

#ifndef LIGHTFX_HUB_CONFIG_H
#define LIGHTFX_HUB_CONFIG_H

#include <config/light_program_config.h>
#include <config/yaml_schema.h>

// ============================================================================
// ConfigStore Schema Adapter — owns /lightfx.yaml on hub flash
// ============================================================================

struct HubLightFxConfigSchema {
    using DataType = LightProgramConfig;

    template<typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        return light_program_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return light_program_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/lightfx.yaml"; }
};

#endif // LIGHTFX_HUB_CONFIG_H
