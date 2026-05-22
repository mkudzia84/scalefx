/*
 * hubfx_config.h — `/hubfx.yaml` schema (board master file).
 *
 * `/hubfx.yaml` carries everything that describes THE BOARD itself —
 * not any specific effect.  Three top-level blocks:
 *
 *   audio:     codec rail voltage (board-level hardware setting)
 *   features:  master enable matrix (kill-switches for every effect)
 *   ports:     port → role attachment + human-readable labels
 *
 * Effect details (wiring, sounds, programs, severity catalogs) live in
 * dedicated sub-config files at canonical paths:
 *
 *   /hubfx.yaml      ← THIS FILE (board: audio + features + port roles)
 *     ├── /alerts.yaml         (severity → AlertSound + volume)
 *     ├── /enginefx.yaml       (engine wiring + sound pack)
 *     ├── /landing.yaml        (landing-light defs)
 *     └── /lightfx.yaml        (master brightness + program path list)
 *                ↳ /lightfx/programs/<name>.yaml   (one file per program,
 *                                                   referenced explicitly
 *                                                   by full path)
 *
 * Boot order: this file loads first (its `ports:` block attaches role
 * variants to physical ports) → every sub-file loads from its canonical
 * path (effect configures via PortRefs, never touches role attach) →
 * this file re-applies so the master matrix has the last word over each
 * sub-file's local `enabled:` flag.
 *
 * Defaults reflect "a board with no YAML on flash still boots
 * functional": when `ports[]` is empty, every PWM port auto-attaches
 * `LedAnimator` (the most common case for HubFX), every servo port
 * gets `ServoActuator`, inputs stay None.
 *
 * See `instructions/19-HUBFX-CONFIG-SCHEMA.md` for the full file graph.
 */

#ifndef HUBFX_CONFIG_H
#define HUBFX_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <config/yaml_schema.h>
#include <serial/diag_log.h>
#include <serial/ports.h>             // PortKind::*
#include <serial/roles.h>             // RoleKind::*

#include "../effects/effect_id.h"     // PortRef
#include "../effects/input/trigger_input.h"   // TriggerKind, FailsafeBehaviour
#include "port_ref_yaml.h"

// YAML pool — sized for audio + features + ports[] + inputs[].
struct HubFxYamlPool {
    static constexpr size_t MAX_NODES        = 320;
    static constexpr size_t STRING_POOL_SIZE = 3072;
    static constexpr size_t MAX_DEPTH        = 8;
};

constexpr uint8_t kMaxPortMappings  = 24;   ///< 8 PWM + 11 servo + 1 input + headroom for expanders
constexpr size_t  kPortLabelMax     = 32;
constexpr uint8_t kMaxInputBindings = 16;   ///< matches InputDispatcher::kMaxBindings
constexpr size_t  kInputNameMax     = 24;

// ─── Data struct ────────────────────────────────────────────────────

/// One entry in the board's port → role mapping table.  Hub-local ports
/// only (no guid field) — expander ports are out of scope until the
/// expander config file lands.
struct PortMapping {
    uint8_t kind = 0;                    ///< PortKind::* (servo|pwm|hbridge|input)
    uint8_t idx  = 0;                    ///< per-kind index on the board
    uint8_t role = 0;                    ///< RoleKind::* — the variant to attach
    char    label[kPortLabelMax] = {};   ///< human-readable description (Studio uses this)
};

/// One logical RC input channel.  Just names a `(port, channel_id)`
/// tuple — the trigger TYPE and RANGE/THRESHOLD mapping live in the
/// EFFECT that uses the channel (`/enginefx.yaml`, `/lightfx.yaml`),
/// since different effects can interpret the same channel differently
/// (one as a Boolean toggle, another as a ProportionalU8, etc.).
///
/// The `port` field is the source input port — defaults to hub-local
/// IN_1 (`{Input, 0}`) for the single-input HubFX rev.  `channelId` is
/// 1-based for user friendliness; the resolver subtracts 1 when
/// passing the channel index to the dispatcher (0-based internally).
struct InputBinding {
    char    name[kInputNameMax] = {};
    hubfx::effects::PortRef port =
        hubfx::effects::PortRef::local(PortKind::Input, 0);
    uint8_t channelId       = 1;          ///< 1-based channel id in the source protocol
    char    description[48] = {};         ///< optional — Studio UI / docs only
};

// ─── Failsafe name helper (used by effect configs) ──────────────────

inline uint8_t failsafeFromName(const char* name) {
    using F = hubfx::effects::input::FailsafeBehaviour;
    if (!name || !name[0])                          return (uint8_t)F::ForceLow;
    if (std::strcmp(name, "hold")       == 0)       return (uint8_t)F::Hold;
    if (std::strcmp(name, "force_low")  == 0 ||
        std::strcmp(name, "force-low")  == 0)       return (uint8_t)F::ForceLow;
    if (std::strcmp(name, "force_high") == 0 ||
        std::strcmp(name, "force-high") == 0)       return (uint8_t)F::ForceHigh;
    SFX_LOG_WARN("[hubfx-config] unknown failsafe '%s' — using force_low", name);
    return (uint8_t)F::ForceLow;
}

struct HubFxConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    /// System audio block — TAS5825P PVDD-rail selector.  Picked up at
    /// boot before audio.begin() via the CodecAdapter trait.  Lives in
    /// /hubfx.yaml (not /alerts.yaml or its own /audio.yaml) because
    /// it's a BOARD-LEVEL hardware setting, not an effect-level one.
    struct AudioBlock {
        char codecSupply[8] = "12v";          ///< 12v | 15v | 20v | 24v
    } audio;

    /// Master enable matrix.  Each flag is the kill-switch for one
    /// effect family; setting it false overrides whatever the matching
    /// sub-file says about its own enabled state.  The runtime-
    /// capability walker on `BoardServer` picks these up via each
    /// service's `enabled()` accessor so the host's CLI / Studio view
    /// of which features are live tracks the YAML.
    struct FeaturesBlock {
        bool alerts        = true;
        bool enginefx      = false;
        bool landingLights = true;
        bool lightfx       = true;
        bool gears         = true;
        bool gunfx         = false;
    } features;

    /// Port → role mapping.  Populated by the YAML's `ports:` sequence;
    /// each entry attaches one `RoleKind` variant to one hub-local port.
    /// Empty table ⇒ fallback to "LedAnimator on every PWM port" in the
    /// apply path so an out-of-the-box HubFX rev with no /hubfx.yaml
    /// still drives its LEDs.
    PortMapping ports[kMaxPortMappings] = {};
    uint8_t     numPorts = 0;

    /// Named RC input channels.  Populated by the YAML's `inputs:`
    /// sequence; effects reference inputs by `name` and the apply path
    /// resolves them to (port, channel, TriggerMapping) here.  An empty
    /// table leaves every effect's input ref unresolved (engine throttle
    /// stays inert, etc.) — the matching service logs a WARN and falls
    /// back to no-trigger.
    InputBinding inputs[kMaxInputBindings] = {};
    uint8_t      numInputs = 0;

    /// Aggregated live-state telemetry.  When enabled, the sketch's
    /// loop tick emits one-line snapshots every `interval_ms` via the
    /// DiagLog wire stream — prefixed with `[T-in]` / `[T-out]` so a
    /// CLI subscriber or Studio dashboard can parse them.  Off by
    /// default; flip at runtime by editing /hubfx.yaml + config-reload.
    struct TelemetryBlock {
        bool     inputs     = false;     ///< emit RC channel value snapshots
        bool     outputs    = false;     ///< emit PWM duty snapshots (hub-local)
        uint16_t intervalMs = 250;       ///< snapshot period (4 Hz default)
    } telemetry;
};

/// Look up a named input.  Returns nullptr if no entry with `name`
/// exists in `cfg.inputs[]`.  Case-sensitive.
inline const InputBinding* findInputByName(const HubFxConfig& cfg, const char* name) {
    if (!name || !name[0]) return nullptr;
    for (uint8_t i = 0; i < cfg.numInputs; ++i) {
        if (std::strcmp(cfg.inputs[i].name, name) == 0) return &cfg.inputs[i];
    }
    return nullptr;
}

// ─── Port + role name helpers (snake_case / kebab-case → enum) ──────

inline uint8_t hubfxPortKindFromName(const char* name) {
    if (!name) return 0;
    if (std::strcmp(name, "servo")   == 0) return PortKind::Servo;
    if (std::strcmp(name, "pwm")     == 0) return PortKind::Pwm;
    if (std::strcmp(name, "hbridge") == 0) return PortKind::HBridge;
    if (std::strcmp(name, "input")   == 0) return PortKind::Input;
    return 0;
}

/// Accept both snake_case (`led_animator`) and kebab-case
/// (`led-animator`) — kebab is what `RoleKind::getName()` returns; snake
/// matches the rest of the YAML convention.  Unknown ⇒ `None` + WARN.
inline uint8_t hubfxRoleKindFromName(const char* name) {
    if (!name || !name[0])                                return RoleKind::None;
    if (std::strcmp(name, "none")              == 0)      return RoleKind::None;
    if (std::strcmp(name, "servo_actuator")    == 0 ||
        std::strcmp(name, "servo-actuator")    == 0)      return RoleKind::ServoActuator;
    if (std::strcmp(name, "rc_pwm_input")      == 0 ||
        std::strcmp(name, "rc-pwm-input")      == 0)      return RoleKind::RcPwmInput;
    if (std::strcmp(name, "sbus_input")        == 0 ||
        std::strcmp(name, "sbus-input")        == 0)      return RoleKind::SbusInput;
    if (std::strcmp(name, "jeti_ex_input")     == 0 ||
        std::strcmp(name, "jeti-ex-input")     == 0)      return RoleKind::JetiExInput;
    if (std::strcmp(name, "led_animator")      == 0 ||
        std::strcmp(name, "led-animator")      == 0)      return RoleKind::LedAnimator;
    if (std::strcmp(name, "dc_motor")          == 0 ||
        std::strcmp(name, "dc-motor")          == 0)      return RoleKind::DcMotor;
    if (std::strcmp(name, "heater")            == 0)      return RoleKind::Heater;
    if (std::strcmp(name, "bi_dc_motor")       == 0 ||
        std::strcmp(name, "bi-dc-motor")       == 0)      return RoleKind::BiDcMotor;
    SFX_LOG_WARN("[hubfx-config] unknown port role '%s' — using none", name);
    return RoleKind::None;
}

// ─── Declarative schema ─────────────────────────────────────────────

namespace hubfx_config_schema {

using namespace sfx;

using Aud   = HubFxConfig::AudioBlock;
using Feat  = HubFxConfig::FeaturesBlock;
using Telem = HubFxConfig::TelemetryBlock;

inline const auto fields = schema<HubFxConfig>(

    group<&HubFxConfig::audio>("audio",
        prop<&Aud::codecSupply>("codec_supply", "12v")
    ),

    group<&HubFxConfig::features>("features",
        prop<&Feat::alerts>       ("alerts",         true),
        prop<&Feat::enginefx>     ("enginefx",       false),
        prop<&Feat::landingLights>("landing_lights", true),
        prop<&Feat::lightfx>      ("lightfx",        true),
        prop<&Feat::gears>        ("gears",          true),
        prop<&Feat::gunfx>        ("gunfx",          false)
    ),

    group<&HubFxConfig::telemetry>("telemetry",
        prop<&Telem::inputs>    ("inputs",      false),
        prop<&Telem::outputs>   ("outputs",     false),
        prop<&Telem::intervalMs>("interval_ms", uint16_t(250)).range(50, 5000)
    )
);

}  // namespace hubfx_config_schema

// ─── ConfigStore adapter ────────────────────────────────────────────

struct HubFxConfigSchema {
    using DataType = HubFxConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        if (!hubfx_config_schema::fields.populate(d, p.root())) return false;

        // `ports:` is a sequence of maps — the DSL handles sequences of
        // map items but the resulting wire-up clashes with the nested
        // string parsing, so walk it by hand for consistency with the
        // other sub-config loaders (lightfx_config / landing_config).
        d.numPorts = 0;
        const auto* root = p.root();
        const auto* portsNode = root ? root->child("ports") : nullptr;
        if (portsNode && portsNode->type == YamlNode::Sequence) {
            const int n = portsNode->childCount();
            for (int i = 0; i < n && d.numPorts < kMaxPortMappings; ++i) {
                const auto* item = portsNode->childAt(i);
                if (!item) continue;
                PortMapping& m = d.ports[d.numPorts];
                const uint8_t k = hubfxPortKindFromName(
                    item->template childAs<const char*>("kind", ""));
                if (k == 0) {
                    SFX_LOG_WARN("[hubfx-config] ports[%d]: missing or unknown `kind`", i);
                    continue;
                }
                m.kind = k;
                m.idx  = (uint8_t)item->template childAs<int32_t>("idx", 0);
                m.role = hubfxRoleKindFromName(
                    item->template childAs<const char*>("role", "none"));
                const char* lbl = item->template childAs<const char*>("label", "");
                std::memset(m.label, 0, sizeof(m.label));
                if (lbl && lbl[0]) std::strncpy(m.label, lbl, sizeof(m.label) - 1);
                d.numPorts++;
            }
        }

        // `inputs:` is a sequence of maps — same story: direct traversal.
        // Each entry names one logical RC channel (name + id + optional
        // port + optional description).  Trigger TYPE and RANGE / THRESHOLD
        // mapping live in the EFFECT that consumes the channel, since
        // different effects can interpret the same channel differently.
        d.numInputs = 0;
        const auto* inputsNode = root ? root->child("inputs") : nullptr;
        if (inputsNode && inputsNode->type == YamlNode::Sequence) {
            const int n = inputsNode->childCount();
            for (int i = 0; i < n && d.numInputs < kMaxInputBindings; ++i) {
                const auto* item = inputsNode->childAt(i);
                if (!item) continue;
                InputBinding& b = d.inputs[d.numInputs];

                const char* nm = item->template childAs<const char*>("name", "");
                if (!nm || !nm[0]) {
                    SFX_LOG_WARN("[hubfx-config] inputs[%d]: missing `name`", i);
                    continue;
                }
                std::memset(b.name, 0, sizeof(b.name));
                std::strncpy(b.name, nm, sizeof(b.name) - 1);

                b.channelId = (uint8_t)item->template childAs<int32_t>("id", 1);

                // Optional `port:` map — defaults to hub-local IN_1.
                const auto* portNode = item->child("port");
                if (portNode) {
                    b.port = hubfx::config::portRefFromNode(portNode);
                }

                const char* desc = item->template childAs<const char*>("description", "");
                std::memset(b.description, 0, sizeof(b.description));
                if (desc && desc[0]) {
                    std::strncpy(b.description, desc, sizeof(b.description) - 1);
                }
                d.numInputs++;
            }
        }
        return true;
    }
    static bool validate(const DataType& d, char* err, size_t errLen) {
        return hubfx_config_schema::fields.validate(d, err, errLen);
    }
    static const char* defaultPath() { return "/hubfx.yaml"; }
};

#endif  // HUBFX_CONFIG_H
