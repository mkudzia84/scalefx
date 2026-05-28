/*
 * enginefx_config.h — `/enginefx.yaml` schema.
 *
 * Full EngineFx wiring + sound pack in its own file.  `/hubfx.yaml`'s
 * `enginefx.enabled:` flag overrides the `enabled` field here at apply
 * time — i.e. /hubfx.yaml owns the master enable matrix, /enginefx.yaml
 * owns the engine details.
 *
 * YAML shape:
 *
 *   schema_version: 1
 *   enabled: true
 *   type: turbine                       # turbine | radial | diesel (informational)
 *   toggle:
 *     input: engine_toggle              # named channel from /hubfx.yaml inputs[]
 *     threshold_us: 1500                # >= threshold ⇒ engine on
 *     hysteresis_us: 50
 *     failsafe: force_low               # hold | force_low | force_high (RC loss)
 *   output: both                        # left | right | both — stereo routing
 *   sounds:
 *     starting: "/sounds/KA50/engine_start.mp3"
 *     running:  "/sounds/KA50/engine_loop.mp3"
 *     stopping: "/sounds/KA50/engine_stop.mp3"
 *     transitions:
 *       starting_offset_ms: 60000
 *       stopping_offset_ms: 25000
 *
 * The mixer channels are board-fixed at `HubFxLayout::EngineA` /
 * `EngineB` — not configurable via YAML.  The `type:` field is
 * informational only (the state machine is sound-driven; the tag
 * exists for Studio's UI grouping).
 */

#ifndef HUBFX_ENGINEFX_CONFIG_H
#define HUBFX_ENGINEFX_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_schema.h>

#include "../effects/enginefx/enginefx_service.h"   // EngineFxConfig (firmware-side)
#include "../effects/audio_layout.h"                // HubFxLayout
#include "../effects/effect_id.h"                   // PortRef
#include "port_ref_yaml.h"                          // portRefFromNode

struct EngineFxYamlPool {
    static constexpr size_t MAX_NODES        = 96;
    static constexpr size_t STRING_POOL_SIZE = 2048;
    static constexpr size_t MAX_DEPTH        = 8;
};

/// Human-readable stereo routing for any audio effect.  Maps to the
/// `outputMask` byte the firmware audio mixer takes (bit 0 = left,
/// bit 1 = right).  Unknown strings map to `both` and log a WARN.
inline uint8_t audioOutputMaskFromName(const char* name) {
    if (!name || !name[0])              return 0x03;
    if (std::strcmp(name, "left")  == 0) return 0x01;
    if (std::strcmp(name, "right") == 0) return 0x02;
    if (std::strcmp(name, "both")  == 0) return 0x03;
    SFX_LOG_WARN("[enginefx] unknown output '%s' — using 'both'", name);
    return 0x03;
}

inline const char* audioOutputMaskName(uint8_t mask) {
    switch (mask & 0x03) {
        case 0x01: return "left";
        case 0x02: return "right";
        case 0x03: return "both";
        default:   return "none";
    }
}

/// Parsed form of `/enginefx.yaml`.  Translated to
/// `hubfx::effects::enginefx::EngineFxConfig` at apply time.  Mixer
/// channels are NOT in this struct — they're board-fixed in the
/// translator via `HubFxLayout::EngineA / EngineB`.
struct EngineFxYamlConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    bool     enabled = false;
    char     type[16] = "turbine";
    char     output[8]  = "both";     ///< "left" | "right" | "both"
    uint8_t  outputMask = 0x03;       ///< populated by populate() from `output`

    /// RC throttle toggle.  `input:` is a named channel from
    /// /hubfx.yaml's `inputs:` block — the resolver looks it up to get
    /// the source PortRef + channel id.  The threshold / hysteresis /
    /// failsafe behaviour are carried HERE, in the effect file, since
    /// they describe how THIS effect interprets the channel.  Empty
    /// `input` ⇒ no throttle trigger (manual ENGINE_START / STOP only).
    struct Toggle {
        char     input[kInputNameMax] = {};
        uint16_t thresholdUs          = 1500;
        uint16_t hysteresisUs         = 50;
        char     failsafe[16]         = "force_low";
    } toggle;

    struct Sounds {
        char starting[64] = {};
        char running [64] = {};
        char stopping[64] = {};

        struct Transitions {
            uint32_t startingOffsetMs = 0;
            uint32_t stoppingOffsetMs = 0;
            uint16_t startFadeInMs    = 0;
            uint16_t stopFadeOutMs    = 0;
            /// Cross-fade window between start→loop, loop→stop, and
            /// loop wraps.  The next track is launched on the OTHER
            /// engine mixer slot `crossfade_ms` before the current
            /// track ends, ramping in from 0 → 1 while the current
            /// ramps 1 → 0 over the same window.  Eliminates the
            /// per-transition audio gap caused by source teardown +
            /// drain-buffer pre-fill.  0 = legacy sequential play
            /// (hard cut between tracks).  Default 200 ms suits most
            /// engine packs; bump to 400-500 ms for very slow turbines.
            uint16_t crossfadeMs      = 200;
        } transitions;
    } sounds;
};

// ─── Declarative schema ─────────────────────────────────────────────

namespace enginefx_config_schema {

using namespace sfx;
using T  = EngineFxYamlConfig::Toggle;
using S  = EngineFxYamlConfig::Sounds;
using Tr = S::Transitions;

inline const auto fields = schema<EngineFxYamlConfig>(
    prop<&EngineFxYamlConfig::enabled>("enabled", false),
    prop<&EngineFxYamlConfig::type>   ("type",    "turbine"),
    prop<&EngineFxYamlConfig::output> ("output",  "both"),

    group<&EngineFxYamlConfig::toggle>("toggle",
        prop<&T::input>       ("input",         ""),
        prop<&T::thresholdUs> ("threshold_us",  uint16_t(1500)).range(800, 2200),
        prop<&T::hysteresisUs>("hysteresis_us", uint16_t(50)).range(0, 500),
        prop<&T::failsafe>    ("failsafe",      "force_low")
    ),

    group<&EngineFxYamlConfig::sounds>("sounds",
        prop<&S::starting>("starting", ""),
        prop<&S::running> ("running",  ""),
        prop<&S::stopping>("stopping", ""),

        group<&S::transitions>("transitions",
            prop<&Tr::startingOffsetMs>("starting_offset_ms", uint32_t(0)),
            prop<&Tr::stoppingOffsetMs>("stopping_offset_ms", uint32_t(0)),
            prop<&Tr::startFadeInMs>   ("start_fade_in_ms",   uint16_t(0)).range(0, 10000),
            prop<&Tr::stopFadeOutMs>   ("stop_fade_out_ms",   uint16_t(0)).range(0, 10000),
            prop<&Tr::crossfadeMs>     ("crossfade_ms",       uint16_t(200)).range(0, 2000)
        )
    )
);

}  // namespace enginefx_config_schema

// ─── ConfigStore adapter ────────────────────────────────────────────

struct EngineFxConfigSchema {
    using DataType = EngineFxYamlConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        if (!enginefx_config_schema::fields.populate(d, p.root())) return false;
        // Resolve the human-readable `output:` string into the byte mask
        // the audio mixer takes.  Single source of truth — `cfg.outputMask`
        // is what the apply translator reads.
        d.outputMask = audioOutputMaskFromName(d.output);
        return true;
    }
    static bool validate(const DataType& d, char* err, size_t errLen) {
        return enginefx_config_schema::fields.validate(d, err, errLen);
    }
    static const char* defaultPath() { return "/enginefx.yaml"; }
};

/// Translate the parsed YAML to a firmware-side `EngineFxConfig`.
/// `/hubfx.yaml`'s `inputs[]` supplies the source PortRef + channel id
/// for the named input; the trigger interpretation (threshold,
/// hysteresis, failsafe) comes from THIS file.  Unknown name ⇒ engine
/// starts with no throttle binding (only ENGINE_START / STOP work).
inline hubfx::effects::enginefx::EngineFxConfig
toEngineFxServiceConfig(const EngineFxYamlConfig& y, const HubFxConfig& hub) {
    using namespace hubfx::effects;
    enginefx::EngineFxConfig cfg;
    cfg.enabled      = y.enabled;
    cfg.channelA     = audio::HubFxLayout::EngineA;
    cfg.channelB     = audio::HubFxLayout::EngineB;
    cfg.outputMask   = y.outputMask;
    std::strncpy(cfg.startingPath, y.sounds.starting, sizeof(cfg.startingPath) - 1);
    std::strncpy(cfg.runningPath,  y.sounds.running,  sizeof(cfg.runningPath)  - 1);
    std::strncpy(cfg.stoppingPath, y.sounds.stopping, sizeof(cfg.stoppingPath) - 1);
    cfg.startingOffsetMs = y.sounds.transitions.startingOffsetMs;
    cfg.stoppingOffsetMs = y.sounds.transitions.stoppingOffsetMs;
    cfg.startFadeInMs    = y.sounds.transitions.startFadeInMs;
    cfg.stopFadeOutMs    = y.sounds.transitions.stopFadeOutMs;
    cfg.crossfadeMs      = y.sounds.transitions.crossfadeMs;

    // Trigger params live in /enginefx.yaml — copy through.
    cfg.thresholdUs  = y.toggle.thresholdUs;
    cfg.hysteresisUs = y.toggle.hysteresisUs;
    cfg.failsafe     = failsafeFromName(y.toggle.failsafe);

    // Resolve `toggle.input` against /hubfx.yaml's named inputs for
    // the source port + channel id.
    if (y.toggle.input[0]) {
        const InputBinding* b = findInputByName(hub, y.toggle.input);
        if (b) {
            cfg.rcInput      = b->port;
            cfg.inputChannel = (b->channelId > 0) ? (uint8_t)(b->channelId - 1) : 0;
        } else {
            SFX_LOG_WARN("[enginefx-config] toggle.input='%s' not in /hubfx.yaml "
                         "inputs[] — engine throttle disabled",
                         y.toggle.input);
            // Leave cfg.rcInput at default (portKind=0 → service skips
            // the trigger subscribe entirely).
        }
    }
    return cfg;
}

#endif  // HUBFX_ENGINEFX_CONFIG_H
