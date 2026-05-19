/*
 * effect_id.h — master-side identifiers for effect-family ownership.
 *
 *   The HubFX master orchestrates several independent effect families
 *   (LightFX programs, LandingLight state machines, GunFX, GearControl,
 *   ...) each of which claims a set of (board, port) targets.  The
 *   `TopologyServicePolicy` claim registry uses these IDs to enforce
 *   exclusivity: a port that's been claimed by one effect family can't
 *   be silently driven by another.
 *
 *   Append-only enum.  These IDs are master-internal — NOT carried on
 *   the wire today.  If we later expose claim state to Studio (e.g.
 *   `TOPOLOGY_CLAIMS_LIST_REQ`), the same byte values become wire
 *   constants and that's when Rule 11 starts applying.
 */

#ifndef HUBFX_EFFECT_ID_H
#define HUBFX_EFFECT_ID_H

#include <cstdint>
#include <cstring>

namespace hubfx::effects {

enum class EffectId : uint8_t {
    None         = 0x00,
    LightFx      = 0x01,
    GunFx        = 0x02,
    GearCtrl     = 0x03,
    LandingLight = 0x04,
    // 0x05..0xFE reserved
};

inline const char* effectIdName(EffectId id) {
    switch (id) {
        case EffectId::None:         return "none";
        case EffectId::LightFx:      return "lightfx";
        case EffectId::GunFx:        return "gunfx";
        case EffectId::GearCtrl:     return "gearcontrol";
        case EffectId::LandingLight: return "landing-light";
        default:                     return "unknown";
    }
}

// ─── PortRef — system-wide port address ──────────────────────────────
//
// `guid[0] == 0` means "the hub's own port" (synonymous with the
// 4-hex-char hub-guid suffix).  Otherwise `guid` is the 4-hex-char
// suffix of the target expander's deviceName.  `portKind` is one of
// `PortKind::Servo/Pwm/HBridge/Input`; `portIdx` is the per-kind
// index on that board.
//
struct PortRef {
    char    guid[5]  = {0};   ///< "" = hub-local
    uint8_t portKind = 0;     ///< PortKind::*
    uint8_t portIdx  = 0;

    bool isLocal() const { return guid[0] == 0; }

    bool equals(const PortRef& o) const {
        if (portKind != o.portKind || portIdx != o.portIdx) return false;
        return std::strcmp(guid, o.guid) == 0;
    }

    // Convenience constructor for hub-local ports.
    static PortRef local(uint8_t kind, uint8_t idx) {
        PortRef r;
        r.guid[0] = 0;
        r.portKind = kind;
        r.portIdx  = idx;
        return r;
    }

    // Convenience constructor for remote ports.
    static PortRef remote(const char* g, uint8_t kind, uint8_t idx) {
        PortRef r;
        if (g) {
            size_t n = std::strlen(g);
            if (n > 4) n = 4;
            std::memcpy(r.guid, g, n);
            r.guid[n] = 0;
        }
        r.portKind = kind;
        r.portIdx  = idx;
        return r;
    }
};

}  // namespace hubfx::effects

#endif  // HUBFX_EFFECT_ID_H
