/*
 * Engine FX Configuration — Data Struct + Schema for /enginefx.yaml
 *
 * Per Rule 26, engine FX lives in its own top-level file on the hub flash.
 *
 * YAML structure (at root of /enginefx.yaml):
 *   enabled: true
 *   type: turbine
 *   output_channels: all           # all | ch1 | ch2 | ch1+ch2
 *   engine_toggle:
 *     input_channel: 1
 *     threshold_us: 1500
 *   sounds:
 *     starting: "/sounds/ka50/engine_start.wav"
 *     running:  "/sounds/ka50/engine_loop.wav"
 *     stopping: "/sounds/ka50/engine_stop.wav"
 *     transitions:
 *       starting_offset_ms: 60000
 *       stopping_offset_ms: 25000
 */

#ifndef ENGINE_CONFIG_H
#define ENGINE_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>
#include "audio_output_convert.h"

// ============================================================================
// Data Struct
// ============================================================================

struct EngineConfig {
    bool        enabled = true;
    char        type[16] = "turbine";              // turbine | radial | diesel
    OutputChannelMask outputChannels;               // all | ch1 | ch2 | ch1+ch2

    /// engine_toggle sub-section
    struct Toggle {
        uint8_t  inputChannel = 1;         // PWM input channel (1-10)
        uint16_t threshold_us = 1500;      // On/off threshold (µs)
    } toggle;

    /// sounds sub-section
    struct Sounds {
        char starting[64] = "";
        char running[64]  = "";
        char stopping[64] = "";

        /// sounds.transitions sub-section
        struct Transitions {
            uint32_t startingOffset_ms = 60000;   // Offset when restarting from stopping
            uint32_t stoppingOffset_ms = 25000;   // Offset when stopping from starting
        } transitions;
    } sounds;
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace engine_config {

using namespace sfx;
using T  = EngineConfig::Toggle;
using S  = EngineConfig::Sounds;
using Tr = S::Transitions;

/// Field-level schema for the engine_fx YAML section.
/// Provides auto-generated populate() and validate().
inline const auto fields = schema<EngineConfig>(
    prop<&EngineConfig::enabled>        ("enabled",         true),
    prop<&EngineConfig::type>           ("type",            "turbine"),
    prop<&EngineConfig::outputChannels> ("output_channels", OutputChannelMask{AudioChannel::ALL}),

    group<&EngineConfig::toggle>("engine_toggle",
        prop<&T::inputChannel>("input_channel", uint8_t(1)).range(1, 10),
        prop<&T::threshold_us>("threshold_us",  uint16_t(1500)).range(800, 2200)
    ),

    group<&EngineConfig::sounds>("sounds",
        prop<&S::starting>("starting", ""),
        prop<&S::running> ("running",  ""),
        prop<&S::stopping>("stopping", ""),

        group<&S::transitions>("transitions",
            prop<&Tr::startingOffset_ms>("starting_offset_ms", uint32_t(60000)),
            prop<&Tr::stoppingOffset_ms>("stopping_offset_ms", uint32_t(25000))
        )
    )
);

} // namespace engine_config

// ============================================================================
// ConfigStore Schema Adapter — owns /enginefx.yaml
// ============================================================================

struct EngineConfigSchema {
    using DataType = EngineConfig;

    template<typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        return engine_config::fields.populate(d, p.root());
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        return engine_config::fields.validate(d, err, errLen);
    }

    static const char* defaultPath() { return "/enginefx.yaml"; }
};

#endif // ENGINE_CONFIG_H
