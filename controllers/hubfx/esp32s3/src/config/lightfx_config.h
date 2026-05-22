/*
 * lightfx_config.h — `/lightfx.yaml` schema.
 *
 * Full LightFx settings in its own file: master brightness + explicit
 * list of program file references + optional RC program selector.
 * `/hubfx.yaml`'s `features.lightfx:` flag is the master kill-switch
 * (overrides the `enabled:` field here).
 *
 * YAML shape:
 *
 *   schema_version: 1
 *   enabled: true
 *   master_brightness_pct: 100
 *   programs:                              # explicit LittleFS paths
 *     - /lightfx/programs/helicopter_off.yaml
 *     - /lightfx/programs/helicopter_flight.yaml
 *
 *   # Optional: drive program selection from an RC channel.  Each
 *   # range maps a PWM µs band to a program name (must match an entry
 *   # from `programs:` above).  Hysteresis keeps the selector stable
 *   # when the stick sits near a range boundary.
 *   program_selector:
 *     input: program_select               # named channel from /hubfx.yaml inputs[]
 *     hysteresis_us: 50
 *     ranges:
 *       - { from_us:  900, to_us: 1200, program: helicopter_off }
 *       - { from_us: 1200, to_us: 1500, program: helicopter_nav }
 *       - { from_us: 1500, to_us: 1800, program: helicopter_flight }
 *       - { from_us: 1800, to_us: 2100, program: helicopter_landing }
 *
 * Each entry in `programs:` is a FULL path opened verbatim at apply
 * time.  The program's `name` field comes from the basename minus
 * `.yaml`.  See `lightfx_program_loader.h` for the per-program parser.
 */

#ifndef HUBFX_LIGHTFX_CONFIG_H
#define HUBFX_LIGHTFX_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <config/yaml_schema.h>
#include <serial/diag_log.h>

constexpr uint8_t  kMaxProgramRefs       = 8;
constexpr size_t   kProgramPathMax       = 64;   ///< full LittleFS path, e.g. "/lightfx/programs/helicopter_off.yaml"
constexpr uint8_t  kMaxSelectorRanges    = 8;
constexpr size_t   kSelectorProgramMax   = 24;   ///< program name length (matches Program::name)
constexpr size_t   kSelectorInputNameMax = 24;

struct LightFxYamlPool {
    static constexpr size_t MAX_NODES        = 192;
    static constexpr size_t STRING_POOL_SIZE = 3072;
    static constexpr size_t MAX_DEPTH        = 10;
};

/// One `(from_us, to_us, program)` row of a program selector.
struct ProgramSelectorRange {
    uint16_t fromUs = 0;
    uint16_t toUs   = 0;
    char     program[kSelectorProgramMax] = {};
};

/// Range-based program selector.  When `enabled` is true and `input`
/// resolves to a known channel in /hubfx.yaml's inputs[], the LightFx
/// service subscribes a Raw-µs TriggerInput to it and switches the
/// active program whenever the current µs falls into a different range.
struct ProgramSelector {
    bool                 enabled      = false;
    char                 input[kSelectorInputNameMax] = {};
    uint16_t             hysteresisUs = 50;
    ProgramSelectorRange ranges[kMaxSelectorRanges]   = {};
    uint8_t              numRanges    = 0;
};

/// Parsed form of `/lightfx.yaml`.
struct LightFxYamlConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    bool    enabled              = true;
    uint8_t masterBrightnessPct  = 100;
    char    programPaths[kMaxProgramRefs][kProgramPathMax] = {};
    uint8_t numPrograms          = 0;

    ProgramSelector programSelector;
};

// ─── Declarative schema ─────────────────────────────────────────────

namespace lightfx_config_schema {

using namespace sfx;

inline const auto fields = schema<LightFxYamlConfig>(
    prop<&LightFxYamlConfig::enabled>            ("enabled",               true),
    prop<&LightFxYamlConfig::masterBrightnessPct>("master_brightness_pct", uint8_t(100)).range(0, 100)
);

}  // namespace lightfx_config_schema

// ─── ConfigStore adapter ────────────────────────────────────────────

struct LightFxConfigSchema {
    using DataType = LightFxYamlConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        if (!lightfx_config_schema::fields.populate(d, p.root())) return false;

        const auto* root = p.root();

        // `programs:` is a sequence of scalar paths — the DSL doesn't
        // support that, so walk it by hand.  Each entry is the FULL
        // LittleFS path of a per-program YAML, opened verbatim at apply
        // time.
        d.numPrograms = 0;
        const auto* progs = root ? root->child("programs") : nullptr;
        if (progs && progs->type == YamlNode::Sequence) {
            const int n = progs->childCount();
            for (int i = 0; i < n && d.numPrograms < kMaxProgramRefs; ++i) {
                const auto* item = progs->childAt(i);
                if (!item || item->type != YamlNode::Scalar) continue;
                const char* path = item->template as<const char*>("");
                if (!path || !path[0]) continue;
                std::strncpy(d.programPaths[d.numPrograms], path, kProgramPathMax - 1);
                d.programPaths[d.numPrograms][kProgramPathMax - 1] = '\0';
                d.numPrograms++;
            }
        }

        // `program_selector:` — optional block with input name +
        // hysteresis + ranges[] (each {from_us, to_us, program}).
        d.programSelector = ProgramSelector{};
        const auto* sel = root ? root->child("program_selector") : nullptr;
        if (sel) {
            const char* inp = sel->template childAs<const char*>("input", "");
            if (inp && inp[0]) {
                std::strncpy(d.programSelector.input, inp,
                             sizeof(d.programSelector.input) - 1);
            }
            d.programSelector.hysteresisUs =
                (uint16_t)sel->template childAs<int32_t>("hysteresis_us", 50);

            const auto* rangesNode = sel->child("ranges");
            if (rangesNode && rangesNode->type == YamlNode::Sequence) {
                const int n = rangesNode->childCount();
                for (int i = 0; i < n && d.programSelector.numRanges < kMaxSelectorRanges; ++i) {
                    const auto* item = rangesNode->childAt(i);
                    if (!item) continue;
                    auto& r = d.programSelector.ranges[d.programSelector.numRanges];
                    r.fromUs = (uint16_t)item->template childAs<int32_t>("from_us", 0);
                    r.toUs   = (uint16_t)item->template childAs<int32_t>("to_us",   0);
                    const char* prog = item->template childAs<const char*>("program", "");
                    if (!prog || !prog[0]) {
                        SFX_LOG_WARN("[lightfx-config] program_selector.ranges[%d]: "
                                     "missing `program`", i);
                        continue;
                    }
                    std::strncpy(r.program, prog, sizeof(r.program) - 1);
                    d.programSelector.numRanges++;
                }
            }
            // Treat as enabled once we have an input + at least one range.
            d.programSelector.enabled =
                d.programSelector.input[0] && d.programSelector.numRanges > 0;
        }
        return true;
    }
    static bool validate(const DataType& d, char* err, size_t errLen) {
        return lightfx_config_schema::fields.validate(d, err, errLen);
    }
    static const char* defaultPath() { return "/lightfx.yaml"; }
};

#endif  // HUBFX_LIGHTFX_CONFIG_H
