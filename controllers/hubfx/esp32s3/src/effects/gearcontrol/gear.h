/*
 * gear.h — target-driven single-strut state machine for the GearControl effect.
 *
 *   Drives one H-bridge motor + ≤2 door servos through topology.  Same
 *   architectural pattern as `LandingLight`: construction installs callbacks
 *   the owning service binds to topology's `sendRoleCommand` + batch
 *   primitives + phase broadcast.  No transport leaks into the FSM.
 *
 *   Each strut owns a TARGET (`Up` / `Down`) and walks a single symmetric
 *   transit toward it (doors open → strut moves → doors close):
 *
 *     settled ⇄ doors_opening ⇄ doors_open ⇄ strut_moving ⇄ strut_done
 *             ⇄ doors_closing ⇄ doors_closed ⇄ settled
 *
 *   `pump()` is the only engine: from `_sub` (transit position) + `_target`
 *   it commands the next leg, parks (single-step mode), settles, or REVERSES.
 *   Reversal is not a special path — `pump()` recomputes when `_seekDir !=
 *   _target`.  So setting the opposite target mid-cycle pre-empts cleanly:
 *   doors already open → just seek the new way; strut mid-seek → re-issue the
 *   seek the other way (the BiDcMotor role overwrites cleanly, no brake); doors
 *   closing → re-open then reverse.
 *
 *   Completion: door legs wait on SERVO_MOTION_DONE (monitored) with a travel-
 *   timeout backstop re-armed on every leg entry; the motor leg waits on
 *   BIMOTOR_ENDSTOP_RESULT (Timeout → ERROR).
 *
 *   The coordinator drives synchronous modes via `stepToward()` (advance one
 *   leg + park) + `legComplete()` (barrier predicate); Independent uses the
 *   full-cycle `setTarget()`.  Boot state is `Unknown` (position uncertain);
 *   `emergencyHold()` brakes + freezes in place (→ `Held`).
 *
 *   Status LEDs are NOT driven from here — the GearControl expander board
 *   lights its own per-motor direction LEDs locally.
 */

#ifndef HUBFX_GEAR_H
#define HUBFX_GEAR_H

#include <cstdint>

#include "../effect_id.h"
#include "gearcontrol_protocol.h"

namespace hubfx::effects::gearctrl {

// ── Shared callback type (used by Gear + DoorSequencer) ───────────────
using SendRoleCmdFn = bool (*)(void* ctx, const PortRef& addr,
                               uint8_t innerType,
                               const uint8_t* payload, size_t len);

/// One door servo on a gear — a ServoActuator role addressed by PortRef.
/// A door drives its servo to the calibrated MAX-µs end on OPEN
/// (kPosNormFull) and the MIN-µs end on CLOSE (0); the servo role honours
/// its own REV flag + calibration for the travel + direction.  No per-door
/// position — same parametrisation as a landing-light servo.
struct DoorDef {
    PortRef  servo;                ///< ServoActuator role port; empty = none
};

struct GearDef {
    uint8_t  id            = 0;
    char     name[16]      = {};
    PortRef  motor;                            ///< must have BiDcMotor role

    // Motor speed when running (-32767..32767 signed duty).  Negative
    // values reverse the H-bridge polarity at runtime.
    int16_t  deployDuty    = +20000;            ///< signed duty for "going down"
    int16_t  retractDuty   = -20000;            ///< signed duty for "going up"
    uint32_t timeoutMs     = 30000;             ///< full-travel timeout

    // ── Stall guard (persisted calibration; pushed to the motor before each
    //    seek via BIMOTOR_SET_GUARD, so a saved calibration drives real
    //    deploy/retract — not the role's hardcoded default).  Defaults mirror
    //    the BiDcMotorRole LiveRatio defaults. ─────────────────────────────
    uint8_t  guardMode       = 1;       ///< 0 = Fixed, 1 = LiveRatio
    uint16_t guardRatioX100  = 250;     ///< LiveRatio: trip ratio ×100 (250 = 2.5×)
    uint16_t guardSampleMs   = 200;     ///< LiveRatio: running-current sample window
    uint16_t guardWindowMs   = 80;      ///< sustained-over-threshold confirm window
    uint16_t guardThresholdMa = 1000;   ///< Fixed: |I| trip threshold (mA)
    uint16_t guardCeilingMa  = 0;       ///< absolute over-current ceiling (0 = none)

    // ── Door sequencing (instructions/29) ────────────────────────────
    DoorDef  doors[2]      = {};
    uint8_t  numDoors      = 0;
    uint8_t  openMode      = DoorMode::DUAL_SYNC;   ///< door-pair sequencing (OPEN leg)
    uint16_t doorDelayMs   = 500;                   ///< DUAL_DELAY only
    uint8_t  closePolicy   = ClosePolicy::BOTH;     ///< post-deploy close policy
};

}  // namespace hubfx::effects::gearctrl

#include "door_sequencer.h"   // needs DoorDef + SendRoleCmdFn above

namespace hubfx::effects::gearctrl {

class Gear {
public:
    using SendRoleCmdFn = gearctrl::SendRoleCmdFn;
    using BatchFn       = void (*)(void* ctx);
    /// Phase-change broadcast — fired on overall-phase OR sub-phase change.
    /// `errReason` is the GearError code while phase==Error (0 otherwise).
    using PhaseEventFn  = void (*)(void* ctx, uint8_t id, uint8_t newPhase,
                                   uint8_t newSubPhase, uint8_t errReason);

    /// The two stable states a strut drives toward.
    enum class Target : uint8_t { Up = 0, Down = 1 };

    /// Outcome of a MANUAL door/strut command — the service maps it to ACK or a
    /// NACK with the matching GearError (the firmware is the authoritative
    /// interlock; the GUI gate only mirrors it).
    enum class ManualResult : uint8_t {
        Ok = 0,        ///< accepted + driving
        Busy,          ///< a coordinated cycle / another manual op is in flight
        DoorsClosed,   ///< strut move refused — doors not open
        StrutNotUp,    ///< door close refused — strut not retracted
    };

    Gear() = default;

    void configure(const GearDef& def,
                   SendRoleCmdFn sendFn, void* sendCtx,
                   BatchFn beginFn, BatchFn commitFn,
                   PhaseEventFn phaseFn = nullptr, void* phaseCtx = nullptr) {
        _def      = def;
        _send     = sendFn;
        _sendCtx  = sendCtx;
        _begin    = beginFn;
        _commit   = commitFn;
        _phaseEv  = phaseFn;
        _phaseCtx = phaseCtx;
        // Initial-start: the physical position is UNKNOWN at boot.  The first
        // setTarget/stepToward homes the strut (a full seek toward the target;
        // the stall guard trips at whichever endstop it's already against).
        _phase    = GearPhase::Unknown;
        _sub      = GearSubPhase::idle;
        _target   = Target::Up;
        _seekDir  = Target::Up;
        _errorReason = 0;
        _stepMode = false;
        _stepArmed= false;
        _legDone  = false;
        _strutState = GearStrutState::Unknown;   // position uncertain until first seek
        _manualLeg  = ManualLeg::None;
        _movingDeadlineMs = 0;
        _doorDeadlineMs   = 0;
        _doorSeq.configure(_def.doors, _def.numDoors, _def.openMode,
                           _def.doorDelayMs, _def.closePolicy,
                           sendFn, sendCtx);
    }

    const GearDef& def() const { return _def; }
    uint8_t  id()          const { return _def.id; }
    uint8_t  phase()       const { return _phase; }
    uint8_t  subPhase()    const { return _sub; }
    /// GearError code behind an Error phase (0 when not in Error).
    uint8_t  errorReason() const { return _errorReason; }

    // ── Commands ──────────────────────────────────────────────────────
    /// Full-cycle: drive autonomously to `t` (Up/Down).  Pre-empts a transit
    /// already in flight and reverses cleanly (the FSM recomputes from its
    /// current position).
    void setTarget(Target t);
    /// Single-step (coordinator sync modes): advance toward `t` by ONE leg,
    /// then PARK at the next boundary (doors-open / strut-done / settled).
    /// The coordinator calls it repeatedly to keep struts in lockstep.
    void stepToward(Target t);
    /// Emergency hold: brake the motor + freeze the doors in place; phase →
    /// HELD (position uncertain).  Resumes on the next setTarget/stepToward.
    void emergencyHold();
    /// Clear an ERROR state → UNKNOWN so commands are accepted again.  No-op
    /// when not in ERROR.  (GEAR_RESET.)
    void clearError();

    // ── Manual / maintenance: drive ONE subsystem independently (setup) ───
    /// Open (`open=true`) or close the doors only, honouring the configured
    /// door-mode + close-policy.  INTERLOCK: closing requires the strut UP.
    /// Refuses while a coordinated cycle / another manual op is in flight.
    ManualResult manualDoors(bool open);
    /// Deploy (`down=true`) or retract the strut only.  INTERLOCK: either
    /// direction requires the doors OPEN (the leg passes through the door gap).
    ManualResult manualStrut(bool down);

    /// Dry-run the same interlock without driving — lets a FLEET command refuse
    /// all-or-nothing (no partial application) before any leg moves.
    ManualResult checkManualDoors(bool open) const;
    ManualResult checkManualStrut(bool down) const;

    /// Interlock read-outs (also surfaced on the wire for the GUI gate):
    /// `doorsOpen()` = every configured door at its open end (or no doors);
    /// `strutState()` = GearStrutState (Unknown/Up/Out/Moving).
    bool    doorsOpen()  const { return _doorSeq.allAtOpen(); }
    uint8_t strutState() const { return _strutState; }
    bool    strutUp()    const { return _strutState == GearStrutState::Up; }

    // ── Coordinator predicates ────────────────────────────────────────
    /// True while a transit is in flight (raising or lowering).
    bool isCycling() const {
        return _phase == GearPhase::MovingUp || _phase == GearPhase::MovingDown;
    }
    /// Settled at the commanded target.
    bool atTarget() const { return settledAtTarget(); }
    /// True when the current leg has parked at a boundary (or settled / error /
    /// held) — the sync coordinator waits on this before stepping all struts.
    bool legComplete() const;

    /// Service tick — drives the door timers + the silent-expander backstop.
    void update(uint32_t nowMs);

    /// BIMOTOR_ENDSTOP_RESULT for this strut's motor port (GUID-matched by the
    /// service).  `outcome`: 0=reached, 1=timeout, 2=aborted.
    void onEndstopResult(uint8_t outcome);
    /// SERVO_MOTION_DONE for one of this strut's door ports (GUID-matched).
    void onServoMotionDone(uint8_t portIdx);

private:
    // The single engine: given `_sub` (transit position) + `_target`, command
    // the next leg, park, settle, or reverse.  The ONLY place that issues role
    // commands + emits phase events.  Every command/event funnels here.
    void pump();

    void setPhaseSub(uint8_t newPhase, uint8_t newSub);
    void enterError(uint8_t reason);   ///< brake + → Error with a GearError reason
    void startSeek();          ///< push guard + seek toward _target; arm motor backstop; set _seekDir
    void openDoors();          ///< _doorSeq.open() + arm door backstop
    void closeDoors();         ///< close per policy (Down) / re-stow (Up) + arm backstop
    void reverseDoors();       ///< _doorSeq.reverse() (re-open) + arm backstop
    void armDoorBackstop();
    void armMotorBackstop();
    void commandSeek(int16_t signedDuty);
    /// Manual-op completion: settle the door/strut state + clear the manual leg.
    void finishManualDoor(bool opened);
    void finishManualStrut();
    /// Map _strutState → the overall GearPhase reported on the wire (so a manual
    /// op shows a consistent phase: strut Up→Up, Out→Down, Moving→MovingX).
    uint8_t strutPhase() const;
    void commandGuard();                ///< push the persisted stall guard pre-seek
    void commandMotorBrake();

    bool settledAtTarget() const {
        return _sub == GearSubPhase::idle &&
               ((_target == Target::Down && _phase == GearPhase::Down) ||
                (_target == Target::Up   && _phase == GearPhase::Up));
    }
    uint8_t movingPhase() const {
        return (_target == Target::Down) ? GearPhase::MovingDown : GearPhase::MovingUp;
    }
    int16_t seekDuty() const {
        return (_target == Target::Down) ? _def.deployDuty : _def.retractDuty;
    }

    GearDef       _def{};
    uint8_t       _phase   = GearPhase::Unconfigured;
    uint8_t       _sub     = GearSubPhase::idle;
    Target        _target  = Target::Up;       ///< desired stable state
    Target        _seekDir = Target::Up;       ///< endstop the strut last sought / is at
    uint8_t       _errorReason = 0;            ///< GearError code while phase==Error
    bool          _stepMode  = false;          ///< true → coordinator drives leg-by-leg
    bool          _stepArmed = false;          ///< step-mode: permission to cross ONE boundary
    bool          _legDone   = false;          ///< current leg's completion event fired

    /// Explicit strut position (independent of the transit `_phase`), so the
    /// manual close-doors interlock + the wire status have a reliable "strut up
    /// vs out" answer even after a Held/Unknown/manual op.  Set on every
    /// confirmed endstop reach; Unknown at boot/error/hold.
    uint8_t       _strutState = GearStrutState::Unknown;
    /// Which independent manual op (if any) is in flight — routes the completion
    /// async to the manual finisher instead of the coordinated `pump()`.
    enum class ManualLeg : uint8_t { None, DoorOpen, DoorClose, StrutDown, StrutUp };
    ManualLeg     _manualLeg = ManualLeg::None;

    uint32_t      _movingDeadlineMs = 0;       ///< EffectClock deadline for the motor seek backstop
    uint32_t      _doorDeadlineMs   = 0;       ///< EffectClock deadline for a door leg (missed SERVO_MOTION_DONE backstop)

    /// A servo has no position feedback, so a door leg completes on the
    /// SERVO_MOTION_DONE async — but that event can be lost (lossy broadcast,
    /// a no-op move, a flaky link), hanging the whole sequence.  Every door leg
    /// (including a reversal) re-arms this backstop on entry.
    static constexpr uint32_t kDoorTravelTimeoutMs = 4000;

    DoorSequencer _doorSeq{};

    SendRoleCmdFn _send     = nullptr;
    void*         _sendCtx  = nullptr;
    BatchFn       _begin    = nullptr;
    BatchFn       _commit   = nullptr;
    PhaseEventFn  _phaseEv  = nullptr;
    void*         _phaseCtx = nullptr;
};

}  // namespace hubfx::effects::gearctrl

#include "gear.ipp"

#endif  // HUBFX_GEAR_H
