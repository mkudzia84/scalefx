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
#include <serial/ports.h>            // PortKind::*

#include "../effects/effect_id.h"    // PortRef

namespace hubfx::config {

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
    const char* guid = node->template childAs<const char*>("guid", "");
    if (guid && guid[0]) {
        std::strncpy(r.guid, guid, sizeof(r.guid) - 1);
        r.guid[sizeof(r.guid) - 1] = '\0';
    }
    return r;
}

}  // namespace hubfx::config

#endif  // HUBFX_CONFIG_PORT_REF_YAML_H
