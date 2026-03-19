/*
 * Engine FX Configuration — Data Struct + Declarative Schema
 *
 * Maps the engine_fx section of config.yaml to a C++ struct.
 * Uses sfx::schema DSL for automatic populate() / validate().
 *
 * YAML structure:
 *   engine_fx:
 *     enabled: true
 *     type: turbine
 *     engine_toggle:
 *       input_channel: 1
 *       threshold_us: 1500
 *     sounds:
 *       starting: "/sounds/ka50/engine_start.wav"
 *       running:  "/sounds/ka50/engine_loop.wav"
 *       stopping: "/sounds/ka50/engine_stop.wav"
 *       transitions:
 *         starting_offset_ms: 60000
 *         stopping_offset_ms: 25000
 */

#ifndef ENGINE_CONFIG_H
#define ENGINE_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>

// ============================================================================
// Data Struct
// ============================================================================

struct EngineConfig {
    bool    enabled = true;
    char    type[16] = "turbine";          // turbine | radial | diesel

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
    prop<&EngineConfig::enabled>("enabled", true),
    prop<&EngineConfig::type>   ("type",    "turbine"),

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

#endif // ENGINE_CONFIG_H
