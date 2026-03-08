/*
 * Door Sequencer Module - Implementation
 *
 * Mode-aware door servo sequencing for landing gear.
 * Extracted from LandingGear to isolate door open/close logic.
 */

#include "door_sequencer.h"

// ============================================================================
// Initialization & Configuration
// ============================================================================

void DoorSequencer::begin(ServoControl* door0, ServoControl* door1) {
    _doors[0] = door0;
    _doors[1] = door1;
}

void DoorSequencer::setConfig(const GearControlDoorConfig& config) {
    _config = config;
}

void DoorSequencer::setMode(uint8_t preDeployMode, uint8_t postDeployMode, uint16_t delay_ms) {
    _preDeployMode = preDeployMode;
    _postDeployMode = postDeployMode;
    _delay_ms = delay_ms;
}

// ============================================================================
// Sequenced Operations
// ============================================================================

void DoorSequencer::startOpen(bool deploying) {
    // Deploy: open with pre-deploy mode. Retract: open with post-deploy mode.
    _activeMode = deploying ? _preDeployMode : _postDeployMode;

    if (_activeMode == DoorMode::NONE) {
        _state = State::COMPLETE;  // Immediately complete for NONE mode
        return;
    }

    _state = State::OPENING;
    _doorState = DoorState::OPENING;
    _startTime_ms = millis();
    _phase = 0;
    _phaseStart_ms = millis();

    // Direction context: opening goes door 0→1
    _dir.firstIdx = 0;
    _dir.secondIdx = 1;
    _dir.secondTarget_us = _config.open1_us;
    _dir.finalDoorState = DoorState::OPEN;

    // Command initial doors based on active mode
    switch (_activeMode) {
        case DoorMode::SINGLE:
            _doors[0]->setTarget(_config.open0_us);
            break;
        case DoorMode::DUAL_SYNC:
            _doors[0]->setTarget(_config.open0_us);
            _doors[1]->setTarget(_config.open1_us);
            break;
        case DoorMode::DUAL_DELAY:
        case DoorMode::DUAL_SEQ:
            // Door 0 first; door 1 started later by update()
            _doors[0]->setTarget(_config.open0_us);
            break;
    }
}

void DoorSequencer::startClose(bool deploying) {
    // Deploy: close with post-deploy mode. Retract: close with pre-deploy mode.
    _activeMode = deploying ? _postDeployMode : _preDeployMode;

    if (_activeMode == DoorMode::NONE) {
        _state = State::COMPLETE;
        return;
    }

    _state = State::CLOSING;
    _doorState = DoorState::CLOSING;
    _startTime_ms = millis();
    _phase = 0;
    _phaseStart_ms = millis();

    // Direction context: closing goes door 1→0 (reverse)
    _dir.firstIdx = 1;
    _dir.secondIdx = 0;
    _dir.secondTarget_us = _config.close0_us;
    _dir.finalDoorState = DoorState::CLOSED;

    // Command initial doors (reverse order for DELAY/SEQ)
    switch (_activeMode) {
        case DoorMode::SINGLE:
            _doors[0]->setTarget(_config.close0_us);
            break;
        case DoorMode::DUAL_SYNC:
            _doors[0]->setTarget(_config.close0_us);
            _doors[1]->setTarget(_config.close1_us);
            break;
        case DoorMode::DUAL_DELAY:
        case DoorMode::DUAL_SEQ:
            // Reverse order: door 1 first; door 0 started later by update()
            _doors[1]->setTarget(_config.close1_us);
            break;
    }
}

void DoorSequencer::update() {
    if (_state == State::OPENING || _state == State::CLOSING) {
        _updateSequence();
    }
}

// ============================================================================
// Unified Sequence Update
// ============================================================================
//
// Handles both OPENING and CLOSING states using _dir context set by
// startOpen()/startClose(). Door ordering for DUAL_DELAY/DUAL_SEQ is
// encoded in _dir.firstIdx/_dir.secondIdx so the logic is identical.
//

void DoorSequencer::_updateSequence() {
    uint32_t now = millis();

    switch (_activeMode) {
        // SINGLE and DUAL_SYNC: wait for all commanded doors to reach target
        case DoorMode::SINGLE:
        case DoorMode::DUAL_SYNC: {
            bool allReady = _doors[0]->atTarget();
            if (_activeMode == DoorMode::DUAL_SYNC) {
                allReady = allReady && _doors[1]->atTarget();
            }
            if (allReady) {
                if (now - _startTime_ms >= DoorSeqConfig::SETTLE_TIME_ms) {
                    _state = State::COMPLETE;
                    _doorState = _dir.finalDoorState;
                }
            } else {
                _startTime_ms = now;  // Reset settle timer while still moving
            }
            break;
        }

        // DUAL_DELAY: second door starts after configurable delay
        case DoorMode::DUAL_DELAY: {
            if (_phase == 0 && (now - _phaseStart_ms) >= _delay_ms) {
                _doors[_dir.secondIdx]->setTarget(_dir.secondTarget_us);
                _phase = 1;
                _startTime_ms = now;  // Reset settle timer for phase 1
            }
            // Wait for all started doors
            bool ready = _doors[_dir.firstIdx]->atTarget();
            if (_phase == 1) {
                ready = ready && _doors[_dir.secondIdx]->atTarget();
            }
            if (ready) {
                if (now - _startTime_ms >= DoorSeqConfig::SETTLE_TIME_ms) {
                    _state = State::COMPLETE;
                    _doorState = _dir.finalDoorState;
                }
            } else {
                _startTime_ms = now;
            }
            break;
        }

        // DUAL_SEQ: second door starts after first door completes
        case DoorMode::DUAL_SEQ: {
            if (_phase == 0) {
                if (_doors[_dir.firstIdx]->atTarget()) {
                    _doors[_dir.secondIdx]->setTarget(_dir.secondTarget_us);
                    _phase = 1;
                    _startTime_ms = now;  // Reset settle timer for second door
                }
            } else {
                if (_doors[_dir.secondIdx]->atTarget()) {
                    if (now - _startTime_ms >= DoorSeqConfig::SETTLE_TIME_ms) {
                        _state = State::COMPLETE;
                        _doorState = _dir.finalDoorState;
                    }
                } else {
                    _startTime_ms = now;
                }
            }
            break;
        }

        default:
            _state = State::COMPLETE;
            _doorState = _dir.finalDoorState;
            break;
    }
}

// ============================================================================
// Direct Control (bypasses sequencing)
// ============================================================================

void DoorSequencer::openImmediate() {
    // Immediate open always uses pre-deploy mode
    if (_preDeployMode == DoorMode::NONE) return;
    _doors[0]->setTarget(_config.open0_us);
    if (_preDeployMode >= DoorMode::DUAL_SYNC && _doors[1]) {
        _doors[1]->setTarget(_config.open1_us);
    }
    _doorState = DoorState::OPEN;
}

void DoorSequencer::closeImmediate() {
    // Immediate close always uses pre-deploy mode
    if (_preDeployMode == DoorMode::NONE) return;
    _doors[0]->setTarget(_config.close0_us);
    if (_preDeployMode >= DoorMode::DUAL_SYNC && _doors[1]) {
        _doors[1]->setTarget(_config.close1_us);
    }
    _doorState = DoorState::CLOSED;
}

void DoorSequencer::setPosition(uint8_t doorIndex, uint16_t pos_us) {
    if (doorIndex < 2 && _doors[doorIndex]) {
        _doors[doorIndex]->setTarget(pos_us);
    }
}

// ============================================================================
// Door Servo Access
// ============================================================================

uint16_t DoorSequencer::position_us(uint8_t doorIndex) const {
    if (doorIndex < 2 && _doors[doorIndex]) {
        return (uint16_t)_doors[doorIndex]->position();
    }
    return 0;
}

ServoControl& DoorSequencer::servo(uint8_t doorIndex) {
    return *_doors[doorIndex < 2 ? doorIndex : 0];
}

const ServoControl& DoorSequencer::servo(uint8_t doorIndex) const {
    return *_doors[doorIndex < 2 ? doorIndex : 0];
}
