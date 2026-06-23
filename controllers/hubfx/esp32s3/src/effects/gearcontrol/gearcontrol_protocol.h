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
    /// `[count:u8]` per-entry: `[id:u8][phase:u8][subPhase:u8][errReason:u8]`
    /// `[doorsOpen:u8][strutState:u8]` (Rule 11: old clients read the first 2
    /// bytes; subPhase is the rich door/strut sub-state; errReason is the
    /// GearError code when phase==Error else 0 — appended 2026-06-14; doorsOpen
    /// (1=all doors open) + strutState (GearStrutState) appended 2026-06-23 for
    /// the manual-control interlock gate; host derives stride from count).
    constexpr uint8_t GEAR_STATUS_RESP = 0xC3;
    /// async TAG_ASYNC: `[id:u8][phase:u8][subPhase:u8][errReason:u8]`
    /// `[doorsOpen:u8][strutState:u8]` (Rule 11 append).  Broadcast whenever the
    /// overall phase, the sub-phase, the door-open flag OR the strut state
    /// changes.  errReason carries the GearError code in the Error phase.
    constexpr uint8_t GEAR_PHASE_EVENT = 0xC4;
    /// `[]` → GEAR_LIST_RESP
    constexpr uint8_t GEAR_LIST_REQ    = 0xC5;
    /// `[count:u8]` per-entry: `[id:u8][nameLen:u8][name:str]`
    constexpr uint8_t GEAR_LIST_RESP   = 0xC6;
    /// `[id:u8]` → ACK / NACK.  Clears ERROR → UNKNOWN so the gear
    /// accepts deploy/retract again.  No-op (ACK) when not in ERROR.
    /// (0xC7..0xC9 are EngineFX — this lives at 0xD7.)
    constexpr uint8_t GEAR_RESET       = 0xD7;
    /// `[]` (whole set) or `[id:u8]` (one) → ACK.  EMERGENCY STOP: brake the
    /// motor + freeze the door servos exactly where they are (phase → HELD;
    /// position becomes uncertain).  Distinct from GEAR_STOP / GEAR_ALL-stop
    /// (which reset to a baseline) — a HELD gear resumes on the next deploy
    /// OR retract.  Was GEAR_CALIBRATE (removed).
    constexpr uint8_t GEAR_ESTOP       = 0xD8;
    /// `[id:u8][target:u8]` → ACK / NACK.  SINGLE-STEP: advance the strut ONE
    /// leg toward `target` (0=up, 1=down) then PARK at the next boundary
    /// (doors-open / strut-done / settled) — the manual "do one item in the
    /// sequence" command.  Auto-clears an ERROR first (like deploy/retract).
    /// Was GEAR_CALIB_CANCEL (removed).
    constexpr uint8_t GEAR_STEP        = 0xD9;

    // ── Manual / maintenance: drive doors and strut INDEPENDENTLY (setup) ──
    //   These live in the free 0x01..0x04 block (the gear slice 0xBE..0xC6 +
    //   0xD7..0xD9 is contiguous-full; 0x00..0x0F is the documented free range).
    //   Each enforces a HARD safety interlock IN FIRMWARE (the GUI gate only
    //   mirrors it): close-doors requires the strut UP; move-strut (either way)
    //   requires the doors OPEN.  Rejected → NACK with the GearError below.
    //   They refuse while a coordinated cycle (deploy/retract) is in flight.
    /// `[id:u8][open:u8]` (open=1 open / 0 close) → ACK / NACK.  Drive ONE
    /// strut's doors only, honouring its configured door-mode + close-policy.
    constexpr uint8_t GEAR_DOOR        = 0x01;
    /// `[id:u8][down:u8]` (down=1 deploy / 0 retract) → ACK / NACK.  Drive ONE
    /// strut's motor only to its endstop.
    constexpr uint8_t GEAR_STRUT       = 0x02;
    /// `[open:u8]` → ACK / NACK.  Every configured strut's doors (per-leg
    /// interlock; NACK if any leg refuses).
    constexpr uint8_t GEAR_DOOR_ALL    = 0x03;
    /// `[down:u8]` → ACK / NACK.  Every configured strut's motor.
    constexpr uint8_t GEAR_STRUT_ALL   = 0x04;
}

/// Open/close action for GEAR_DOOR / GEAR_DOOR_ALL.
namespace GearDoorAction { constexpr uint8_t Close = 0; constexpr uint8_t Open = 1; }
/// Deploy/retract action for GEAR_STRUT / GEAR_STRUT_ALL (mirrors GearStepTarget).
namespace GearStrutAction { constexpr uint8_t Retract = 0; constexpr uint8_t Deploy = 1; }

/// Strut position, reported as a Rule-11 append on GEAR_STATUS_RESP +
/// GEAR_PHASE_EVENT so the host can GATE the manual controls (close-doors needs
/// Up; move-strut needs the doors open).  Independent of GearPhase because a
/// manual op or a Held/Unknown leaves GearPhase ambiguous about the strut end.
namespace GearStrutState {
    constexpr uint8_t Unknown = 0;   ///< position never established
    constexpr uint8_t Up      = 1;   ///< strut retracted / stowed (interlock: doors may close)
    constexpr uint8_t Out     = 2;   ///< strut deployed / extended
    constexpr uint8_t Moving  = 3;   ///< motor seeking an endstop
    inline const char* getName(uint8_t s) {
        switch (s) { case Up: return "up"; case Out: return "out";
                     case Moving: return "moving"; default: return "unknown"; }
    }
}

/// Target end for GEAR_STEP (mirrors Gear::Target).
namespace GearStepTarget {
    constexpr uint8_t Up   = 0;
    constexpr uint8_t Down = 1;
}

namespace GearAllAction {
    constexpr uint8_t Stop    = 0;
    constexpr uint8_t Deploy  = 1;
    constexpr uint8_t Retract = 2;
}

/// Stable + transitional phase of one strut, reported by `GEAR_STATUS_RESP`
/// and `GEAR_PHASE_EVENT`.  Target-driven FSM: the two STABLE states are
/// `Up` (gear up / stowed) and `Down` (gear down / extended); `MovingUp`/
/// `MovingDown` are in transit toward the target; `Unknown` = boot (position
/// never established) and `Held` = emergency-stopped (frozen in place) — both
/// accept either target to resume.  Wire byte values 1..5 are unchanged from
/// the old Retracted/Deploying/Deployed/Retracting/Error so a pre-refactor
/// host still decodes them; 6/7 are Rule-11 appends.
namespace GearPhase {
    constexpr uint8_t Unconfigured = 0;
    constexpr uint8_t Up           = 1;   ///< gear up / stowed (stable)
    constexpr uint8_t MovingDown   = 2;   ///< lowering (in transit → Down)
    constexpr uint8_t Down         = 3;   ///< gear down / extended (stable)
    constexpr uint8_t MovingUp     = 4;   ///< raising (in transit → Up)
    constexpr uint8_t Error        = 5;   ///< motor timeout/abort — GEAR_RESET to clear
    constexpr uint8_t Unknown      = 6;   ///< boot / position uncertain
    constexpr uint8_t Held         = 7;   ///< emergency-stopped (frozen)
    // Back-compat aliases (same byte values; shared code still uses these).
    constexpr uint8_t Retracted    = Up;
    constexpr uint8_t Deploying    = MovingDown;
    constexpr uint8_t Deployed     = Down;
    constexpr uint8_t Retracting   = MovingUp;

    inline const char* getName(uint8_t p) {
        switch (p) {
            case Unconfigured: return "unconfigured";
            case Up:           return "gear up";
            case MovingDown:   return "lowering";
            case Down:         return "gear down";
            case MovingUp:     return "raising";
            case Error:        return "error";
            case Unknown:      return "unknown";
            case Held:         return "held";
            default:           return "unknown";
        }
    }
    /// True for the two settled states a target can rest at.
    inline bool isStable(uint8_t p) { return p == Up || p == Down; }
}

/// Sub-phase inside a transit — the door/strut op-queue position (rich state
/// propagation).  Carried as the trailing byte of GEAR_STATUS_RESP +
/// GEAR_PHASE_EVENT.  Symmetric: the same positions are walked in both
/// directions (doors open → strut moves → doors close).
namespace GearSubPhase {
    constexpr uint8_t idle          = 0;   ///< settled (Up / Down / Error / Held / Unknown)
    constexpr uint8_t doors_opening = 1;   ///< doors opening
    constexpr uint8_t doors_open    = 2;   ///< doors open, awaiting strut leg
    constexpr uint8_t strut_moving  = 3;   ///< strut motor seeking the endstop
    constexpr uint8_t strut_done    = 4;   ///< strut reached, awaiting close
    constexpr uint8_t doors_closing = 5;   ///< doors closing
    constexpr uint8_t doors_closed  = 6;   ///< doors closed, about to settle
    // Back-compat aliases (same byte values).
    constexpr uint8_t motor_running = strut_moving;
    constexpr uint8_t motor_done    = strut_done;

    inline const char* getName(uint8_t s) {
        switch (s) {
            case idle:          return "idle";
            case doors_opening: return "doors-opening";
            case doors_open:    return "doors-open";
            case strut_moving:  return "strut-moving";
            case strut_done:    return "strut-done";
            case doors_closing: return "doors-closing";
            case doors_closed:  return "doors-closed";
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
    constexpr uint8_t NO_MOTOR         = 0x66;   ///< drove the strut but |I|≈0 → no motor / open circuit
    // ── Manual-control interlock rejections (firmware-enforced safety) ──
    constexpr uint8_t DOORS_CLOSED     = 0x67;   ///< manual strut move refused — doors not open
    constexpr uint8_t STRUT_NOT_UP     = 0x68;   ///< manual door-close refused — strut not retracted
    constexpr uint8_t GEAR_BUSY        = 0x69;   ///< manual op refused — a coordinated cycle / another manual op is in flight

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_ID:         return "Unknown gear id";
            case GEAR_TABLE_FULL:    return "Gear registry full";
            case MOTOR_UNAVAILABLE:  return "Gear motor unavailable";
            case IN_ERROR_STATE:     return "Gear in error state — issue GEAR_RESET to clear";
            case TIMEOUT:            return "Gear motor timed out";
            case NO_MOTOR:           return "No motor detected — drew no current under load (open circuit?)";
            case DOORS_CLOSED:       return "Open the doors before moving the strut";
            case STRUT_NOT_UP:       return "Retract the strut before closing the doors";
            case GEAR_BUSY:          return "Gear busy — wait for the current move to finish";
            default:                 return nullptr;
        }
    }
    /// Short tag for the Studio/CLI error pill (phase==Error).
    inline const char* getTag(uint8_t code) {
        switch (code) {
            case TIMEOUT:  return "timeout";
            case NO_MOTOR: return "no motor";
            default:       return "fault";
        }
    }
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEARCONTROL_PROTOCOL_H
