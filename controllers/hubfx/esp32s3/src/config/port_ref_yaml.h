/*
 * port_ref_yaml.h — shared `{kind, idx, guid?}` PortRef parser.
 *
 *   Every sub-config that references a hub-local or expander port uses
 *   the same YAML shape:
 *
 *       port: { kind: pwm, idx: 5 }                    # hub-local
 *       port: { kind: servo, idx: 0, guid: AB12 }      # remote expander
 *
 *   This header centralises the YamlNode → `hubfx::effects::PortRef`
 *   translator so landing_config / enginefx_config / lightfx_program_loader
 *   all parse the same way and a future shape change (e.g. adding a
 *   speed-curve field) lands in one place.
 */

#ifndef HUBFX_CONFIG_PORT_REF_YAML_H
#define HUBFX_CONFIG_PORT_REF_YAML_H

#include <cstdint>
#include <cstring>

#include <config/yaml_parser.h>
#include <serial/diag_log.h>         // SFX_LOG_WARN
#include <serial/ports.h>            // PortKind::*

#include "../effects/effect_id.h"    // PortRef

namespace hubfx::config {

// ─── Board-alias table (cross-file `board: <alias>` resolution) ──────
//
// `/hubfx.yaml` declares expander boards as `alias → guid` pairs and
// loads FIRST (its store is wired ahead of every effect store).  Its
// populate() calls `setBoardAliases(...)` to seed this module-level
// table; sub-config stores that parse later resolve `board: <alias>`
// PortRefs through `portRefFromNode` against it.  Single-binary firmware
// ⇒ one `inline` definition; the table is rebuilt on every config-reload
// (hubfx reloads first).  `guid:` raw form still works and bypasses the
// table entirely.
constexpr uint8_t kAliasTableMax = 4;     ///< matches HubFxConfig::kMaxExpanderDecls
constexpr size_t  kAliasNameMax  = 16;    ///< matches HubFxConfig::kAliasMax
constexpr size_t  kAliasGuidMax  = 5;     ///< 4 hex + NUL (matches PortRef::guid)

struct AliasEntry {
    char alias[kAliasNameMax] = {};
    char guid [kAliasGuidMax] = {};
};

inline AliasEntry gAliasTable[kAliasTableMax];
inline uint8_t    gAliasCount = 0;

/// Seed the alias table from any array exposing `.alias` / `.guid` char
/// members (e.g. `HubFxConfig::ExpanderDecl`).  Templated so this header
/// stays free of a circular include on hubfx_config.h.
template <typename TDecl>
inline void setBoardAliases(const TDecl* decls, uint8_t n) {
    gAliasCount = 0;
    if (!decls) return;
    for (uint8_t i = 0; i < n && gAliasCount < kAliasTableMax; ++i) {
        if (!decls[i].alias[0] || !decls[i].guid[0]) continue;   // anonymous board ⇒ no alias
        AliasEntry& e = gAliasTable[gAliasCount++];
        std::memset(&e, 0, sizeof(e));
        std::strncpy(e.alias, decls[i].alias, sizeof(e.alias) - 1);
        std::strncpy(e.guid,  decls[i].guid,  sizeof(e.guid)  - 1);
    }
}

/// Resolve a board alias → 4-hex GUID.  Returns nullptr on unknown alias.
inline const char* resolveBoardAlias(const char* alias) {
    if (!alias || !alias[0]) return nullptr;
    for (uint8_t i = 0; i < gAliasCount; ++i) {
        if (std::strcmp(gAliasTable[i].alias, alias) == 0) return gAliasTable[i].guid;
    }
    return nullptr;
}

/// Snake-case kind name → PortKind enum.  Returns 0 on unknown
/// (caller's policy: skip the entry or substitute a default).
inline uint8_t portKindFromName(const char* name) {
    if (!name) return 0;
    if (std::strcmp(name, "servo")   == 0) return PortKind::Servo;
    if (std::strcmp(name, "pwm")     == 0) return PortKind::Pwm;
    if (std::strcmp(name, "hbridge") == 0) return PortKind::HBridge;
    if (std::strcmp(name, "input")   == 0) return PortKind::Input;
    return 0;
}

/// Parse a `{kind, idx, guid?}` YamlNode map into a PortRef.  Returns
/// a default-constructed PortRef (portKind=0) on `nullptr` or missing
/// kind — caller should check `r.portKind != 0` before using.
inline hubfx::effects::PortRef portRefFromNode(const YamlNode* node) {
    hubfx::effects::PortRef r;
    if (!node) return r;
    r.portKind = portKindFromName(node->template childAs<const char*>("kind", ""));
    r.portIdx  = (uint8_t)node->template childAs<int32_t>("idx", 0);

    // Board addressing — `board: <alias>` is preferred (resolved against
    // the /hubfx.yaml expander table); `guid: <hex>` is the raw fallback.
    // Missing both ⇒ hub-local (guid stays "").
    const char* board = node->template childAs<const char*>("board", "");
    if (board && board[0]) {
        const char* g = resolveBoardAlias(board);
        if (g && g[0]) {
            std::strncpy(r.guid, g, sizeof(r.guid) - 1);
        } else {
            SFX_LOG_WARN("[portref] unknown board alias '%s' — treating as hub-local", board);
        }
        return r;
    }
    const char* guid = node->template childAs<const char*>("guid", "");
    if (guid && guid[0]) {
        std::strncpy(r.guid, guid, sizeof(r.guid) - 1);
        r.guid[sizeof(r.guid) - 1] = '\0';
    }
    return r;
}

}  // namespace hubfx::config

#endif  // HUBFX_CONFIG_PORT_REF_YAML_H
