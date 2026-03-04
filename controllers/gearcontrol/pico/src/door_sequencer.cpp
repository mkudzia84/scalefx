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

void DoorSequencer::setMode(uint8_t mode, uint16_t delay_ms) {
    _mode = mode;
    _delay_ms = delay_ms;
}

// ============================================================================
// Sequenced Operations
// ============================================================================

void DoorSequencer::startOpen() {
    if (_mode == DoorMode::NONE) {
        _state = State::COMPLETE;  // Immediately complete for NONE mode
        return;
    }

    _state = State::OPENING;
    _startTime_ms = millis();
    _phase = 0;
    _phaseStart_ms = millis();

    // Command initial doors based on mode
    switch (_mode) {
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

void DoorSequencer::startClose() {
    if (_mode == DoorMode::NONE) {
        _state = State::COMPLETE;
        return;
    }

    _state = State::CLOSING;
    _startTime_ms = millis();
    _phase = 0;
    _phaseStart_ms = millis();

    // Command initial doors (reverse order for DELAY/SEQ)
    switch (_mode) {
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
    if (_state == State::OPENING) {
        _updateOpening();
    } else if (_state == State::CLOSING) {
        _updateClosing();
    }
}

// ============================================================================
// Opening State Machine
// ============================================================================

void DoorSequencer::_updateOpening() {
    uint32_t now = millis();
    uint32_t elapsed = now - _startTime_ms;

    switch (_mode) {
        case DoorMode::SINGLE: {
            bool ready = _doors[0]->atTarget();
            if ((ready && elapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                (!ready && elapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms)) {
                _state = State::COMPLETE;
            }
            break;
        }

        case DoorMode::DUAL_SYNC: {
            bool ready = _doors[0]->atTarget() && _doors[1]->atTarget();
            if ((ready && elapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                (!ready && elapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms)) {
                _state = State::COMPLETE;
            }
            break;
        }

        case DoorMode::DUAL_DELAY: {
            uint32_t phaseElapsed = now - _phaseStart_ms;
            // Start door 1 after delay
            if (_phase == 0 && phaseElapsed >= _delay_ms) {
                _doors[1]->setTarget(_config.open1_us);
                _phase = 1;
            }
            // Wait for all started doors
            bool ready = _doors[0]->atTarget();
            if (_phase == 1) {
                ready = ready && _doors[1]->atTarget();
            }
            uint32_t maxTime = DoorSeqConfig::DOOR_TRAVEL_TIME_ms + _delay_ms;
            if ((ready && elapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                elapsed >= maxTime) {
                _state = State::COMPLETE;
            }
            break;
        }

        case DoorMode::DUAL_SEQ: {
            uint32_t phaseElapsed = now - _phaseStart_ms;
            if (_phase == 0) {
                // Wait for door 0 to complete
                bool ready = _doors[0]->atTarget();
                if (ready || phaseElapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms) {
                    _doors[1]->setTarget(_config.open1_us);
                    _phase = 1;
                    _phaseStart_ms = now;
                }
            } else {
                // Wait for door 1 to complete
                bool ready = _doors[1]->atTarget();
                if ((ready && phaseElapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                    phaseElapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms) {
                    _state = State::COMPLETE;
                }
            }
            break;
        }

        default:
            _state = State::COMPLETE;
            break;
    }
}

// ============================================================================
// Closing State Machine
// ============================================================================

void DoorSequencer::_updateClosing() {
    uint32_t now = millis();
    uint32_t elapsed = now - _startTime_ms;

    switch (_mode) {
        case DoorMode::SINGLE: {
            bool ready = _doors[0]->atTarget();
            if ((ready && elapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                elapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms) {
                _state = State::COMPLETE;
            }
            break;
        }

        case DoorMode::DUAL_SYNC: {
            bool ready = _doors[0]->atTarget() && _doors[1]->atTarget();
            if ((ready && elapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                elapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms) {
                _state = State::COMPLETE;
            }
            break;
        }

        case DoorMode::DUAL_DELAY: {
            uint32_t phaseElapsed = now - _phaseStart_ms;
            // Reverse: door 1 started in startClose(), start door 0 after delay
            if (_phase == 0 && phaseElapsed >= _delay_ms) {
                _doors[0]->setTarget(_config.close0_us);
                _phase = 1;
            }
            bool ready = _doors[1]->atTarget();
            if (_phase == 1) {
                ready = ready && _doors[0]->atTarget();
            }
            uint32_t maxTime = DoorSeqConfig::DOOR_TRAVEL_TIME_ms + _delay_ms;
            if ((ready && elapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                elapsed >= maxTime) {
                _state = State::COMPLETE;
            }
            break;
        }

        case DoorMode::DUAL_SEQ: {
            uint32_t phaseElapsed = now - _phaseStart_ms;
            if (_phase == 0) {
                // Reverse: wait for door 1 to close first
                bool ready = _doors[1]->atTarget();
                if (ready || phaseElapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms) {
                    _doors[0]->setTarget(_config.close0_us);
                    _phase = 1;
                    _phaseStart_ms = now;
                }
            } else {
                // Wait for door 0 to close
                bool ready = _doors[0]->atTarget();
                if ((ready && phaseElapsed >= DoorSeqConfig::SETTLE_TIME_ms) ||
                    phaseElapsed >= DoorSeqConfig::DOOR_TRAVEL_TIME_ms) {
                    _state = State::COMPLETE;
                }
            }
            break;
        }

        default:
            _state = State::COMPLETE;
            break;
    }
}

// ============================================================================
// Direct Control (bypasses sequencing)
// ============================================================================

void DoorSequencer::openImmediate() {
    if (_mode == DoorMode::NONE) return;
    _doors[0]->setTarget(_config.open0_us);
    if (_mode >= DoorMode::DUAL_SYNC) {
        _doors[1]->setTarget(_config.open1_us);
    }
}

void DoorSequencer::closeImmediate() {
    if (_mode == DoorMode::NONE) return;
    _doors[0]->setTarget(_config.close0_us);
    if (_mode >= DoorMode::DUAL_SYNC) {
        _doors[1]->setTarget(_config.close1_us);
    }
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
