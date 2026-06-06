/*
 * Role wire protocol — runtime role attachment + per-role commands.
 *
 * A role is a behaviour ("LED animator", "DC motor", "heater", …) that
 * runs on top of one declared port.  Roles are attached and detached at
 * runtime by the hub (typically once during configuration), then driven
 * by per-role command opcodes.
 *
 * Each port can host at most one role at a time.  The role pool per
 * port-kind is fixed by the firmware's compile-time role variant
 * (declared in `sfx_board/server/port_registry.h`).
 *
 * Packet ranges (within the generic-expander 0x40..0x7F slice):
 *   0x40..0x47  attach / detach / enumeration
 *   0x48..0x4F  servo actuator role
 *   0x50..0x57  RC PWM input role (single-channel pulse on InputPort)
 *   0x58..0x5F  LED animator role
 *   0x60..0x67  DC motor role            (uni-directional, PwmPort)
 *   0x68..0x6F  Bi-directional DC motor  (HBridgePort)
 *   0x70..0x77  Heater role
 *   0x78..0x7B  SBUS input role          (16-ch + flags on InputPort)
 *   0x7C..0x7F  Jeti EX input role       (channels + telemetry on InputPort)
 *
 * Wire layout — see per-packet comments.
 */

#ifndef SFX_ROLES_PROTOCOL_H
#define SFX_ROLES_PROTOCOL_H

#include <cstdint>

// ============================================================================
// Role kinds — single-byte enum, append-only (Rule 11)
// ============================================================================

namespace RoleKind {
    constexpr uint8_t None          = 0x00;  ///< no role attached / detached sentinel
    constexpr uint8_t ServoActuator = 0x01;  ///< motion profile on a servo port (output mode)
    constexpr uint8_t RcPwmInput    = 0x02;  ///< single-channel pulse capture on an input port (RC PWM)
    constexpr uint8_t SbusInput     = 0x03;  ///< Futaba SBUS receiver on an input port (UART)
    constexpr uint8_t JetiExInput   = 0x04;  ///< Jeti Duplex EX Bus on an input port (UART half-duplex)
    constexpr uint8_t JetiExTelemetry = 0x05;  ///< Jeti EX Bus telemetry monitor on an input port — polls a downstream slave (e.g. ESC), feeds the master channel's responder (concentrator pass-through)
    constexpr uint8_t LedAnimator   = 0x10;  ///< event-queue LED animation on a PWM port
    constexpr uint8_t DcMotor       = 0x11;  ///< uni-directional motor with optional stall on a PWM port
    constexpr uint8_t Heater        = 0x12;  ///< bang-bang heater on a PWM port with temp sense
    constexpr uint8_t BiDcMotor     = 0x20;  ///< bi-directional motor on an H-bridge port
    // 0x21..0xFE reserved.
    constexpr uint8_t Reserved      = 0xFF;

    inline const char* getName(uint8_t kind) {
        switch (kind) {
            case None:          return "none";
            case ServoActuator: return "servo-actuator";
            case RcPwmInput:    return "rc-pwm-input";
            case SbusInput:     return "sbus-input";
            case JetiExInput:   return "jeti-ex-input";
            case JetiExTelemetry: return "jeti-ex-telemetry";
            case LedAnimator:   return "led-animator";
            case DcMotor:       return "dc-motor";
            case Heater:        return "heater";
            case BiDcMotor:     return "bi-dc-motor";
            default:            return "unknown";
        }
    }
}

// ============================================================================
// Wire packet types
// ============================================================================

namespace RolePacket {
    // ── Attach / detach / enumeration (0x40..0x47) ────────────────────
    constexpr uint8_t ROLE_ATTACH      = 0x40;
        ///< [portKind:u8][portIdx:u8][roleKind:u8][configLen:u8][config:configLen bytes]
        ///< → ACK / NACK.  Re-emplaces the port's role slot.
    constexpr uint8_t ROLE_DETACH      = 0x41;
        ///< [portKind:u8][portIdx:u8] → ACK / NACK.  Re-emplaces as None.
    constexpr uint8_t ROLE_LIST_REQ    = 0x42;  ///< [] → ROLE_LIST_RESP
    constexpr uint8_t ROLE_LIST_RESP   = 0x43;
        ///< [count:u8] × [portKind:u8][portIdx:u8][roleKind:u8][flags:u8]
    constexpr uint8_t ROLE_ATTACHED    = 0x44;
        ///< async TAG_ASYNC: [portKind:u8][portIdx:u8][roleKind:u8]
        ///< Fired by the slave immediately after a successful ROLE_ATTACH,
        ///< so masters watching for state changes don't have to poll
        ///< ROLE_LIST_REQ.  The payload mirrors the attach request
        ///< (config bytes are not echoed — query the role's STATUS for
        ///< current parameters).
    constexpr uint8_t ROLE_DETACHED    = 0x45;
        ///< async TAG_ASYNC: [portKind:u8][portIdx:u8]
        ///< Fired by the slave immediately after a successful
        ///< ROLE_DETACH (or after the keepalive watchdog clears state).

    /// Recoil impulse — adds a transient OFFSET to the servo's output for
    /// `duration_ms`, ON TOP of whatever the motion profile is doing (aiming or
    /// stationary), then auto-removes it ("de-jerks").  Applied at the role
    /// level so it never fights the aim (unlike the old instant-snap, which
    /// suppressed RC tracking).  GunFx fires one per shot with a random offset.
    /// 0x46 (the 0x48..0x4F servo block is full). Calls
    /// ServoActuatorRole::applyRecoil().
    constexpr uint8_t SERVO_RECOIL          = 0x46;  ///< [portIdx:u8][offset_us:i16LE][duration_ms:u16LE] → ACK

    /// Subscribe to the batched servo telemetry stream (SERVO_MOTION_UPDATE).
    /// GLOBAL (all servos), like RCIN_SET_BROADCAST_HZ is per-input. 0x47
    /// (0x48..0x4F is full). hz=0 turns it off. Clamped to a sane max server-side.
    constexpr uint8_t SERVO_SET_BROADCAST_HZ = 0x47;  ///< [hz:u8] → ACK (0 = off)

    // ── Servo actuator role (0x48..0x4F) ──────────────────────────────
    constexpr uint8_t SERVO_SET_TARGET      = 0x48;  ///< [portIdx:u8][target_us:u16LE] → ACK
    constexpr uint8_t SERVO_GET_STATUS_REQ  = 0x49;  ///< [portIdx:u8] → SERVO_STATUS_RESP
    constexpr uint8_t SERVO_STATUS_RESP     = 0x4A;  ///< [portIdx:u8][pos_us:u16LE][target_us:u16LE][vel:i16LE][flags:u8]
    constexpr uint8_t SERVO_TARGET_REACHED  = 0x4B;  ///< async TAG_ASYNC: [portIdx:u8][pos_us:u16LE]
    /// Live servo telemetry — BATCHED snapshot of every active servo, emitted
    /// periodically while subscribed (SERVO_SET_BROADCAST_HZ) and a host is
    /// listening (hostVerboseActive).  Generic / port-keyed (Rule 42): any UI
    /// domain that uses a servo consumes the same stream, independent of which
    /// effect owns it.  Gated off during uploads (RoleService::update() doesn't
    /// run while the loop is upload-exclusive).
    constexpr uint8_t SERVO_MOTION_UPDATE   = 0x4C;
        ///< async TAG_ASYNC: [count:u8]{ [portIdx:u8][pos_us:u16LE]
        ///< [target_us:u16LE][vel:i16LE] } × count
    /// Live motion-profile retune (Phase 2.9.x — actuator mechanism on
    /// the role layer, Rule 42).  Same wire shape as the role-attach
    /// payload tail — operator sliders push without re-attaching the
    /// role (which would lose target/position state).
    constexpr uint8_t SERVO_SET_PROFILE      = 0x4D;
        ///< [portIdx:u8][minUs:u16LE][maxUs:u16LE][maxSpeed:u16LE][reversed:u8]
        ///< [centerUs:u16LE][maxAccel:u16LE][maxJerk:u16LE] → ACK
    constexpr uint8_t SERVO_GET_PROFILE_REQ  = 0x4E;  ///< [portIdx:u8] → SERVO_PROFILE_RESP
    constexpr uint8_t SERVO_PROFILE_RESP     = 0x4F;
        ///< [portIdx:u8][minUs:u16LE][maxUs:u16LE][maxSpeed:u16LE][reversed:u8]
        ///< [centerUs:u16LE][maxAccel:u16LE][maxJerk:u16LE]

    // ── RC PWM / PPM input role (0x50..0x57) ──────────────────────────
    // The pulse-capture role decodes a PPM sum-signal (1..24 channels on
    // one wire); a plain single-channel RC PWM is just a 1-channel frame.
    constexpr uint8_t RCIN_GET_VALUE_REQ    = 0x50;  ///< [portIdx:u8] → RCIN_VALUE_RESP (channel 1)
    constexpr uint8_t RCIN_VALUE_RESP       = 0x51;  ///< [portIdx:u8][us:u16LE][valid:u8] (channel 1)
    constexpr uint8_t RCIN_SET_BROADCAST_HZ = 0x52;  ///< [portIdx:u8][hz:u8] → ACK (0 = off)
    constexpr uint8_t RCIN_VALUE_BROADCAST  = 0x53;  ///< async TAG_ASYNC: [portIdx:u8][us:u16LE][valid:u8] (legacy single-channel; decoded for back-compat)
    constexpr uint8_t PPM_FRAME_BROADCAST   = 0x54;  ///< async TAG_ASYNC: [portIdx:u8][count:u8][valid:u8][channels:u16LE × count]

    // ── Servo actuator role (overflow — 0x48..0x4F is full) ───────────
    // Servo-role command that didn't fit the 0x48..0x4F block (cf. SERVO_RECOIL
    // @0x46 / SERVO_SET_BROADCAST_HZ @0x47).  INTENT-layer "normalised position"
    // (Rule 42): a fraction in [0, kPosNormFull] (0 = calibrated MIN-µs end,
    // kPosNormFull = MAX-µs end) that the ROLE maps linearly onto its LIVE
    // calibrated [minUs,maxUs] and reflects for REV.  Decoupled from any input
    // unit — effects express "where in the travel" and the role owns the limits,
    // so a re-calibration is honoured on the next command with no plumbing.
    // GunFx yaw/pitch convert their RC pulse → fraction; landing sends full
    // (open end) / zero (close end).  See ServoActuatorRole::setNormalizedTarget().
    constexpr uint8_t SERVO_SET_POS_NORM    = 0x55;  ///< [portIdx:u8][pos:u16LE 0..10000] → ACK
    constexpr uint16_t kPosNormFull         = 10000; ///< pos == kPosNormFull → calibrated MAX end

    // ── LED animator role (0x58..0x5E) ────────────────────────────────
    constexpr uint8_t LED_QUEUE_LOAD        = 0x58;  ///< [portIdx:u8][count:u8] × event(N bytes) → ACK
    constexpr uint8_t LED_START             = 0x59;  ///< [portIdx:u8] → ACK
    constexpr uint8_t LED_STOP              = 0x5A;  ///< [portIdx:u8] → ACK
    constexpr uint8_t LED_SET_BRIGHTNESS    = 0x5B;  ///< [portIdx:u8][brightness:u8] → ACK (master scale)
    constexpr uint8_t LED_GET_STATUS_REQ    = 0x5C;  ///< [portIdx:u8] → LED_STATUS_RESP
    constexpr uint8_t LED_STATUS_RESP       = 0x5D;  ///< [portIdx:u8][brightness:u8][playing:u8][queueDepth:u8]
    constexpr uint8_t LED_QUEUE_DONE        = 0x5E;  ///< async TAG_ASYNC: [portIdx:u8]

    // ── BiDc motor — Strategy A move-to-end (range continuation) ──────
    // The BiMotor packet block (0x68..0x6F) is exhausted, so the
    // position-aware seek packet spills into the spare slot at the top
    // of the LED animator range — same precedent as MOTOR_SET_PCT (0x76)
    // spilling into the heater range.  Dispatch is by exact opcode in
    // RoleServicePolicy, not by range, so there is no actual conflict.
    constexpr uint8_t BIMOTOR_MOVE_TO_END   = 0x5F;
        ///< [portIdx:u8][endLabel:u8(1=A,2=B,0=Unknown)][signed_duty:i16LE][timeout_ms:u16LE] → ACK
        ///< Like BIMOTOR_SEEK_ENDSTOP but records `endLabel` so the role's
        ///< position state becomes that end on a Reached outcome.  Special
        ///< case — `signed_duty == 0` silently sets position = endLabel
        ///< WITHOUT moving (used to restore last-known position from
        ///< /hubfx.yaml on boot before any real move).

    // ── DC motor role (uni-directional, 0x60..0x67) ───────────────────
    constexpr uint8_t MOTOR_SET_DUTY        = 0x60;  ///< [portIdx:u8][duty:u16LE] → ACK
    constexpr uint8_t MOTOR_BRAKE           = 0x61;  ///< [portIdx:u8] → ACK
    constexpr uint8_t MOTOR_GET_STATUS_REQ  = 0x62;  ///< [portIdx:u8] → MOTOR_STATUS_RESP
    constexpr uint8_t MOTOR_STATUS_RESP     = 0x63;  ///< [portIdx:u8][duty:u16LE][v_mV:i16LE][i_mA:i16LE][stallFlags:u8]
    constexpr uint8_t MOTOR_STALL_EVENT     = 0x64;  ///< async TAG_ASYNC: [portIdx:u8][peak_mA:u16LE][duration_ms:u16LE]
    /// Live element-config retune (Phase 2.9.x — Rule 42).  Voltage
    /// scaling for sub-rail elements is updatable without re-attach.
    constexpr uint8_t MOTOR_SET_ELEMENT     = 0x65;  ///< [portIdx:u8][elementMv:u16LE][scaling:u8] → ACK
    constexpr uint8_t MOTOR_GET_ELEMENT_REQ = 0x66;  ///< [portIdx:u8] → MOTOR_ELEMENT_RESP
    constexpr uint8_t MOTOR_ELEMENT_RESP    = 0x67;  ///< [portIdx:u8][elementMv:u16LE][scaling:u8][portRailMv:u16LE]

    // ── Bi-directional DC motor role (0x68..0x6F) ─────────────────────
    constexpr uint8_t BIMOTOR_SET_SIGNED      = 0x68;  ///< [portIdx:u8][signed_duty:i16LE] → ACK
    constexpr uint8_t BIMOTOR_BRAKE           = 0x69;  ///< [portIdx:u8] → ACK
    constexpr uint8_t BIMOTOR_COAST           = 0x6A;  ///< [portIdx:u8] → ACK
    constexpr uint8_t BIMOTOR_GET_STATUS_REQ  = 0x6B;  ///< [portIdx:u8] → BIMOTOR_STATUS_RESP
    /// Rule 11 extension 2026-05-24: STATUS_RESP gained two optional trailing
    /// bytes — `[position:u8][guardMode:u8]` — so callers can read the
    /// Strategy A position state and confirm which stall mode is active.
    /// Length 8 = original payload; length 10 = extended.  Older masters
    /// safely ignore the trailing bytes.
    constexpr uint8_t BIMOTOR_STATUS_RESP     = 0x6C;
        ///< [portIdx:u8][signed_duty:i16LE][v_mV:i16LE][i_mA:i16LE][stallFlags:u8]
        ///<   {Rule 11 ext} [position:u8][guardMode:u8]
    constexpr uint8_t BIMOTOR_STALL_EVENT     = 0x6D;  ///< async TAG_ASYNC: [portIdx:u8][peak_mA:u16LE][duration_ms:u16LE]
    /// Autonomous "drive until endstop" seek.  The role drives `signed_duty`
    /// until it detects a stall (= endstop reached) or `timeout_ms` elapses,
    /// then BRAKES locally — the stop decision never crosses the wire.
    /// `timeout_ms == 0` ⇒ no timeout (seek until stall or abort).  Aborted
    /// by any BIMOTOR_BRAKE / COAST / SET_SIGNED while seeking.
    /// Position-agnostic — use BIMOTOR_MOVE_TO_END for Strategy A.
    constexpr uint8_t BIMOTOR_SEEK_ENDSTOP    = 0x6E;  ///< [portIdx:u8][signed_duty:i16LE][timeout_ms:u16LE] → ACK
    /// async TAG_ASYNC result of a SEEK_ENDSTOP / MOVE_TO_END: outcome
    /// 0=reached(stall), 1=timeout, 2=aborted.  travel_ms = time spent
    /// seeking; peak_mA = peak current during the confirming stall window
    /// (0 on timeout/abort).
    /// Rule 11 extension 2026-05-24: optional trailing `[position:u8]` so
    /// the master sees the resulting endstop label without a follow-up
    /// STATUS query.  Length 6 = original; length 7 = extended.
    constexpr uint8_t BIMOTOR_ENDSTOP_RESULT  = 0x6F;
        ///< async TAG_ASYNC: [portIdx:u8][outcome:u8][travel_ms:u16LE][peak_mA:u16LE]
        ///<   {Rule 11 ext} [position:u8(0=Unknown,1=A,2=B)]

    // ── Heater role (0x70..0x77) ──────────────────────────────────────
    constexpr uint8_t HEATER_SET_TARGET       = 0x70;  ///< [portIdx:u8][target_cx10:i16LE] → ACK
    constexpr uint8_t HEATER_GET_STATUS_REQ   = 0x71;  ///< [portIdx:u8] → HEATER_STATUS_RESP
    constexpr uint8_t HEATER_STATUS_RESP      = 0x72;  ///< [portIdx:u8][target_cx10:i16LE][actual_cx10:i16LE][duty:u16LE][flags:u8]
    /// Live element-config retune (Phase 2.9.x — Rule 42).  Same shape
    /// as MOTOR_SET_ELEMENT — element_mv + scaling + drivePct + hyst.
    constexpr uint8_t HEATER_SET_ELEMENT      = 0x73;
        ///< [portIdx:u8][elementMv:u16LE][scaling:u8][drivePct:u8][hyst_cx10:i16LE] → ACK
    constexpr uint8_t HEATER_GET_ELEMENT_REQ  = 0x74;  ///< [portIdx:u8] → HEATER_ELEMENT_RESP
    constexpr uint8_t HEATER_ELEMENT_RESP     = 0x75;
        ///< [portIdx:u8][elementMv:u16LE][scaling:u8][drivePct:u8][hyst_cx10:i16LE][portRailMv:u16LE]

    // ── DC motor intent-layer driver (continuation slot — the DC motor's
    //    primary range 0x60..0x67 is exhausted, so MOTOR_SET_PCT lives
    //    in the spare slot at the top of the heater range).
    //    Rule 42 intent surface: "drive at N % of the element's rated
    //    voltage".  The role internally applies scaleDuty() to produce
    //    the actual port-native duty.  MOTOR_SET_DUTY (0x60) stays as
    //    the raw port-level bypass for advanced callers.
    constexpr uint8_t MOTOR_SET_PCT           = 0x76;
        ///< [portIdx:u8][pct:u8 (0..100)] → ACK

    // ── BiDc motor stall-guard retune (range continuation) ────────────
    // BiMotor 0x68..0x6F is exhausted; SET_GUARD lives at the heater-
    // range tail (same precedent as MOTOR_SET_PCT @ 0x76 + BIMOTOR_MOVE_
    // TO_END @ 0x5F).  Live retune lets a Studio calibration dialog flip
    // between Fixed / LiveRatio modes and adjust thresholds without
    // re-attaching the role (which would destroy position + seek state).
    constexpr uint8_t BIMOTOR_SET_GUARD       = 0x77;
        ///< [portIdx:u8][mode:u8(0=Fixed,1=LiveRatio)][window_ms:u16LE]
        ///< [a:u16LE][b:u16LE][c:u16LE][d:u16LE] → ACK
        ///<   Fixed:     a = threshold_mA; b/c/d ignored
        ///<   LiveRatio: a = ratio_x100 (e.g. 250 = 2.5×)
        ///<              b = runSample_ms
        ///<              c = inrushBlank_ms
        ///<              d = maxTravel_ms (failsafe; 0 = use per-seek timeout)
        ///< window_ms is the sustained-over-threshold filter, shared by
        ///< both modes (0 = leave unchanged).

    // ── BiDc motor dual-stage soft-start probe (uses the free 0x56 slot) ──
    // Before a seek applies full power it can drive a LOW-power probe first to
    // classify free-vs-already-at-stop (DC-motor end-stop spec).  Orthogonal to
    // the stall guard above — both apply.  probePct 0 = probe disabled.
    constexpr uint8_t BIMOTOR_SET_PROBE       = 0x56;
        ///< [portIdx:u8][probePct:u8][window_ms:u16LE][dropPct:u8] → ACK
        ///<   probePct = probe drive as % of seek duty (0 = disable probe)
        ///<   window_ms = classification window (0 → default 250)
        ///<   dropPct  = "free" if I falls below dropPct% of probe peak (0 → 70)

    // ── SBUS input role (0x78..0x7B) ──────────────────────────────────
    constexpr uint8_t SBUS_GET_FRAME_REQ      = 0x78;  ///< [portIdx:u8] → SBUS_FRAME_RESP
    constexpr uint8_t SBUS_FRAME_RESP         = 0x79;
        ///< [portIdx:u8][count:u8][flags:u8][channels:u16LE × count]
        ///< flags: bit0=valid, bit1=failsafe, bit2=frameLost, bit3=ch17, bit4=ch18
    constexpr uint8_t SBUS_SET_BROADCAST_HZ   = 0x7A;  ///< [portIdx:u8][hz:u8] → ACK (0 = off)
    constexpr uint8_t SBUS_FRAME_BROADCAST    = 0x7B;
        ///< async TAG_ASYNC: [portIdx:u8][count:u8][flags:u8][channels:u16LE × count]

    // ── Jeti EX input role (0x7C..0x7F) ───────────────────────────────
    constexpr uint8_t JETIEX_GET_FRAME_REQ    = 0x7C;  ///< [portIdx:u8] → JETIEX_FRAME_RESP
    constexpr uint8_t JETIEX_FRAME_RESP       = 0x7D;
        ///< [portIdx:u8][count:u8][valid:u8][rxFrames:u32LE][rxErrors:u32LE]
        ///< [channels:u16LE × count]
    constexpr uint8_t JETIEX_SET_BROADCAST_HZ = 0x7E;  ///< [portIdx:u8][hz:u8] → ACK (0 = off)
    constexpr uint8_t JETIEX_FRAME_BROADCAST  = 0x7F;
        ///< async TAG_ASYNC: [portIdx:u8][count:u8][valid:u8][channels:u16LE × count]
}

/// Outcome byte in BIMOTOR_ENDSTOP_RESULT.
namespace BiMotorSeekOutcome {
    constexpr uint8_t Reached = 0;   ///< stall hit = endstop reached
    constexpr uint8_t Timeout = 1;   ///< timeout elapsed before any stall
    constexpr uint8_t Aborted = 2;   ///< cancelled by BRAKE / COAST / SET_SIGNED

    inline const char* getName(uint8_t o) {
        switch (o) {
            case Reached: return "reached";
            case Timeout: return "timeout";
            case Aborted: return "aborted";
            default:      return "?";
        }
    }
}

// ============================================================================
// Role-layer error codes (range 0x40..0x4F inside the generic SerialError space)
// ============================================================================

namespace RoleError {
    constexpr uint8_t ROLE_NOT_ATTACHED        = 0x40;  ///< command for an empty role slot
    constexpr uint8_t ROLE_KIND_MISMATCH       = 0x41;  ///< command opcode doesn't match attached role kind
    constexpr uint8_t ROLE_KIND_NOT_SUPPORTED  = 0x42;  ///< role kind can't run on this port kind
    constexpr uint8_t ROLE_CONFIG_INVALID      = 0x43;  ///< attach config payload rejected
    constexpr uint8_t ROLE_NO_SENSE_REQUIRED   = 0x44;  ///< role needs a sense channel the port doesn't have
    constexpr uint8_t ROLE_QUEUE_FULL          = 0x45;  ///< event queue overflow

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case ROLE_NOT_ATTACHED:       return "Role not attached";
            case ROLE_KIND_MISMATCH:      return "Role kind mismatch";
            case ROLE_KIND_NOT_SUPPORTED: return "Role kind not supported on this port";
            case ROLE_CONFIG_INVALID:     return "Role config payload invalid";
            case ROLE_NO_SENSE_REQUIRED:  return "Role requires a sense channel this port lacks";
            case ROLE_QUEUE_FULL:         return "Role queue full";
            default:                      return nullptr;
        }
    }
}

#endif  // SFX_ROLES_PROTOCOL_H
