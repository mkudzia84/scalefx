/*
 * landing_light_protocol.h — HubFX wire surface for the landing-light
 * effect.
 *
 *   A landing light is a (servo + LED group) primitive owned exclusively
 *   by the master.  Studio / CLI talks to the hub; the hub orchestrates
 *   the servo and the LEDs via topology.  Programs in other effect
 *   families (lightfx, gearcontrol) reference landing lights by ID
 *   instead of driving the underlying ports directly.
 *
 *   Packet slice: 0xB1..0xB6 inside the HubFX-master 0x80..0xAF range
 *   (the legacy slot 0xB1..0xB2 used by `SLAVE_ENUM_*` was retired
 *   with the generic-expander refactor).
 *
 *   Rule 11 append-only — never reorder existing payload fields.
 */

#ifndef HUBFX_LANDING_LIGHT_PROTOCOL_H
#define HUBFX_LANDING_LIGHT_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::landing {

namespace LandingLightPacket {
    /// `[]` → LL_LIST_RESP
    constexpr uint8_t LL_LIST_REQ      = 0xB1;
    /// `[count:u8]` per-entry:
    ///   `[id:u8][nameLen:u8][name:str][owner:u8][phase:u8]`
    constexpr uint8_t LL_LIST_RESP     = 0xB2;
    /// `[id:u8][state:u8]` (0=OFF, 1=ON) → ACK / NACK
    constexpr uint8_t LL_SET_STATE     = 0xB3;
    /// `[]` → LL_STATUS_RESP
    constexpr uint8_t LL_STATUS_REQ    = 0xB4;
    /// `[count:u8]` per-entry: `[id:u8][phase:u8]`
    constexpr uint8_t LL_STATUS_RESP   = 0xB5;
    /// async TAG_ASYNC: `[id:u8][phase:u8]`
    constexpr uint8_t LL_PHASE_EVENT   = 0xB6;
}

/// Lifecycle phase reported by `LL_STATUS_RESP` / `LL_PHASE_EVENT`.
namespace LandingLightPhase {
    constexpr uint8_t Unconfigured = 0;
    constexpr uint8_t Retracted    = 1;
    constexpr uint8_t Deploying    = 2;
    constexpr uint8_t Deployed     = 3;
    constexpr uint8_t Retracting   = 4;

    inline const char* getName(uint8_t p) {
        switch (p) {
            case Unconfigured: return "unconfigured";
            case Retracted:    return "retracted";
            case Deploying:    return "deploying";
            case Deployed:     return "deployed";
            case Retracting:   return "retracting";
            default:           return "unknown";
        }
    }
}

/// Requested state for `LL_SET_STATE`.
namespace LandingLightState {
    constexpr uint8_t Off = 0;
    constexpr uint8_t On  = 1;
}

/// Landing-light layer error codes, inside the HubFX 0x80..0x8F slot.
namespace LandingLightError {
    constexpr uint8_t UNKNOWN_ID         = 0xB1;
    constexpr uint8_t WRONG_OWNER        = 0xB2;  ///< caller's effect ID doesn't own this LL
    constexpr uint8_t SERVO_UNAVAILABLE  = 0xB3;
    constexpr uint8_t LL_TABLE_FULL      = 0xB4;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:        return "Unknown landing-light id";
            case WRONG_OWNER:       return "Landing-light owned by a different effect";
            case SERVO_UNAVAILABLE: return "Landing-light servo unavailable";
            case LL_TABLE_FULL:     return "Landing-light table full";
            default:                return nullptr;
        }
    }
}

}  // namespace hubfx::effects::landing

#endif  // HUBFX_LANDING_LIGHT_PROTOCOL_H
