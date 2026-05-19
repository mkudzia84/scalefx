/*
 * gearcontrol_protocol.h — HubFX wire surface for the GearControl effect.
 *
 *   A "gear" is one retractable landing-gear unit: an H-bridge motor
 *   (BiDcMotor role on an HBridgePort) plus 0..N status LEDs (LedAnimator
 *   roles on PWM ports).  Doors are deferred to a follow-up turn —
 *   v1 ships with motor + LEDs only.
 *
 *   The effect runs on the HubFX master and addresses every port via
 *   `TopologyServicePolicy`.  Stall + servo-target events from the
 *   target boards drive the state machine through topology's
 *   `onRoleEvent` subscription.
 *
 *   Gear-bound landing lights: when a gear transitions DEPLOYED ↔ RETRACTED,
 *   the effect forwards setState(ON/OFF) calls to `LandingLightService`
 *   for every landing light whose `owner == EffectId::GearCtrl`.
 *
 *   Packet slice: 0xBE..0xC6 in the HubFX 0x80..0xCF range.
 */

#ifndef HUBFX_GEARCONTROL_PROTOCOL_H
#define HUBFX_GEARCONTROL_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::gearctrl {

namespace GearPacket {
    /// `[id:u8]` → ACK / NACK
    constexpr uint8_t GEAR_DEPLOY      = 0xBE;
    /// `[id:u8]` → ACK / NACK
    constexpr uint8_t GEAR_RETRACT     = 0xBF;
    /// `[id:u8]` → ACK
    constexpr uint8_t GEAR_STOP        = 0xC0;
    /// `[action:u8]` 0=stop, 1=deploy, 2=retract — applies to every configured gear.
    constexpr uint8_t GEAR_ALL         = 0xC1;
    /// `[]` → GEAR_STATUS_RESP
    constexpr uint8_t GEAR_STATUS_REQ  = 0xC2;
    /// `[count:u8]` per-entry: `[id:u8][phase:u8]`
    constexpr uint8_t GEAR_STATUS_RESP = 0xC3;
    /// async TAG_ASYNC: `[id:u8][phase:u8]`
    constexpr uint8_t GEAR_PHASE_EVENT = 0xC4;
    /// `[]` → GEAR_LIST_RESP
    constexpr uint8_t GEAR_LIST_REQ    = 0xC5;
    /// `[count:u8]` per-entry: `[id:u8][nameLen:u8][name:str]`
    constexpr uint8_t GEAR_LIST_RESP   = 0xC6;
}

namespace GearAllAction {
    constexpr uint8_t Stop    = 0;
    constexpr uint8_t Deploy  = 1;
    constexpr uint8_t Retract = 2;
}

/// Lifecycle phase reported by `GEAR_STATUS_RESP` and `GEAR_PHASE_EVENT`.
namespace GearPhase {
    constexpr uint8_t Unconfigured = 0;
    constexpr uint8_t Retracted    = 1;
    constexpr uint8_t Deploying    = 2;
    constexpr uint8_t Deployed     = 3;
    constexpr uint8_t Retracting   = 4;
    constexpr uint8_t Error        = 5;

    inline const char* getName(uint8_t p) {
        switch (p) {
            case Unconfigured: return "unconfigured";
            case Retracted:    return "retracted";
            case Deploying:    return "deploying";
            case Deployed:     return "deployed";
            case Retracting:   return "retracting";
            case Error:        return "error";
            default:           return "unknown";
        }
    }
}

/// GearControl-effect error codes (HubFX 0x80..0x8F slot, distinct
/// values from the legacy expander to avoid table-key collisions).
namespace GearError {
    constexpr uint8_t UNKNOWN_ID       = 0xC1;
    constexpr uint8_t GEAR_TABLE_FULL  = 0xC2;
    constexpr uint8_t MOTOR_UNAVAILABLE= 0xC3;
    constexpr uint8_t IN_ERROR_STATE   = 0xC4;
    constexpr uint8_t TIMEOUT          = 0xC5;

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:        return "Unknown gear id";
            case GEAR_TABLE_FULL:   return "Gear registry full";
            case MOTOR_UNAVAILABLE: return "Gear motor unavailable";
            case IN_ERROR_STATE:    return "Gear is in error state — issue GEAR_STOP to reset";
            case TIMEOUT:           return "Gear motor timed out";
            default:                return nullptr;
        }
    }
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEARCONTROL_PROTOCOL_H
