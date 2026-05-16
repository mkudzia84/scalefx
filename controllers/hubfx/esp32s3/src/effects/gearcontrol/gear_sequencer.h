/*
 * GearSequencer (master-side) — op-queue driven deploy/retract sequencer.
 *
 * Ported from controllers/gearcontrol/pico/src/gear_sequencer.{h,cpp}
 * (slave-side).  Key adaptations for the new master/slave split:
 *
 *   - Door subsystem reference is the master-side DoorSequencer
 *     (sibling header — uses ServoCommandFn stubs internally).
 *   - The slave-side `StallDetector*` dependency is replaced with a
 *     pair of stub callbacks that the orchestrator wires to the
 *     PWM-stall observer chain on SlaveApi:
 *       - MotorFn      drives PWM_RECONFIGURE / PWM_SET_DUTY for the
 *                      retract motor (deploy = +1, retract = -1, stop = 0)
 *       - StallStateFn returns the latest stall verdict from the
 *                      slave's PWM stall reports (RUNNING / CONFIRMED /
 *                      TIMEOUT / NO_MOTOR / TIMEOUT_ERROR)
 *     This keeps GearSequencer agnostic of how the master observes the
 *     slave bus — the orchestrator owns that wiring and is free to
 *     change the underlying stall-detection strategy without touching
 *     the sequencer.
 *
 * Op-queue design preserved verbatim:
 *   OPEN_DOORS -> [SYNC_BARRIER] -> RUN_MOTOR -> [SYNC_BARRIER] -> CLOSE_DOORS
 *
 * Preemption preserved: deploy during retract (or vice versa) re-builds
 * the queue while handling the in-flight op safely (motor stop, door
 * reverse).  See gear_sequencer.cpp for the preemption table.
 *
 * Sync mode preserved: SYNC_BARRIERs let an upper-level coordinator
 * (e.g., a multi-gear "all gears deploy simultaneously" flow) hold the
 * sequence between phases.  Coordinator calls advanceSyncPhase() to
 * release the barrier.
 */

#ifndef HUBFX_GEARCONTROL_GEAR_SEQUENCER_H
#define HUBFX_GEARCONTROL_GEAR_SEQUENCER_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include "door_sequencer.h"

namespace hubfx::effects::gearcontrol {

// ── Public types ─────────────────────────────────────────────────────

enum class GearSeqStep : uint8_t {
    IDLE = 0,
    OPENING_DOORS,
    SYNC_DOORS_OPEN,
    RUNNING_MOTOR,
    SYNC_MOTOR_DONE,
    CLOSING_DOORS,
    ERROR
};

struct GearOp {
    enum Type : uint8_t {
        OPEN_DOORS,
        RUN_MOTOR,
        CLOSE_DOORS,
        SYNC_BARRIER,
    };
    Type type;
    bool deploying;   ///< direction context for this op
};

/// Verdict surfaced by the orchestrator from the slave-side stall
/// detector.  Mirrors sfx_peripherals::StallDetector::Result so the
/// state machine retains identical semantics — but the actual stall
/// detection runs on the slave (timing-critical) and the result
/// arrives via PWM_STALL async events.
enum class StallVerdict : uint8_t {
    RUNNING          = 0,
    STALL_CONFIRMED  = 1,
    NO_MOTOR         = 2,
    TIMEOUT_STALL    = 3,
    TIMEOUT_ERROR    = 4,
};

// ── Sequencer ────────────────────────────────────────────────────────

class GearSequencer {
public:
    static constexpr uint8_t MAX_OPS = 5;   ///< OPEN + SYNC + MOTOR + SYNC + CLOSE

    /// Drive the retract motor.  +1 = deploy direction, -1 = retract,
    /// 0 = stop.  Wired by the orchestrator to a SlaveApi PWM
    /// command (typically PWM_SET_DUTY on the retract channel with a
    /// signed magnitude → unsigned-duty + direction-pin policy).
    using MotorFn = std::function<void(int8_t direction)>;

    /// Returns the most recent stall verdict observed for the retract
    /// motor channel.  Wired by the orchestrator to a small adapter
    /// over `SlaveApi::onPwmStall(...)` that latches the last reported
    /// state and returns RUNNING when no terminal verdict has been
    /// observed since `armStallObserver()` was called.  Until wired,
    /// returns RUNNING — which means motor ops will hit the timeout
    /// path naturally rather than spuriously firing.
    using StallStateFn = std::function<StallVerdict()>;

    /// Reset the stall observer to its "running" state at the start of
    /// each motor op.  The orchestrator clears any latched verdict so
    /// stall reports from a previous op don't leak into the next.
    using ArmStallObserverFn = std::function<void(uint32_t timeout_ms)>;

    /// Phase change callback.
    ///   finished == false → a new op has started (phase transition).
    ///   finished == true  → queue exhausted (success: error == false)
    ///                        or fatal (error == true).
    using PhaseCallback = std::function<void(bool finished, bool error)>;

    /// Stall configuration forwarded to the slave on each motor op.
    /// Identical layout to the slave-side StallConfig — kept here so
    /// GearControlConfig can hand it off without an extra adapter.
    struct StallConfig {
        uint16_t startupIgnore_ms;
        uint16_t motorDetectThreshold_mA;
        uint16_t stallThreshold_mA;
        uint16_t stallConfirm_ms;
        uint32_t timeout_ms;
    };

    GearSequencer() = default;
    ~GearSequencer() = default;

    GearSequencer(const GearSequencer&)            = delete;
    GearSequencer& operator=(const GearSequencer&) = delete;

    // ── Initialization ───────────────────────────────────────────────

    /// Bind dependencies.  `doorSeq` must outlive the GearSequencer.
    /// `motorFn`, `stallFn`, `armFn` are stub-style callbacks the
    /// orchestrator wires up later.  Until wired, motor ops emit no
    /// commands and stall verdicts always read RUNNING — which means
    /// the motor will run for the full configured `timeout_ms`.
    void begin(DoorSequencer*      doorSeq,
               MotorFn             motorFn,
               StallStateFn        stallFn,
               ArmStallObserverFn  armFn) {
        _doorSeq  = doorSeq;
        _motorFn  = std::move(motorFn);
        _stallFn  = std::move(stallFn);
        _armFn    = std::move(armFn);
    }

    // ── Sequence control ─────────────────────────────────────────────

    void start(bool deploying, bool syncMode, const StallConfig& cfg);
    void update();
    void advanceSyncPhase();
    void stop();

    // ── Callbacks ────────────────────────────────────────────────────

    void onPhaseChange(PhaseCallback cb) { _phaseCb = std::move(cb); }

    // ── State queries ────────────────────────────────────────────────

    bool          isActive()      const { return _current < _count; }
    GearSeqStep   step()          const;
    bool          deploying()     const { return _deploying;   }
    bool          syncMode()      const { return _syncMode;    }
    bool          motorRunning()  const { return _motorRunning;}
    uint32_t      elapsed_ms()    const { return millis() - _sequenceStartTime_ms; }

    bool isWaitingSyncDoorsOpen() const { return step() == GearSeqStep::SYNC_DOORS_OPEN; }
    bool isWaitingSyncMotorDone() const { return step() == GearSeqStep::SYNC_MOTOR_DONE; }
    bool isSyncOpeningDoors()     const { return _syncMode && step() == GearSeqStep::OPENING_DOORS; }
    bool isSyncRunningMotor()     const { return _syncMode && step() == GearSeqStep::RUNNING_MOTOR; }

private:
    // Op queue
    GearOp   _ops[MAX_OPS]{};
    uint8_t  _count   = 0;
    uint8_t  _current = 0;

    // Sequence metadata
    bool     _deploying            = false;
    bool     _syncMode             = false;
    bool     _motorRunning         = false;
    uint32_t _stepStartTime_ms     = 0;
    uint32_t _sequenceStartTime_ms = 0;
    StallConfig _stallConfig{};

    // Dependencies (not owned)
    DoorSequencer*      _doorSeq = nullptr;
    MotorFn             _motorFn;
    StallStateFn        _stallFn;
    ArmStallObserverFn  _armFn;

    PhaseCallback _phaseCb;

    // Queue management
    void _enqueue(GearOp::Type type, bool dep);
    void _clear();

    // Op execution
    void _beginCurrentOp();
    void _advanceToNextOp();
    void _finish();
    void _error();
    void _emitPhase(bool finished = false, bool error = false);

    // Stub-safe shims — call only when callback is bound.
    void _stubMotor(int8_t direction) { if (_motorFn) _motorFn(direction); }
    void _stubArm(uint32_t timeout_ms){ if (_armFn)   _armFn(timeout_ms);  }
    StallVerdict _stubStall() const   {
        return _stallFn ? _stallFn() : StallVerdict::RUNNING;
    }
};

}  // namespace hubfx::effects::gearcontrol

#endif  // HUBFX_GEARCONTROL_GEAR_SEQUENCER_H
