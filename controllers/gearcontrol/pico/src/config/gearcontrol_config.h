/*
 * GearControl Configuration — Placeholder Config Schema
 *
 * Minimal schema for standalone operation. Populate with real fields
 * when specific config settings are needed (gear calibration, servo
 * limits, channel enables, INA226 thresholds, etc.).
 *
 * YAML structure (top-level keys — extend as needed):
 *   # gearcontrol:
 *   #   gear_count: 3
 *   #   servo_defaults:
 *   #     min_us: 500
 *   #     max_us: 2500
 */

#ifndef GEARCONTROL_CONFIG_H
#define GEARCONTROL_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>

// ============================================================================
// Data Struct — extend with real fields as needed
// ============================================================================

struct GearControlConfig {
    // Placeholder — add fields here when standalone config is needed
    // Example future fields:
    // uint8_t gearCount = 3;
    // uint16_t servoMinUs = 500;
    // uint16_t servoMaxUs = 2500;
    // uint16_t stallCurrent_mA = 500;
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace gearcontrol_config {

using namespace sfx;

/// Field-level schema — add prop<> bindings as fields are added.
inline const auto fields = schema<GearControlConfig>(
    // Example: prop<&GearControlConfig::gearCount>("gear_count", uint8_t(3))
);

} // namespace gearcontrol_config

// ============================================================================
// ConfigStore Schema Adapter
// ============================================================================

struct GearControlConfigSchema {
    using DataType = GearControlConfig;

    static bool populate(DataType& d, const YamlParser<>& p) {
        return gearcontrol_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return gearcontrol_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/config.yaml"; }
};

#endif // GEARCONTROL_CONFIG_H
