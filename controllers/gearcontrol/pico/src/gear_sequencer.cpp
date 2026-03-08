/*
 * Gear Sequencer Module - Implementation
 *
 * Op-queue based deploy/retract sequencing with preemption support.
 * See gear_sequencer.h for design overview.
 */

#include "gear_sequencer.h"

// ============================================================================
// Initialization
// ============================================================================

void GearSequencer::begin(DoorSequencer* doorSeq, StallDetector* stallDetector,
                           MotorFn motorFn, CurrentFn currentFn) {
    _doorSeq = doorSeq;
    _stallDetector = stallDetector;
    _motorFn = motorFn;
    _currentFn = currentFn;
}

// ============================================================================
// Queue Management
// ============================================================================

void GearSequencer::_enqueue(GearOp::Type type, bool dep) {
    if (_count < MAX_OPS) _ops[_count++] = {type, dep};
}

void GearSequencer::_clear() {
    _count = 0;
    _current = 0;
    _motorRunning = false;
}

GearSeqStep GearSequencer::step() const {
    if (_current >= _count) return GearSeqStep::IDLE;
    switch (_ops[_current].type) {
        case GearOp::OPEN_DOORS:   return GearSeqStep::OPENING_DOORS;
        case GearOp::RUN_MOTOR:    return GearSeqStep::RUNNING_MOTOR;
        case GearOp::CLOSE_DOORS:  return GearSeqStep::CLOSING_DOORS;
        case GearOp::SYNC_BARRIER:
            return (_current > 0 && _ops[_current - 1].type == GearOp::RUN_MOTOR)
                ? GearSeqStep::SYNC_MOTOR_DONE : GearSeqStep::SYNC_DOORS_OPEN;
        default: return GearSeqStep::IDLE;
    }
}

void GearSequencer::_emitPhase(bool finished, bool error) {
    if (_phaseCb) _phaseCb(finished, error);
}

// ============================================================================
// Sequence Control
// ============================================================================

/**
 * @brief Build and start a deploy/retract op queue, preempting if necessary
 *
 * Preemption behaviour by current op:
 *   OPEN_DOORS  → non-sync: skip (doors finish in background, motor overlaps)
 *                 sync: include OPEN_DOORS so barrier waits for door completion
 *   RUN_MOTOR   → stop motor; doors are open; re-queue motor + close
 *   CLOSE_DOORS → reverse doors (reopen); queue full open + motor + close
 *   SYNC_BARRIER→ doors are fully open (completed before barrier); skip door open
 */
void GearSequencer::start(bool deploying, bool syncMode, const StallConfig& cfg) {
    bool wasActive = isActive();
    bool doorsNeedOpening = true;

    // Handle preemption of the current operation
    if (wasActive) {
        switch (_ops[_current].type) {
            case GearOp::OPEN_DOORS:
                // Doors are mid-open — DoorSequencer continues running.
                // Non-sync: skip (motor starts while doors finish in background).
                // Sync: include OPEN_DOORS so the subsequent SYNC_BARRIER isn't
                // reached until doors are done — _beginCurrentOp() will see
                // "doors already opening" and wait for the DoorSequencer.
                doorsNeedOpening = syncMode;
                break;
            case GearOp::RUN_MOTOR:
                // Motor was running — stop it. Doors are already open.
                _motorFn(0);
                _motorRunning = false;
                doorsNeedOpening = false;
                break;
            case GearOp::CLOSE_DOORS:
                // Doors are mid-close — need to reopen for new motor run.
                doorsNeedOpening = true;
                break;
            case GearOp::SYNC_BARRIER:
                // Parked at a barrier — doors are fully open (they completed
                // before the barrier was reached). Motor may or may not have
                // run depending on which barrier, but it's already stopped.
                doorsNeedOpening = false;
                break;
        }
    }

    // Preserve sequence start time during preemption (elapsed keeps accumulating)
    uint32_t seqStart = wasActive ? _sequenceStartTime_ms : millis();

    // Build new queue
    _clear();
    _deploying = deploying;
    _syncMode = syncMode;
    _stallConfig = cfg;
    _sequenceStartTime_ms = seqStart;

    if (doorsNeedOpening) {
        // Need to open doors (fresh start, or reversing a close).
        // Always use preDeployMode (deploying=true) — it's guaranteed to be
        // configured if doors exist, avoiding the postDeployMode==NONE edge case.
        _enqueue(GearOp::OPEN_DOORS, true);
    }
    if (syncMode) _enqueue(GearOp::SYNC_BARRIER, deploying);
    _enqueue(GearOp::RUN_MOTOR, deploying);
    if (syncMode) _enqueue(GearOp::SYNC_BARRIER, deploying);
    _enqueue(GearOp::CLOSE_DOORS, deploying);

    // Start executing the first op
    _beginCurrentOp();
}

void GearSequencer::stop() {
    _motorFn(0);
    _clear();
    _syncMode = false;
}

void GearSequencer::advanceSyncPhase() {
    if (isActive() && _ops[_current].type == GearOp::SYNC_BARRIER) {
        _advanceToNextOp();
    }
}

// ============================================================================
// Op Execution
// ============================================================================

/**
 * @brief Execute the current queue op
 *
 * Called when a new op becomes current (sequence start, advance, or preemption).
 * Each op type initiates its async work; update() polls for completion.
 */
void GearSequencer::_beginCurrentOp() {
    if (!isActive()) {
        _finish();
        return;
    }

    auto& op = _ops[_current];
    _stepStartTime_ms = millis();

    switch (op.type) {
        case GearOp::OPEN_DOORS: {
            // If doors are already in OPENING state (from a preempted sequence),
            // just wait for the DoorSequencer to finish — don't restart.
            if (_doorSeq->isBusy() && _doorSeq->doorState() == DoorState::OPENING) {
                _emitPhase();
                return;
            }
            // If doors are already open, skip this op.
            if (_doorSeq->doorState() == DoorState::OPEN) {
                _advanceToNextOp();
                return;
            }
            // Start opening (may reverse a close in progress)
            _doorSeq->startOpen(op.deploying);
            if (_doorSeq->isComplete()) {
                // Mode was NONE — immediate completion, skip
                _advanceToNextOp();
            } else {
                _emitPhase();
            }
            break;
        }

        case GearOp::RUN_MOTOR: {
            _motorFn(op.deploying ? 1 : -1);
            _motorRunning = true;

            // Configure and start stall detector
            StallDetector::Config cfg;
            cfg.startupIgnore_ms = _stallConfig.startupIgnore_ms;
            cfg.motorDetectThreshold_mA = _stallConfig.motorDetectThreshold_mA;
            cfg.stallThreshold_mA = _stallConfig.stallThreshold_mA;
            cfg.stallConfirm_ms = _stallConfig.stallConfirm_ms;
            cfg.timeout_ms = _stallConfig.timeout_ms;
            _stallDetector->configure(cfg);
            _stallDetector->start();

            _emitPhase();
            break;
        }

        case GearOp::CLOSE_DOORS: {
            _doorSeq->startClose(op.deploying);
            if (_doorSeq->isComplete()) {
                // Mode was NONE — immediate completion
                _advanceToNextOp();
            } else {
                _emitPhase();
            }
            break;
        }

        case GearOp::SYNC_BARRIER:
            // Wait for coordinator to call advanceSyncPhase()
            _emitPhase();
            break;
    }
}

/**
 * @brief Advance to the next op in the queue and begin it
 */
void GearSequencer::_advanceToNextOp() {
    if (_current < _count) _current++;
    _beginCurrentOp();
}

/**
 * @brief Finalize the sequence — clear queue and notify owner
 *
 * Keeps _deploying and _syncMode readable so the callback can query
 * the terminal direction before the owner clears them.
 */
void GearSequencer::_finish() {
    _count = 0;
    _current = 0;
    _motorRunning = false;
    _syncMode = false;
    _emitPhase(true, false);
}

/**
 * @brief Handle a fatal error — stop motor, clear queue, notify owner
 */
void GearSequencer::_error() {
    _motorFn(0);
    _count = 0;
    _current = 0;
    _motorRunning = false;
    _emitPhase(true, true);
}

// ============================================================================
// Update (poll current op for completion)
// ============================================================================

void GearSequencer::update() {
    if (!isActive()) return;

    auto& op = _ops[_current];

    switch (op.type) {
        case GearOp::OPEN_DOORS: {
            _doorSeq->update();
            if (_doorSeq->isComplete()) {
                _advanceToNextOp();
            }
            break;
        }

        case GearOp::RUN_MOTOR: {
            uint16_t current_mA = _currentFn();
            auto result = _stallDetector->update(current_mA);
            switch (result) {
                case StallDetector::Result::RUNNING:
                    break;
                case StallDetector::Result::STALL_CONFIRMED:
                case StallDetector::Result::TIMEOUT_STALL:
                case StallDetector::Result::NO_MOTOR:
                    _motorFn(0);
                    _motorRunning = false;
                    _advanceToNextOp();
                    break;
                case StallDetector::Result::TIMEOUT_ERROR:
                    _error();
                    break;
            }
            break;
        }

        case GearOp::CLOSE_DOORS: {
            _doorSeq->update();
            if (_doorSeq->isComplete()) {
                _advanceToNextOp();  // Will call _finish() when queue exhausted
            }
            break;
        }

        case GearOp::SYNC_BARRIER:
            // Waiting for coordinator — no-op
            break;
    }
}
