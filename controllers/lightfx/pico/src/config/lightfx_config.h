/*
 * LightFX Configuration — Placeholder Config Schema
 *
 * Minimal schema for standalone operation. Populate with real fields
 * when specific config settings are needed (LED defaults, servo limits,
 * landing light bindings, etc.).
 *
 * YAML structure (top-level keys — extend as needed):
 *   # lightfx:
 *   #   master_brightness: 255
 *   #   servo_defaults:
 *   #     min_us: 500
 *   #     max_us: 2500
 */

#ifndef LIGHTFX_CONFIG_H
#define LIGHTFX_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>

// ============================================================================
// Data Struct — extend with real fields as needed
// ============================================================================

struct LightFxConfig {
    // Placeholder — add fields here when standalone config is needed
    // Example future fields:
    // uint8_t masterBrightness = 255;
    // uint16_t servoMinUs = 500;
    // uint16_t servoMaxUs = 2500;
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace lightfx_config {

using namespace sfx;

/// Field-level schema — add prop<> bindings as fields are added.
inline const auto fields = schema<LightFxConfig>(
    // Example: prop<&LightFxConfig::masterBrightness>("master_brightness", uint8_t(255))
);

} // namespace lightfx_config

// ============================================================================
// ConfigStore Schema Adapter
// ============================================================================

struct LightFxConfigSchema {
    using DataType = LightFxConfig;

    static bool populate(DataType& d, const YamlParser<>& p) {
        return lightfx_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return lightfx_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/lightfx.yaml"; }
};

#endif // LIGHTFX_CONFIG_H
