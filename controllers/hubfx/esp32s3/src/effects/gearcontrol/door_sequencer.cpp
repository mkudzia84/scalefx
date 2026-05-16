/*
 * DoorSequencer (master-side) — implementation.
 *
 * Event-driven port of the slave-side door sequencer.  See the header
 * for the shape of the state machine; the only meaningful difference
 * from controllers/gearcontrol/pico/src/door_sequencer.cpp is that
 * "atTarget" is supplied by notifyTargetReached() (i.e., by the
 * SERVO_TARGET_REACHED async packet from the slave) rather than by
 * polling a local ServoControl object.
 */

#include "door_sequencer.h"

namespace hubfx::effects::gearcontrol {

// ── Public API ───────────────────────────────────────────────────────

void DoorSequencer::startOpen(bool deploying) {
    _activeMode = deploying ? _preDeployMode : _postDeployMode;

    if (_activeMode == ::DoorMode::NONE) {
        _state = State::COMPLETE;
        return;
    }

    _state          = State::OPENING;
    _doorState      = ::DoorState::OPENING;
    _phase          = 0;
    _phaseStart_ms  = millis();
    _settleStart_ms = millis();
    _reached[0]     = false;
    _reached[1]     = false;

    // Opening: door 0 first, door 1 second.
    _dir.firstIdx        = 0;
    _dir.secondIdx       = 1;
    _dir.firstTarget_us  = _targets[0].open_us;
    _dir.secondTarget_us = _targets[1].open_us;
    _dir.finalDoorState  = ::DoorState::OPEN;

    switch (_activeMode) {
        case ::DoorMode::SINGLE:
            _emitCommand(0, _targets[0].open_us);
            break;
        case ::DoorMode::DUAL_SYNC:
            _emitCommand(0, _targets[0].open_us);
            _emitCommand(1, _targets[1].open_us);
            break;
        case ::DoorMode::DUAL_DELAY:
        case ::DoorMode::DUAL_SEQ:
            // First door commanded immediately; second door deferred.
            _emitCommand(_dir.firstIdx, _dir.firstTarget_us);
            break;
    }
}

void DoorSequencer::startClose(bool deploying) {
    _activeMode = deploying ? _postDeployMode : _preDeployMode;

    if (_activeMode == ::DoorMode::NONE) {
        _state = State::COMPLETE;
        return;
    }

    _state          = State::CLOSING;
    _doorState      = ::DoorState::CLOSING;
    _phase          = 0;
    _phaseStart_ms  = millis();
    _settleStart_ms = millis();
    _reached[0]     = false;
    _reached[1]     = false;

    // Closing: door 1 first, door 0 second (reverse order).
    _dir.firstIdx        = 1;
    _dir.secondIdx       = 0;
    _dir.firstTarget_us  = _targets[1].close_us;
    _dir.secondTarget_us = _targets[0].close_us;
    _dir.finalDoorState  = ::DoorState::CLOSED;

    switch (_activeMode) {
        case ::DoorMode::SINGLE:
            _emitCommand(0, _targets[0].close_us);
            break;
        case ::DoorMode::DUAL_SYNC:
            _emitCommand(0, _targets[0].close_us);
            _emitCommand(1, _targets[1].close_us);
            break;
        case ::DoorMode::DUAL_DELAY:
        case ::DoorMode::DUAL_SEQ:
            _emitCommand(_dir.firstIdx, _dir.firstTarget_us);
            break;
    }
}

void DoorSequencer::notifyTargetReached(uint8_t doorIndex) {
    if (doorIndex >= 2) return;
    if (_state != State::OPENING && _state != State::CLOSING) return;

    _reached[doorIndex] = true;

    // For DUAL_SEQ the second door is dispatched on first-reach.
    if (_activeMode == ::DoorMode::DUAL_SEQ &&
        _phase == 0 &&
        doorIndex == _dir.firstIdx) {
        _emitCommand(_dir.secondIdx, _dir.secondTarget_us);
        _phase          = 1;
        _settleStart_ms = millis();
    }

    _completeIfSettled(millis());
}

void DoorSequencer::update() {
    if (_state != State::OPENING && _state != State::CLOSING) return;

    const uint32_t now = millis();

    // DUAL_DELAY: dispatch the second door once the timer elapses.
    if (_activeMode == ::DoorMode::DUAL_DELAY &&
        _phase == 0 &&
        (now - _phaseStart_ms) >= _delay_ms) {
        _emitCommand(_dir.secondIdx, _dir.secondTarget_us);
        _phase          = 1;
        _settleStart_ms = now;
    }

    _completeIfSettled(now);
}

void DoorSequencer::openImmediate() {
    if (_preDeployMode == ::DoorMode::NONE) return;
    _emitCommand(0, _targets[0].open_us);
    if (_preDeployMode >= ::DoorMode::DUAL_SYNC) {
        _emitCommand(1, _targets[1].open_us);
    }
    _doorState = ::DoorState::OPEN;
}

void DoorSequencer::closeImmediate() {
    if (_preDeployMode == ::DoorMode::NONE) return;
    _emitCommand(0, _targets[0].close_us);
    if (_preDeployMode >= ::DoorMode::DUAL_SYNC) {
        _emitCommand(1, _targets[1].close_us);
    }
    _doorState = ::DoorState::CLOSED;
}

// ── Internal helpers ─────────────────────────────────────────────────

bool DoorSequencer::_allRequiredReached() const {
    switch (_activeMode) {
        case ::DoorMode::SINGLE:
            return _reached[0];
        case ::DoorMode::DUAL_SYNC:
            return _reached[0] && _reached[1];
        case ::DoorMode::DUAL_DELAY:
        case ::DoorMode::DUAL_SEQ:
            // For DELAY/SEQ both doors must reach, but the second only
            // needs to reach once it's been dispatched (phase==1).
            if (_phase == 0) return false;
            return _reached[_dir.firstIdx] && _reached[_dir.secondIdx];
        default:
            return true;
    }
}

void DoorSequencer::_completeIfSettled(uint32_t now) {
    if (!_allRequiredReached()) {
        _settleStart_ms = now;
        return;
    }
    if (now - _settleStart_ms >= DoorSeqConfig::SETTLE_TIME_ms) {
        _state     = State::COMPLETE;
        _doorState = _dir.finalDoorState;
    }
}

}  // namespace hubfx::effects::gearcontrol
