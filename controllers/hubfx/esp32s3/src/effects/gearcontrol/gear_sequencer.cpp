/*
 * GearSequencer (master-side) — implementation.
 *
 * Mirrors the slave-side gear_sequencer.cpp op-queue logic; the only
 * substantive change is the indirection through StallStateFn /
 * ArmStallObserverFn / MotorFn instead of direct StallDetector pointer
 * + immediate motor PWM control.  The state-machine shape, queue
 * layout, preemption table, and sync-barrier semantics are identical.
 */

#include "gear_sequencer.h"

namespace hubfx::effects::gearcontrol {

// ── Wire-format door state (used to peek at door progress) ───────────
//
// We re-use the gearcontrol wire enum here because the door sequencer
// already exposes its state in those terms.  Pulled in via the
// transitive include from door_sequencer.h.

// ── Queue management ─────────────────────────────────────────────────

void GearSequencer::_enqueue(GearOp::Type type, bool dep) {
    if (_count < MAX_OPS) _ops[_count++] = { type, dep };
}

void GearSequencer::_clear() {
    _count        = 0;
    _current      = 0;
    _motorRunning = false;
}

GearSeqStep GearSequencer::step() const {
    if (_current >= _count) return GearSeqStep::IDLE;
    switch (_ops[_current].type) {
        case GearOp::OPEN_DOORS:  return GearSeqStep::OPENING_DOORS;
        case GearOp::RUN_MOTOR:   return GearSeqStep::RUNNING_MOTOR;
        case GearOp::CLOSE_DOORS: return GearSeqStep::CLOSING_DOORS;
        case GearOp::SYNC_BARRIER:
            return (_current > 0 && _ops[_current - 1].type == GearOp::RUN_MOTOR)
                ? GearSeqStep::SYNC_MOTOR_DONE
                : GearSeqStep::SYNC_DOORS_OPEN;
        default:
            return GearSeqStep::IDLE;
    }
}

void GearSequencer::_emitPhase(bool finished, bool error) {
    if (_phaseCb) _phaseCb(finished, error);
}

// ── Sequence control ─────────────────────────────────────────────────

void GearSequencer::start(bool deploying, bool syncMode, const StallConfig& cfg) {
    const bool wasActive    = isActive();
    bool       doorsNeedOpen = true;

    if (wasActive) {
        switch (_ops[_current].type) {
            case GearOp::OPEN_DOORS:
                // Doors mid-open — non-sync skips, sync keeps the op so
                // the subsequent SYNC_BARRIER waits for door completion.
                doorsNeedOpen = syncMode;
                break;
            case GearOp::RUN_MOTOR:
                // Stop motor; doors are already open.
                _stubMotor(0);
                _motorRunning = false;
                doorsNeedOpen = false;
                break;
            case GearOp::CLOSE_DOORS:
                // Reopen for the new motor run.
                doorsNeedOpen = true;
                break;
            case GearOp::SYNC_BARRIER:
                doorsNeedOpen = false;
                break;
        }
    }

    const uint32_t seqStart = wasActive ? _sequenceStartTime_ms : millis();

    _clear();
    _deploying            = deploying;
    _syncMode             = syncMode;
    _stallConfig          = cfg;
    _sequenceStartTime_ms = seqStart;

    if (doorsNeedOpen) {
        // Always enqueue with deploying=true here — we want the door
        // mode that opens doors, regardless of the gear direction.
        _enqueue(GearOp::OPEN_DOORS, true);
    }
    if (syncMode) _enqueue(GearOp::SYNC_BARRIER, deploying);
    _enqueue(GearOp::RUN_MOTOR, deploying);
    if (syncMode) _enqueue(GearOp::SYNC_BARRIER, deploying);
    _enqueue(GearOp::CLOSE_DOORS, deploying);

    _beginCurrentOp();
}

void GearSequencer::stop() {
    _stubMotor(0);
    _clear();
    _syncMode = false;
}

void GearSequencer::advanceSyncPhase() {
    if (isActive() && _ops[_current].type == GearOp::SYNC_BARRIER) {
        _advanceToNextOp();
    }
}

// ── Op execution ─────────────────────────────────────────────────────

void GearSequencer::_beginCurrentOp() {
    if (!isActive()) {
        _finish();
        return;
    }

    auto& op            = _ops[_current];
    _stepStartTime_ms   = millis();

    switch (op.type) {
        case GearOp::OPEN_DOORS: {
            // Reuse a door open already in progress (preemption case).
            if (_doorSeq->isBusy() && _doorSeq->doorState() == ::DoorState::OPENING) {
                _emitPhase();
                return;
            }
            if (_doorSeq->doorState() == ::DoorState::OPEN) {
                _advanceToNextOp();
                return;
            }
            _doorSeq->startOpen(op.deploying);
            if (_doorSeq->isComplete()) {
                _advanceToNextOp();   // mode == NONE collapsed immediately
            } else {
                _emitPhase();
            }
            break;
        }

        case GearOp::RUN_MOTOR: {
            _stubArm(_stallConfig.timeout_ms);
            _stubMotor(op.deploying ? 1 : -1);
            _motorRunning = true;
            _emitPhase();
            break;
        }

        case GearOp::CLOSE_DOORS: {
            _doorSeq->startClose(op.deploying);
            if (_doorSeq->isComplete()) {
                _advanceToNextOp();
            } else {
                _emitPhase();
            }
            break;
        }

        case GearOp::SYNC_BARRIER:
            _emitPhase();
            break;
    }
}

void GearSequencer::_advanceToNextOp() {
    if (_current < _count) _current++;
    _beginCurrentOp();
}

void GearSequencer::_finish() {
    _count        = 0;
    _current      = 0;
    _motorRunning = false;
    _syncMode     = false;
    _emitPhase(true, false);
}

void GearSequencer::_error() {
    _stubMotor(0);
    _count        = 0;
    _current      = 0;
    _motorRunning = false;
    _emitPhase(true, true);
}

// ── Update — poll current op for completion ──────────────────────────

void GearSequencer::update() {
    if (!isActive()) return;

    auto& op = _ops[_current];

    switch (op.type) {
        case GearOp::OPEN_DOORS: {
            _doorSeq->update();
            if (_doorSeq->isComplete()) _advanceToNextOp();
            break;
        }

        case GearOp::RUN_MOTOR: {
            // Verdict comes from the orchestrator's stall-observer
            // adapter (which in turn pulls from SlaveApi PWM_STALL
            // events).  Until wired, _stubStall() returns RUNNING and
            // the motor will run forever — the orchestrator is
            // expected to enforce a wall-clock timeout via
            // armStallObserver().
            switch (_stubStall()) {
                case StallVerdict::RUNNING:
                    break;
                case StallVerdict::STALL_CONFIRMED:
                case StallVerdict::TIMEOUT_STALL:
                case StallVerdict::NO_MOTOR:
                    _stubMotor(0);
                    _motorRunning = false;
                    _advanceToNextOp();
                    break;
                case StallVerdict::TIMEOUT_ERROR:
                    _error();
                    break;
            }
            break;
        }

        case GearOp::CLOSE_DOORS: {
            _doorSeq->update();
            if (_doorSeq->isComplete()) _advanceToNextOp();
            break;
        }

        case GearOp::SYNC_BARRIER:
            // Coordinator-driven: do nothing until advanceSyncPhase().
            break;
    }
}

}  // namespace hubfx::effects::gearcontrol
