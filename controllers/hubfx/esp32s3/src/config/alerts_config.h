/*
 * alerts_config.h — `/alerts.yaml` schema.
 *
 * Severities → AlertSound enum names + volume.  Lives as its own
 * config file so the alert library can grow independently of system
 * state (sound packs, severity overrides for specific airframes,
 * etc. — see instructions/19-HUBFX-CONFIG-SCHEMA.md).
 *
 * YAML shape:
 *
 *   schema_version: 1
 *   enabled: true
 *   severities:
 *     info:     { sound: init,             volume_pct: 70 }
 *     warning:  { sound: warning,          volume_pct: 80 }
 *     error:    { sound: error,            volume_pct: 90 }
 *     critical: { sound: critical,         volume_pct: 100 }
 *
 * `sound:` accepts any snake-case name listed in `alert_sound.h`'s
 * AlertSound enum.  Unknown names map to `AlertSound::None` (silent
 * no-op rather than a parse failure).
 *
 * The mixer channel is board-fixed at `HubFxLayout::Alert` (== 0) —
 * not configurable via YAML.  See `effects/audio_layout.h`.
 */

#ifndef HUBFX_ALERTS_CONFIG_H
#define HUBFX_ALERTS_CONFIG_H

#include <cstdint>
#include <cstring>

#include <config/yaml_schema.h>

#include "../effects/alerts/alert_sound.h"

// Shared with hubfx_config.h — keep the pool definition in sync.
struct AlertsYamlPool {
    static constexpr size_t MAX_NODES        = 96;
    static constexpr size_t STRING_POOL_SIZE = 1536;
    static constexpr size_t MAX_DEPTH        = 8;
};

/// Parsed form of `/alerts.yaml`.  Translated to
/// `hubfx::effects::alerts::AlertServiceConfig` at apply time.
struct AlertsConfig {
    static constexpr uint8_t kSchemaVersion = 1;

    struct Severity {
        // `sound` is stored as the YAML string so we don't need a
        // string→enum mapper in the schema DSL — apply-time
        // translation converts to `AlertSound`.  Limit matches the
        // longest snake_name in `alertSoundName()`.
        char    soundName[24]  = {};
        uint8_t volumePct      = 100;
    };

    bool     enabled  = true;
    Severity info;
    Severity warning;
    Severity error;
    Severity critical;
};

// ─── Declarative schema ─────────────────────────────────────────────

namespace alerts_config_schema {

using namespace sfx;
using S = AlertsConfig::Severity;

inline const auto fields = schema<AlertsConfig>(
    prop<&AlertsConfig::enabled> ("enabled",  true),

    group<&AlertsConfig::info>("info",
        prop<&S::soundName>("sound",      "init"),
        prop<&S::volumePct>("volume_pct", uint8_t(70)).range(0, 100)
    ),
    group<&AlertsConfig::warning>("warning",
        prop<&S::soundName>("sound",      "warning"),
        prop<&S::volumePct>("volume_pct", uint8_t(80)).range(0, 100)
    ),
    group<&AlertsConfig::error>("error",
        prop<&S::soundName>("sound",      "error"),
        prop<&S::volumePct>("volume_pct", uint8_t(90)).range(0, 100)
    ),
    group<&AlertsConfig::critical>("critical",
        prop<&S::soundName>("sound",      "critical"),
        prop<&S::volumePct>("volume_pct", uint8_t(100)).range(0, 100)
    )
);

}  // namespace alerts_config_schema

// ─── ConfigStore adapter ────────────────────────────────────────────

struct AlertsConfigSchema {
    using DataType = AlertsConfig;

    template <typename TPool>
    static bool populate(DataType& d, const YamlParser<TPool>& p) {
        return alerts_config_schema::fields.populate(d, p.root());
    }
    static bool validate(const DataType& d, char* err, size_t errLen) {
        return alerts_config_schema::fields.validate(d, err, errLen);
    }
    static const char* defaultPath() { return "/alerts.yaml"; }
};

/// Translate the parsed YAML to a firmware-side `AlertServiceConfig`.
/// Resolves each severity's `soundName` string to an `AlertSound`
/// enum; unknown names log a one-line WARN and degrade to `None`.
inline hubfx::effects::alerts::AlertServiceConfig
toAlertServiceConfig(const AlertsConfig& y) {
    using namespace hubfx::effects::alerts;
    AlertServiceConfig cfg;
    cfg.enabled = y.enabled;
    // cfg.channel stays at its default (HubFxLayout::Alert) — board-fixed,
    // not exposed via YAML.

    auto fill = [](AlertSeverityCfg& dst, const AlertsConfig::Severity& src) {
        dst.sound  = alertSoundFromName(src.soundName);
        dst.volume = src.volumePct;
        // Leave dst.path empty — the play path comes from
        // `alertSoundPath(dst.sound)` at runtime.  `outputMask` keeps
        // its default (ALL); not configurable from `/alerts.yaml` yet.
    };
    fill(cfg.info,     y.info);
    fill(cfg.warning,  y.warning);
    fill(cfg.error,    y.error);
    fill(cfg.critical, y.critical);
    return cfg;
}

#endif  // HUBFX_ALERTS_CONFIG_H
