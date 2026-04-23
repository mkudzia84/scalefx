/*
 * LightFX Configuration — Per-Device Schema (LiPo battery + master brightness)
 *
 * Stored in /lightfx.yaml on the LightFX Pico's LittleFS flash. Studio reads
 * and writes this file directly via the FILE_* protocol; the firmware loads
 * it once at boot through ConfigServerT and re-applies on `config.reload`.
 *
 * Sections:
 *   master_brightness — global LED gain (0-100%)
 *   battery           — chemistry / cell count / auto-cutoff safety reaction
 *
 * YAML structure:
 *   master_brightness: 100         # 0-100, applied via LED_MASTER_BRIGHTNESS
 *
 *   battery:
 *     auto_cutoff: true            # Disable all LED channels on low voltage
 *     chemistry:   lipo            # lipo|liion|nimh — voltage monitor is always on
 *     cell_count:  0               # 0 = auto-detect from voltage
 */

#ifndef LIGHTFX_CONFIG_H
#define LIGHTFX_CONFIG_H

#include <cstdint>
#include <config/yaml_schema.h>
#include <config/light_program_config.h>

// ============================================================================
// YAML Parser Pool — board-local preset (transient allocation)
// ============================================================================
//
// The firmware schema below only consumes a handful of keys, but Studio writes
// a much richer YAML (programs × channels × events, landing groups, servo
// bindings) into /lightfx.yaml. The parser still has to allocate a node for
// every key and sequence item while scanning, even though most are ignored at
// populate() time. The default 128-node pool exhausts on real-world Studio
// output around line 100; the previous 256-node bump still leaves no headroom
// for users adding programs, channels, or per-channel events.
//
// Memory is fully **transient**: ConfigStore::loadFromString creates a
// YamlParser<TPool> on the stack, parse() heap-allocates the node + string
// pools via `new[]`, and the parser destructor (~YamlParser → reset →
// delete[]) frees them as soon as loadFromString returns. So the sizes below
// only affect the brief peak during config.reload / boot — never persistent
// heap.
//
// Sizing (1024 nodes / 32 KB strings / depth 16):
//   - Nodes:   1024 × ~24 B  = ~24 KB
//   - Strings:                  32 KB
//   - Stack:   16 × 8 B       = ~128 B
//   - Peak:                    ~56 KB transient during parse
//
// RP2040 has 264 KB SRAM; LightFX baseline is ~33 KB (~12% used) → ~230 KB
// free heap. A 56 KB transient peak still leaves 4× headroom and supports a
// fully populated Studio config (8+ programs × 8 channels × 16 events plus
// landing groups and servo bindings).
struct LightFxYamlPool {
    static constexpr size_t MAX_NODES        = 1024;
    static constexpr size_t STRING_POOL_SIZE = 32768;
    static constexpr size_t MAX_DEPTH        = 16;
};

// ============================================================================
// Data Struct
// ============================================================================

struct LightFxConfig {

    // ---- Battery monitoring ----
    // The ADC monitor is always on; config only sets chemistry, cell count,
    // and the auto-cutoff safety reaction (soft-disable all 8 LED channels
    // when the pack drops below the chemistry's per-cell low threshold).
    // Battery is a hardware-local concern — never pushed to a slave applier
    // (slaves have their own battery, if any), so it lives outside the
    // embedded LightProgramConfig and outside the applier surface.
    struct Battery {
        bool     autoCutoff      = true;   // Soft-disable LED channels on low voltage
        char     chemistry[8]    = "lipo"; // lipo|liion|nimh (monitor is always on)
        uint8_t  cellCount       = 0;      // Number of cells (0 = auto-detect)
    } battery;

    // ---- Light program (master brightness + servos + landing groups + programs) ----
    // Shared LightProgramConfig — walked locally by applyLightProgramConfigLocal()
    // on the standalone Pico, and by pushLightFxConfigToSlave(cfg, client) on
    // the hub. Master brightness lives inside `lightProgram` to keep the two
    // call sites coherent.
    LightProgramConfig lightProgram{};
};

// ============================================================================
// Declarative Schema
// ============================================================================

namespace lightfx_config {

using namespace sfx;

using B = LightFxConfig::Battery;

/// Field-level schema for the LightFX-only sections (battery).
/// LightProgramConfig is populated separately because its declarative schema
/// lives at YAML root (no parent key) and the DSL only supports `asGroup`,
/// not "embed at root."  See LightFxConfigSchema::populate() below.
inline const auto fields = schema<LightFxConfig>(

    // ---- Battery monitoring (always on; chemistry/cells select profile) ----
    group<&LightFxConfig::battery>("battery",
        prop<&B::autoCutoff> ("auto_cutoff", true),
        prop<&B::chemistry>  ("chemistry",   "lipo"),
        prop<&B::cellCount>  ("cell_count",  uint8_t(0)).range(0, 6)
    )
);

} // namespace lightfx_config

// ============================================================================
// ConfigStore Schema Adapter
// ============================================================================

struct LightFxConfigSchema {
    using DataType = LightFxConfig;

    // Templated on the parser's pool type so ConfigStore can instantiate us
    // with LightFxYamlPool (or any future board-local preset) without a
    // signature mismatch. Inner fields.populate() only touches YamlNode, which
    // is pool-independent, so no other code needs to change.
    template<typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        // Battery section first.
        bool ok = lightfx_config::fields.populate(d, p.root());
        // Then walk the same root for the LightProgramConfig sections
        // (master_brightness, servos, landing_groups, programs, input).
        // Both populates ignore unknown keys, so the two fields-sets do not
        // collide as long as masterBrightness is no longer a top-level
        // member of LightFxConfig (only on the embedded lightProgram).
        ok = light_program_config::fields.populate(d.lightProgram, p.root()) && ok;
        return ok;
    }

    static bool validate(const DataType& d, char* err, size_t errLen) {
        if (!lightfx_config::fields.validate(d, err, errLen)) return false;
        return light_program_config::fields.validate(d.lightProgram, err, errLen);
    }

    static const char* defaultPath() { return "/lightfx.yaml"; }
};

#endif // LIGHTFX_CONFIG_H
