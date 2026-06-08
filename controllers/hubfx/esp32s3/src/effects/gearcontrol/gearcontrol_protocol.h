/*
 * gearcontrol_protocol.h — HubFX wire surface for the GearControl effect.
 *
 *   A "gear" is one retractable landing-gear unit: an H-bridge motor
 *   (BiDcMotor role on an HBridgePort) plus ≤2 door servos (ServoActuator
 *   roles).  Deploy / retract bracket the motor seek with door open/close
 *   legs (instructions/29): OPEN_DOORS → [SYNC] → RUN_MOTOR → [SYNC] →
 *   CLOSE_DOORS.  Door-leg completion is MONITORED via the role's
 *   SERVO_MOTION_DONE async (decision #1) — no timers.
 *
 *   The effect runs on the HubFX master and addresses every port via
 *   `TopologyServicePolicy`.  Motor endstop results (BIMOTOR_ENDSTOP_
 *   RESULT) and door servo motion-done (SERVO_MOTION_DONE) events from
 *   the target boards drive the state machine through topology's
 *   `onRoleEvent` subscription.
 *
 *   Gear-bound landing lights: when a gear transitions DEPLOYED ↔ RETRACTED,
 *   the effect forwards setState(ON/OFF) calls to `LandingLightService`
 *   for every landing light whose `owner == EffectId::GearCtrl`.
 *
 *   Packet slice: 0xBE..0xC6 (core verbs) + 0xD7 (reset).  0xD8/0xD9 were
 *   GEAR_CALIBRATE / GEAR_CALIB_CANCEL — REMOVED (instructions/29 decision
 *   #3, endstop calibration now lives entirely on the BiDcMotor role's
 *   LiveRatio + ceiling guard) and are FREE.  Do NOT reuse 0xC7..0xC9 —
 *   they belong to EngineFX.
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
    /// `[count:u8]` per-entry: `[id:u8][phase:u8][subPhase:u8]` (Rule 11:
    /// old clients read the first 2 bytes; the trailing subPhase is the
    /// rich door/motor sub-state).
    constexpr uint8_t GEAR_STATUS_RESP = 0xC3;
    /// async TAG_ASYNC: `[id:u8][phase:u8][subPhase:u8]` (Rule 11 append).
    /// Broadcast whenever the overall phase OR the sub-phase changes.
    constexpr uint8_t GEAR_PHASE_EVENT = 0xC4;
    /// `[]` → GEAR_LIST_RESP
    constexpr uint8_t GEAR_LIST_REQ    = 0xC5;
    /// `[count:u8]` per-entry: `[id:u8][nameLen:u8][name:str]`
    constexpr uint8_t GEAR_LIST_RESP   = 0xC6;
    /// `[id:u8]` → ACK / NACK.  Clears ERROR → RETRACTED so the gear
    /// accepts deploy/retract again.  No-op (ACK) when not in ERROR.
    /// (0xC7..0xC9 are EngineFX — this lives at 0xD7.)
    constexpr uint8_t GEAR_RESET       = 0xD7;
    // 0xD8 / 0xD9 FREE — were GEAR_CALIBRATE / GEAR_CALIB_CANCEL (removed).
}

namespace GearAllAction {
    constexpr uint8_t Stop    = 0;
    constexpr uint8_t Deploy  = 1;
    constexpr uint8_t Retract = 2;
}

/// Lifecycle phase reported by `GEAR_STATUS_RESP` and `GEAR_PHASE_EVENT`.
/// The host status view groups these as: idle (Retracted / Deployed),
/// moving (Deploying / Retracting), or error.
namespace GearPhase {
    constexpr uint8_t Unconfigured = 0;
    constexpr uint8_t Retracted    = 1;
    constexpr uint8_t Deploying    = 2;
    constexpr uint8_t Deployed     = 3;
    constexpr uint8_t Retracting   = 4;
    constexpr uint8_t Error        = 5;
    // 6 was Calibrating — REMOVED (instructions/29 decision #3).

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

/// Sub-phase inside a deploy/retract cycle — the door-bracket op-queue
/// position (instructions/29 decision #5, rich state propagation).
/// Carried as the trailing byte of GEAR_STATUS_RESP + GEAR_PHASE_EVENT.
namespace GearSubPhase {
    constexpr uint8_t idle          = 0;   ///< settled (Retracted / Deployed / Error)
    constexpr uint8_t doors_opening = 1;   ///< OPEN_DOORS leg in flight
    constexpr uint8_t doors_open    = 2;   ///< barrier: doors open, awaiting motor leg
    constexpr uint8_t motor_running = 3;   ///< RUN_MOTOR leg in flight (seek)
    constexpr uint8_t motor_done    = 4;   ///< barrier: motor reached, awaiting close
    constexpr uint8_t doors_closing = 5;   ///< CLOSE_DOORS leg in flight

    inline const char* getName(uint8_t s) {
        switch (s) {
            case idle:          return "idle";
            case doors_opening: return "doors-opening";
            case doors_open:    return "doors-open";
            case motor_running: return "motor-running";
            case motor_done:    return "motor-done";
            case doors_closing: return "doors-closing";
            default:            return "unknown";
        }
    }
}

/// Door-pair sequencing for the OPEN leg (and the reverse on CLOSE).
/// instructions/29 decision #4 / archive DoorMode.  NONE = no doors
/// (instant complete); SINGLE = door0 only; DUAL_SYNC = both at once;
/// DUAL_DELAY = door1 after doorDelayMs; DUAL_SEQ = door1 after door0's
/// SERVO_MOTION_DONE.
namespace DoorMode {
    constexpr uint8_t NONE       = 0;
    constexpr uint8_t SINGLE     = 1;
    constexpr uint8_t DUAL_SYNC  = 2;
    constexpr uint8_t DUAL_DELAY = 3;
    constexpr uint8_t DUAL_SEQ   = 4;
}

/// Post-deploy close policy — which doors close after a deploy completes
/// (retract re-opens whatever closed).  BOTH = close both; FIRST = close
/// door0 only (door1 stays open); NONE = close none.
namespace ClosePolicy {
    constexpr uint8_t BOTH  = 0;
    constexpr uint8_t FIRST = 1;
    constexpr uint8_t NONE  = 2;
}

/// Cross-channel coordination mode (instructions/29 decision #2).
///   Independent — no barriers; each gear runs its own queue.
///   DoorSync    — barrier at door phases only (all open together, all
///                 close together; motors run independently).
///   FullSync    — barriers at door AND motor phases (all open → all run
///                 motors → all close, lockstep).
///   Sequenced   — gear[0] full cycle, then gear[1], … one at a time.
namespace CoordMode {
    constexpr uint8_t Independent = 0;
    constexpr uint8_t DoorSync    = 1;
    constexpr uint8_t FullSync    = 2;
    constexpr uint8_t Sequenced   = 3;
}

/// GearControl-effect error codes — CLAUDE.md error-range allocation
/// reserves 0x60..0x6F for GearControl.
namespace GearError {
    constexpr uint8_t UNKNOWN_ID       = 0x60;
    constexpr uint8_t GEAR_TABLE_FULL  = 0x61;
    constexpr uint8_t MOTOR_UNAVAILABLE= 0x62;
    constexpr uint8_t IN_ERROR_STATE   = 0x63;
    constexpr uint8_t TIMEOUT          = 0x64;
    // 0x65 was NO_STALL_DETECTED (calibration) — REMOVED with calibration.

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:         return "Unknown gear id";
            case GEAR_TABLE_FULL:    return "Gear registry full";
            case MOTOR_UNAVAILABLE:  return "Gear motor unavailable";
            case IN_ERROR_STATE:     return "Gear in error state — issue GEAR_RESET to clear";
            case TIMEOUT:            return "Gear motor timed out";
            default:                 return nullptr;
        }
    }
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEARCONTROL_PROTOCOL_H
