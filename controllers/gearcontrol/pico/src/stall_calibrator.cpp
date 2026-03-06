/*
 * Stall Calibrator - Implementation
 *
 * Self-contained calibration state machine for landing gear motor stall
 * current detection.  See stall_calibrator.h for full documentation.
 *
 * Phase sequence:
 *   OPENING_DOORS → CLEAR_RUN → CLEAR_SETTLE → DEPLOY_RUN →
 *   MID_SETTLE → RETRACT_RUN → [CLOSING_DOORS →] COMPLETE/ERROR
 */

#include "stall_calibrator.h"

// ============================================================================
// Initialization
// ============================================================================

void StallCalibrator::begin(uint8_t gearId, MotorControlFn motorFn,
                            CurrentReadFn currentFn, DoorSequencer* doorSeq) {
    _gearId = gearId;
    _motorFn = motorFn;
    _currentFn = currentFn;
    _doorSeq = doorSeq;
}

// ============================================================================
// State Queries
// ============================================================================

bool StallCalibrator::isActive() const {
    return _phase != CalibPhase::IDLE &&
           _phase != CalibPhase::COMPLETE &&
           _phase != CalibPhase::ERROR &&
           _phase != CalibPhase::CANCELLED;
}

// ============================================================================
// Operations
// ============================================================================

uint8_t StallCalibrator::start(uint16_t initialStallGuess_mA) {
    // Reset all measurement state
    _initialStallGuess_mA = initialStallGuess_mA;
    _phaseStart_ms = millis();
    _lastSample_ms = millis();
    _lastStatusEmit_ms = millis();
    _baseline_mA = 0;
    _baselineSum = 0;
    _baselineCount = 0;
    _peakDeploy_mA = 0;
    _peakRetract_mA = 0;
    _stallStart_ms = 0;
    _stallDetected = false;
    _pendingResult = CalibPhase::IDLE;
    _result = Result{};

    // Open doors first (if configured), then start motor
    if (_doorSeq->mode() != DoorMode::NONE) {
        _doorSeq->startOpen();
        _phase = CalibPhase::OPENING_DOORS;
        _emitStatus(CalibPhase::OPENING_DOORS);
    } else {
        // No doors — start motor immediately
        _phase = CalibPhase::CLEAR_RUN;
        _motorFn(-1);
        _emitStatus(CalibPhase::CLEAR_RUN);
    }

    return SerialError::OK;
}

uint8_t StallCalibrator::cancel() {
    if (!isActive()) return GearControlError::NOT_CALIBRATING;

    _motorFn(0);  // Stop motor

    // Close doors if they were opened for calibration
    if (_doorSeq->mode() != DoorMode::NONE) {
        _doorSeq->startClose();
        _phase = CalibPhase::CLOSING_DOORS;
        _pendingResult = CalibPhase::CANCELLED;
    } else {
        _phase = CalibPhase::CANCELLED;
        _emitStatus(CalibPhase::CANCELLED);
    }

    return SerialError::OK;
}

// ============================================================================
// State Machine Update
// ============================================================================

void StallCalibrator::update() {
    if (!isActive()) return;

    uint32_t now = millis();
    uint32_t elapsed = now - _phaseStart_ms;

    switch (_phase) {
        // -----------------------------------------------------------------
        // OPENING_DOORS: Wait for doors to open before starting motor
        // -----------------------------------------------------------------
        case CalibPhase::OPENING_DOORS: {
            _doorSeq->update();
            if (_doorSeq->isComplete()) {
                _phase = CalibPhase::CLEAR_RUN;
                _phaseStart_ms = now;
                _lastSample_ms = now;
                _motorFn(-1);
                _emitStatus(CalibPhase::CLEAR_RUN);
            }
            break;
        }

        // -----------------------------------------------------------------
        // CLEAR_RUN: Brief retract to move away from deploy endpoint
        // -----------------------------------------------------------------
        case CalibPhase::CLEAR_RUN: {
            // Periodic status emission
            if (now - _lastStatusEmit_ms >= CalibConfig::STATUS_INTERVAL_ms) {
                _lastStatusEmit_ms = now;
                _emitStatus(_phase);
            }

            // Sample current for safety and stall detection
            if (now - _lastSample_ms >= CalibConfig::SAMPLE_INTERVAL_ms) {
                _lastSample_ms = now;
                uint16_t current_mA = _currentFn();

                // Safety cutoff
                if (current_mA >= CalibConfig::SAFETY_LIMIT_mA) {
                    _motorFn(0);
                    _phase = CalibPhase::CLEAR_SETTLE;
                    _phaseStart_ms = now;
                    _stallDetected = false;
                    _emitStatus(CalibPhase::CLEAR_SETTLE);
                    break;
                }

                // After inrush, detect stall to stop early (hit retract endpoint)
                if (elapsed > CalibConfig::INRUSH_IGNORE_ms) {
                    if (current_mA > _initialStallGuess_mA) {
                        if (!_stallDetected) {
                            _stallDetected = true;
                            _stallStart_ms = now;
                        } else if (now - _stallStart_ms >= CalibConfig::STALL_CONFIRM_ms) {
                            _motorFn(0);
                            _phase = CalibPhase::CLEAR_SETTLE;
                            _phaseStart_ms = now;
                            _stallDetected = false;
                            _emitStatus(CalibPhase::CLEAR_SETTLE);
                        }
                    } else {
                        _stallDetected = false;
                    }
                }
            }

            // Timeout → proceed to settle regardless
            if (elapsed >= CalibConfig::CLEAR_TIME_ms) {
                _motorFn(0);
                _phase = CalibPhase::CLEAR_SETTLE;
                _phaseStart_ms = now;
                _stallDetected = false;
                _emitStatus(CalibPhase::CLEAR_SETTLE);
            }
            break;
        }

        // -----------------------------------------------------------------
        // CLEAR_SETTLE: Wait for motor to stop after clearing
        // -----------------------------------------------------------------
        case CalibPhase::CLEAR_SETTLE: {
            if (elapsed >= CalibConfig::CLEAR_SETTLE_ms) {
                // Transition to DEPLOY_RUN
                _phase = CalibPhase::DEPLOY_RUN;
                _phaseStart_ms = now;
                _lastSample_ms = now;
                _lastStatusEmit_ms = now;
                _baseline_mA = 0;
                _baselineSum = 0;
                _baselineCount = 0;
                _stallDetected = false;
                _motorFn(1);
                _emitStatus(CalibPhase::DEPLOY_RUN);
            }
            break;
        }

        // -----------------------------------------------------------------
        // DEPLOY_RUN / RETRACT_RUN: Measure stall current in each direction
        // -----------------------------------------------------------------
        case CalibPhase::DEPLOY_RUN:
        case CalibPhase::RETRACT_RUN: {
            bool isDeployPhase = (_phase == CalibPhase::DEPLOY_RUN);

            // Periodic status emission to client
            if (now - _lastStatusEmit_ms >= CalibConfig::STATUS_INTERVAL_ms) {
                _lastStatusEmit_ms = now;
                _emitStatus(_phase);
            }

            // Sample current at regular intervals
            if (now - _lastSample_ms >= CalibConfig::SAMPLE_INTERVAL_ms) {
                _lastSample_ms = now;
                uint16_t current_mA = _currentFn();

                // Phase 1: Skip inrush spike (first 100ms)
                if (elapsed < CalibConfig::INRUSH_IGNORE_ms) {
                    break;
                }

                // Phase 2: Baseline sampling window (100-500ms) — average, not max
                if (elapsed < CalibConfig::STARTUP_IGNORE_ms) {
                    _baselineSum += current_mA;
                    _baselineCount++;
                    break;
                }

                // Phase 3: Compute baseline once after window closes
                if (_baselineCount > 0) {
                    _baseline_mA = (uint16_t)(_baselineSum / _baselineCount);
                    _baselineCount = 0;
                    _baselineSum = 0;
                }

                // Track peak current for this direction
                uint16_t& peak = isDeployPhase ? _peakDeploy_mA : _peakRetract_mA;
                if (current_mA > peak) {
                    peak = current_mA;
                }

                // Safety cutoff: abort if current exceeds absolute safety limit
                if (current_mA >= CalibConfig::SAFETY_LIMIT_mA) {
                    _motorFn(0);
                    if (isDeployPhase) {
                        _phase = CalibPhase::MID_SETTLE;
                        _phaseStart_ms = now;
                        _stallDetected = false;
                        _emitStatus(CalibPhase::MID_SETTLE);
                    } else {
                        _finish();
                    }
                    break;
                }

                // Detect stall: current significantly above baseline
                uint16_t stallThreshold = _baseline_mA + CalibConfig::STALL_RISE_mA;
                if (current_mA > stallThreshold) {
                    if (!_stallDetected) {
                        _stallDetected = true;
                        _stallStart_ms = now;
                    } else if (now - _stallStart_ms >= CalibConfig::STALL_CONFIRM_ms) {
                        // Stall confirmed — stop motor and proceed
                        _motorFn(0);
                        if (isDeployPhase) {
                            _phase = CalibPhase::MID_SETTLE;
                            _phaseStart_ms = now;
                            _stallDetected = false;
                            _emitStatus(CalibPhase::MID_SETTLE);
                        } else {
                            _finish();
                        }
                    }
                } else {
                    _stallDetected = false;
                }
            }

            // Timeout: end this phase regardless
            if (elapsed >= CalibConfig::TIMEOUT_ms) {
                _motorFn(0);
                if (isDeployPhase) {
                    _phase = CalibPhase::MID_SETTLE;
                    _phaseStart_ms = now;
                    _stallDetected = false;
                    _emitStatus(CalibPhase::MID_SETTLE);
                } else {
                    _finish();
                }
            }
            break;
        }

        // -----------------------------------------------------------------
        // MID_SETTLE: Settling between deploy and retract measurement
        // -----------------------------------------------------------------
        case CalibPhase::MID_SETTLE: {
            // Wait for motor to settle before reversing
            if (elapsed >= CalibConfig::SETTLE_TIME_ms) {
                // Start motor in retract direction
                _phase = CalibPhase::RETRACT_RUN;
                _phaseStart_ms = now;
                _lastSample_ms = now;
                _lastStatusEmit_ms = now;
                _baseline_mA = 0;
                _baselineSum = 0;
                _baselineCount = 0;
                _stallDetected = false;
                _motorFn(-1);
                _emitStatus(CalibPhase::RETRACT_RUN);
            }
            break;
        }

        // -----------------------------------------------------------------
        // CLOSING_DOORS: Wait for doors to close after calibration
        // -----------------------------------------------------------------
        case CalibPhase::CLOSING_DOORS: {
            _doorSeq->update();
            if (_doorSeq->isComplete()) {
                _phase = _pendingResult;
                _emitStatus(_pendingResult);
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Calibration Completion
// ============================================================================

void StallCalibrator::_finish() {
    _motorFn(0);

    // Take the minimum of the two measured stall peaks with safety margin
    uint16_t deployPeak = _peakDeploy_mA;
    uint16_t retractPeak = _peakRetract_mA;

    // Use the lower of the two peaks (conservative), or whichever is non-zero
    uint16_t measuredStall = 0;
    if (deployPeak > 0 && retractPeak > 0) {
        measuredStall = (deployPeak < retractPeak) ? deployPeak : retractPeak;
    } else {
        measuredStall = (deployPeak > 0) ? deployPeak : retractPeak;
    }

    if (measuredStall > 0) {
        // Apply safety margin (80% of measured stall)
        _result.stallThreshold_mA = (uint16_t)(measuredStall * CalibConfig::MARGIN_FACTOR);
        _result.baseline_mA = _baseline_mA;
        _result.peakDeploy_mA = _peakDeploy_mA;
        _result.peakRetract_mA = _peakRetract_mA;

        // Update the existing calibration value for status emission
        _existingCalibStall_mA = _result.stallThreshold_mA;

        // Close doors if they were opened for calibration
        if (_doorSeq->mode() != DoorMode::NONE) {
            _doorSeq->startClose();
            _phase = CalibPhase::CLOSING_DOORS;
            _pendingResult = CalibPhase::COMPLETE;
        } else {
            _phase = CalibPhase::COMPLETE;
            _emitStatus(CalibPhase::COMPLETE);
        }
    } else {
        // No stall detected in either direction
        _result.peakDeploy_mA = _peakDeploy_mA;
        _result.peakRetract_mA = _peakRetract_mA;

        if (_doorSeq->mode() != DoorMode::NONE) {
            _doorSeq->startClose();
            _phase = CalibPhase::CLOSING_DOORS;
            _pendingResult = CalibPhase::ERROR;
        } else {
            _phase = CalibPhase::ERROR;
            _emitStatus(CalibPhase::ERROR);
        }
    }
}

// ============================================================================
// Status Emission
// ============================================================================

void StallCalibrator::_emitStatus(CalibPhase phase) {
    if (!_progressCb) return;

    GearControlCalibStatus status;
    status.gearId = _gearId;
    status.phase = phase;
    status.current_mA = _currentFn();

    // Report peak for current direction
    if (phase == CalibPhase::CLEAR_RUN || phase == CalibPhase::CLEAR_SETTLE ||
        phase == CalibPhase::OPENING_DOORS || phase == CalibPhase::CLOSING_DOORS) {
        status.peak_mA = 0;  // No measurement during clearing/door phases
    } else if (phase == CalibPhase::DEPLOY_RUN || phase == CalibPhase::MID_SETTLE) {
        status.peak_mA = _peakDeploy_mA;
    } else if (phase == CalibPhase::RETRACT_RUN) {
        status.peak_mA = _peakRetract_mA;
    } else {
        // COMPLETE/ERROR/CANCELLED: report the larger peak
        status.peak_mA = (_peakDeploy_mA > _peakRetract_mA)
                          ? _peakDeploy_mA : _peakRetract_mA;
    }

    status.calibratedStall_mA = _existingCalibStall_mA;

    // Terminal phases indicate calibration is finished
    status.finished = (phase == CalibPhase::COMPLETE ||
                       phase == CalibPhase::ERROR ||
                       phase == CalibPhase::CANCELLED);

    _progressCb(status);
}
