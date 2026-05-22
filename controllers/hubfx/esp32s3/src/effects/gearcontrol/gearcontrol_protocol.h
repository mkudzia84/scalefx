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
    /// `[id:u8]` → ACK / NACK.  Clears ERROR → RETRACTED so the gear
    /// accepts deploy/retract again.  No-op (ACK) when not in ERROR.
    constexpr uint8_t GEAR_RESET       = 0xC7;
    /// `[id:u8]` → ACK / NACK.  Start stall-endpoint calibration: the
    /// gear is driven to its retract + deploy hard stops (confirmed by
    /// MOTOR_STALL_EVENT) to validate both endpoints, then homed.  No
    /// stall within the travel timeout → ERROR (NO_STALL_DETECTED).
    constexpr uint8_t GEAR_CALIBRATE   = 0xC8;
    /// `[id:u8]` → ACK / NACK.  Abort an in-progress calibration; the
    /// motor brakes and the gear returns to RETRACTED.
    constexpr uint8_t GEAR_CALIB_CANCEL = 0xC9;
}

namespace GearAllAction {
    constexpr uint8_t Stop    = 0;
    constexpr uint8_t Deploy  = 1;
    constexpr uint8_t Retract = 2;
}

/// Lifecycle phase reported by `GEAR_STATUS_RESP` and `GEAR_PHASE_EVENT`.
/// The host status view groups these as: idle (Retracted / Deployed),
/// moving (Deploying / Retracting), calibrating, or error.
namespace GearPhase {
    constexpr uint8_t Unconfigured = 0;
    constexpr uint8_t Retracted    = 1;
    constexpr uint8_t Deploying    = 2;
    constexpr uint8_t Deployed     = 3;
    constexpr uint8_t Retracting   = 4;
    constexpr uint8_t Error        = 5;
    constexpr uint8_t Calibrating  = 6;

    inline const char* getName(uint8_t p) {
        switch (p) {
            case Unconfigured: return "unconfigured";
            case Retracted:    return "retracted";
            case Deploying:    return "deploying";
            case Deployed:     return "deployed";
            case Retracting:   return "retracting";
            case Error:        return "error";
            case Calibrating:  return "calibrating";
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
    constexpr uint8_t NO_STALL_DETECTED= 0xC6;   ///< calibration: no endpoint stall

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:         return "Unknown gear id";
            case GEAR_TABLE_FULL:    return "Gear registry full";
            case MOTOR_UNAVAILABLE:  return "Gear motor unavailable";
            case IN_ERROR_STATE:     return "Gear in error state — issue GEAR_RESET to clear";
            case TIMEOUT:            return "Gear motor timed out";
            case NO_STALL_DETECTED:  return "Calibration found no endpoint stall";
            default:                 return nullptr;
        }
    }
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEARCONTROL_PROTOCOL_H
