/*
 * gear.h — single-gear state machine for the GearControl effect.
 *
 *   Drives one H-bridge motor + ≤2 door servos through topology
 *   (instructions/29).  Same architectural pattern as `LandingLight`:
 *   construction installs callbacks that the owning service binds to
 *   topology's `sendRoleCommand` + batch primitives + phase broadcast.
 *   No transport / no template-pack knowledge leaks into the state
 *   machine itself.
 *
 *   Deploy / retract run an OP-QUEUE that brackets the motor seek with
 *   door legs:
 *
 *     deploy  : OPEN_DOORS → [SYNC] → RUN_MOTOR(deployDuty) → [SYNC] → CLOSE_DOORS(policy)
 *     retract : OPEN_DOORS → [SYNC] → RUN_MOTOR(retractDuty) → [SYNC] → CLOSE_DOORS(all)
 *
 *   Each leg fully completes before the next starts:
 *     - door legs wait on SERVO_MOTION_DONE per commanded door (monitored,
 *       decision #1 — never a timer);
 *     - the motor leg waits on BIMOTOR_ENDSTOP_RESULT (the existing seek).
 *
 *   The SYNC barriers are no-ops under `Independent` coordination; under
 *   DoorSync / FullSync / Sequenced the SERVICE coordinator parks the gear
 *   at `isWaitingDoorsOpenBarrier()` / `isWaitingMotorDoneBarrier()` and
 *   releases all gears together via `advanceBarrier()`.
 *
 *   Motion uses the BiDcMotor role's autonomous endstop seek
 *   (`BIMOTOR_SEEK_ENDSTOP`): the gear sends one seek per motor leg and the
 *   expander drives → detects the stall → brakes locally, then reports
 *   `BIMOTOR_ENDSTOP_RESULT`.  The seek's own timeout (= `timeoutMs`, 0 =
 *   none) is authoritative; a `Timeout` outcome → ERROR.
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
/// Open/close are NORMALISED positions [0..kPosNormFull] (Rule: servo
/// intent is normalised); the role honours its REV flag + calibration.
struct DoorDef {
    PortRef  servo;                ///< ServoActuator role port; empty = none
    uint16_t openNorm  = 10000;    ///< normalised open position  [0..10000]
    uint16_t closeNorm = 0;        ///< normalised closed position [0..10000]
};

struct GearDef {
    uint8_t  id            = 0;
    char     name[16]      = {};
    PortRef  motor;                            ///< must have BiDcMotor role

    // Motor speed when running (-32767..32767 signed duty).  Negative
    // values reverse the H-bridge polarity at runtime.
    int16_t  deployDuty    = +20000;            ///< signed duty for "going down"
    int16_t  retractDuty   = -20000;            ///< signed duty for "going up"
    uint32_t timeoutMs     = 4000;              ///< full-travel timeout

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
    using PhaseEventFn  = void (*)(void* ctx, uint8_t id,
                                   uint8_t newPhase, uint8_t newSubPhase);

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
        _phase    = phaseFn;
        _phaseCtx = phaseCtx;
        _state    = GearPhase::Retracted;
        _sub      = GearSubPhase::idle;
        _movingDeadlineMs = 0;
        _doorSeq.configure(_def.doors, _def.numDoors, _def.openMode,
                           _def.doorDelayMs, _def.closePolicy,
                           sendFn, sendCtx);
    }

    const GearDef& def() const { return _def; }
    uint8_t  id()       const { return _def.id; }
    uint8_t  phase()    const { return _state; }
    uint8_t  subPhase() const { return _sub; }

    /// `coordRequiresSync` selects whether this gear inserts SYNC barriers
    /// at the door+motor boundaries (set per-cycle by the service from the
    /// global CoordMode).  Independent / Sequenced run without barriers.
    void setSyncBarriers(bool atDoors, bool atMotor) {
        _syncDoors = atDoors;
        _syncMotor = atMotor;
    }

    void deploy();
    void retract();
    void stop();

    /// Clear an ERROR state → RETRACTED so deploy/retract are accepted
    /// again.  No-op when not in ERROR.  (GEAR_RESET.)
    void clearError();

    /// True while a deploy/retract op-queue is in flight (any non-settled
    /// sub-phase).  Used by the multi-gear coordinator.
    bool isCycling() const {
        return _state == GearPhase::Deploying || _state == GearPhase::Retracting;
    }

    /// Barrier predicates — the gear parks here until the coordinator calls
    /// advanceBarrier().  Only meaningful when the matching sync flag is set.
    bool isWaitingDoorsOpenBarrier() const {
        return isCycling() && _sub == GearSubPhase::doors_open && _atDoorsBarrier;
    }
    bool isWaitingMotorDoneBarrier() const {
        return isCycling() && _sub == GearSubPhase::motor_done && _atMotorBarrier;
    }

    /// Coordinator-driven barrier release — advances past the current SYNC
    /// barrier (doors-open → motor, or motor-done → close).
    void advanceBarrier();

    /// Service tick — call once per main loop with the current time.
    /// Drives the DUAL_DELAY door timer + the silent-expander backstop.
    void update(uint32_t nowMs);

    /// Forwarded by the service when BIMOTOR_ENDSTOP_RESULT arrives for this
    /// gear's motor port.  `outcome` is RolePacket::BiMotorSeekOutcome
    /// (0=reached, 1=timeout, 2=aborted).
    void onEndstopResult(uint8_t outcome);

    /// Forwarded by the service when SERVO_MOTION_DONE arrives for one of
    /// this gear's door ports (already GUID-matched).
    void onServoMotionDone(uint8_t portIdx);

private:
    // ── Op-queue ─────────────────────────────────────────────────────
    // The deploy/retract cycle is a fixed 5-slot queue; legs that don't
    // apply (no doors, no barriers) collapse on entry.
    enum class Leg : uint8_t {
        None        = 0,
        OpenDoors   = 1,
        SyncDoors   = 2,   ///< barrier after open
        RunMotor    = 3,
        SyncMotor   = 4,   ///< barrier after motor
        CloseDoors  = 5,
        Done        = 6,
    };

    void startCycle(bool deploying);
    void beginLeg(Leg leg);
    void advanceLeg();
    void finishCycle();
    void enterError();

    void setPhaseSub(uint8_t newPhase, uint8_t newSub);
    void commandSeek(int16_t signedDuty);
    void commandMotorBrake();
    void armBackstop(uint32_t nowMs);

    GearDef       _def{};
    uint8_t       _state            = GearPhase::Unconfigured;
    uint8_t       _sub              = GearSubPhase::idle;
    uint32_t      _movingDeadlineMs = 0;       ///< EffectClock deadline for the motor seek backstop

    DoorSequencer _doorSeq{};

    // Active-cycle state.
    bool          _deploying        = false;
    Leg           _leg              = Leg::None;
    bool          _syncDoors        = false;   ///< coord wants a doors-open barrier
    bool          _syncMotor        = false;   ///< coord wants a motor-done barrier
    bool          _atDoorsBarrier   = false;   ///< parked at the doors-open barrier
    bool          _atMotorBarrier   = false;   ///< parked at the motor-done barrier

    SendRoleCmdFn _send     = nullptr;
    void*         _sendCtx  = nullptr;
    BatchFn       _begin    = nullptr;
    BatchFn       _commit   = nullptr;
    PhaseEventFn  _phase    = nullptr;
    void*         _phaseCtx = nullptr;
};

}  // namespace hubfx::effects::gearctrl

#include "gear.ipp"

#endif  // HUBFX_GEAR_H
