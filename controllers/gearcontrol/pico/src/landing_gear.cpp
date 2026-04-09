/*
 * Landing Gear Module - Implementation
 *
 * Encapsulates a single landing gear unit: door servos, status LEDs,
 * motor H-bridge, and INA226 current monitoring.
 */

#include "landing_gear.h"

using namespace CoreProtocol;

// ============================================================================
// Initialization
// ============================================================================

void LandingGear::begin(uint8_t gearId, uint8_t motorCwPin, uint8_t motorCcwPin) {
    _gearId = gearId;
    _motorCwPin = motorCwPin;
    _motorCcwPin = motorCcwPin;

    // Initialize motor pins
    pinMode(_motorCwPin, OUTPUT);
    pinMode(_motorCcwPin, OUTPUT);
    digitalWrite(_motorCwPin, LOW);
    digitalWrite(_motorCcwPin, LOW);
    _motorInitialized = true;

    // Set default configurations
    _gearConfig.gearId = gearId;
    _gearConfig.flags = 0;  // No flags by default (door closing controlled by doorPostDeploy)
    _gearConfig.stallCurrent_mA = LandingGearConfig::DEFAULT_STALL_CURRENT_mA;
    _gearConfig.timeout_ms = LandingGearConfig::DEFAULT_MOTOR_TIMEOUT_ms;

    // Initialize door sequencer with pointers to our servo objects
    _doorSeq.begin(&_doorServos[0], &_doorServos[1]);

    // Initialize stall calibrator with motor control and current reading callbacks
    _calibrator.begin(gearId,
        [this](int8_t dir) { setMotor(dir); },
        [this]() { return readMotorCurrent_mA(); },
        &_doorSeq);

    // Initialize gear sequencer with door/stall dependencies and motor callbacks
    _gearSeq.begin(&_doorSeq, &_stallDetector,
        [this](int8_t dir) { setMotor(dir); },
        [this]() { return readMotorCurrent_mA(); });

    _gearSeq.onPhaseChange([this](bool finished, bool error) {
        if (error) {
            _state = GearState::ERROR;
        } else if (finished) {
            _state = _gearSeq.deploying() ? GearState::DEPLOYED : GearState::RETRACTED;
        }
        _emitSeqProgress(finished);
    });

    _state = GearState::UNKNOWN;
}

bool LandingGear::attachDoorServo(uint8_t doorIndex, int pin,
                                   int minUs, int maxUs, int initialUs) {
    if (doorIndex >= LandingGearConfig::DOOR_COUNT) return false;
    return _doorServos[doorIndex].begin(pin, minUs, maxUs, initialUs);
}

bool LandingGear::attachStatusLed(uint8_t ledIndex, int pin) {
    if (ledIndex >= LandingGearConfig::LED_COUNT) return false;
    return _statusLeds[ledIndex].begin(pin);
}

void LandingGear::attachCurrentMonitor(INA226* monitor) {
    _currentMonitor = monitor;
}

// ============================================================================
// Configuration
// ============================================================================

void LandingGear::setGearConfig(const GearControlGearConfig& config) {
    _gearConfig = config;
    _gearConfig.gearId = _gearId;  // Enforce our gear ID
}

void LandingGear::configureDoorServo(uint8_t doorIndex, int minUs, int maxUs,
                                      int maxSpeed_usPerSec, int accel_usPerSec2,
                                      int decel_usPerSec2) {
    if (doorIndex >= LandingGearConfig::DOOR_COUNT) return;
    _doorSeq.servo(doorIndex).setLimits(minUs, maxUs);
    _doorSeq.servo(doorIndex).setMotionProfile(maxSpeed_usPerSec, accel_usPerSec2,
                                                decel_usPerSec2);
}

ServoControl& LandingGear::doorServo(uint8_t doorIndex) {
    return _doorSeq.servo(doorIndex);
}

const ServoControl& LandingGear::doorServo(uint8_t doorIndex) const {
    return _doorSeq.servo(doorIndex);
}

// ============================================================================
// Gear Operations
// ============================================================================

uint8_t LandingGear::deploy() {
    // Channel disabled
    if (!_enabled) return GearControlError::GEAR_DISABLED;

    // Already deployed
    if (_state == GearState::DEPLOYED) return SerialError::OK;

    // Can't preempt calibration
    if (_state == GearState::CALIBRATING) return GearControlError::GEAR_BUSY;

    // Already deploying — idempotent
    if (_gearSeq.isActive() && _gearSeq.deploying()) return SerialError::OK;

    // Start deploy (preempts any active retract, including sync sequences)
    _state = GearState::DEPLOYING;
    _emergencyDeploy = false;

    GearSequencer::StallConfig cfg;
    cfg.startupIgnore_ms = LandingGearConfig::STARTUP_IGNORE_ms;
    cfg.motorDetectThreshold_mA = LandingGearConfig::MOTOR_DETECT_THRESHOLD_mA;
    cfg.stallThreshold_mA = effectiveStallThreshold_mA();
    cfg.stallConfirm_ms = LandingGearConfig::STALL_CONFIRM_ms;
    cfg.timeout_ms = _gearConfig.timeout_ms;

    _gearSeq.start(true, _nextSyncMode, cfg);
    return SerialError::OK;
}

uint8_t LandingGear::retract() {
    // Channel disabled
    if (!_enabled) return GearControlError::GEAR_DISABLED;

    // Already retracted
    if (_state == GearState::RETRACTED) return SerialError::OK;

    // Can't preempt calibration
    if (_state == GearState::CALIBRATING) return GearControlError::GEAR_BUSY;

    // Already retracting — idempotent
    if (_gearSeq.isActive() && !_gearSeq.deploying()) return SerialError::OK;

    // Start retract (preempts any active deploy, including sync sequences)
    _state = GearState::RETRACTING;

    GearSequencer::StallConfig cfg;
    cfg.startupIgnore_ms = LandingGearConfig::STARTUP_IGNORE_ms;
    cfg.motorDetectThreshold_mA = LandingGearConfig::MOTOR_DETECT_THRESHOLD_mA;
    cfg.stallThreshold_mA = effectiveStallThreshold_mA();
    cfg.stallConfirm_ms = LandingGearConfig::STALL_CONFIRM_ms;
    cfg.timeout_ms = _gearConfig.timeout_ms;

    _gearSeq.start(false, _nextSyncMode, cfg);
    return SerialError::OK;
}

void LandingGear::stop() {
    stopMotor();
    _gearSeq.stop();
    _calibrator.reset();
    // If gear was calibrating, transition to UNKNOWN
    if (_state == GearState::CALIBRATING) {
        _state = GearState::UNKNOWN;
    }
}

uint8_t LandingGear::calibrate(uint32_t overallTimeout_ms) {
    // Channel disabled
    if (!_enabled) return GearControlError::GEAR_DISABLED;

    // Must not be busy with a sequence
    if (_gearSeq.isActive()) return GearControlError::GEAR_BUSY;

    // Must not already be calibrating
    if (_calibrator.isActive()) return GearControlError::GEAR_BUSY;

    // Must have a current monitor
    if (!_currentMonitor) return GearControlError::NO_CURRENT_MONITOR;

    _state = GearState::CALIBRATING;
    _calibrator.setExistingCalibration(_calibratedStall_mA);
    return _calibrator.start(_gearConfig.stallCurrent_mA, overallTimeout_ms);
}

uint8_t LandingGear::cancelCalibration() {
    return _calibrator.cancel();
}

void LandingGear::openDoors() {
    _doorSeq.openImmediate();
}

void LandingGear::closeDoors() {
    _doorSeq.closeImmediate();
}

void LandingGear::setDoorPosition(uint8_t doorIndex, uint16_t pos_us) {
    _doorSeq.setPosition(doorIndex, pos_us);
}

// ============================================================================
// Motor Control
// ============================================================================

void LandingGear::setMotor(int8_t direction) {
    if (!_motorInitialized) return;

    if (direction > 0) {
        // Deploy (CW): CW=HIGH, CCW=LOW
        digitalWrite(_motorCwPin, HIGH);
        digitalWrite(_motorCcwPin, LOW);
    } else if (direction < 0) {
        // Retract (CCW): CW=LOW, CCW=HIGH
        digitalWrite(_motorCwPin, LOW);
        digitalWrite(_motorCcwPin, HIGH);
    } else {
        // Stop: both LOW (coast)
        digitalWrite(_motorCwPin, LOW);
        digitalWrite(_motorCcwPin, LOW);
    }
}

void LandingGear::stopMotor() {
    setMotor(0);
}

uint16_t LandingGear::readMotorCurrent_mA() const {
    if (!_currentMonitor) return 0;
    float current = _currentMonitor->current_mA();
    return (uint16_t)(current < 0 ? -current : current);  // Absolute value
}

// ============================================================================
// Update
// ============================================================================

void LandingGear::update() {
    // Update door servo motion profiling
    for (int i = 0; i < LandingGearConfig::DOOR_COUNT; i++) {
        if (_doorServos[i].isAttached()) {
            _doorServos[i].update();
        }
    }

    // Update gear sequencing state machine
    _gearSeq.update();

    // Update calibration state machine and handle terminal phases
    _calibrator.update();
    CalibPhase cp = _calibrator.phase();
    if (cp == CalibPhase::COMPLETE) {
        auto& r = _calibrator.result();
        _calibratedStall_mA = r.stallThreshold_mA;
        _calibBaseline_mA = r.baseline_mA;
        _gearConfig.stallCurrent_mA = _calibratedStall_mA;
        _state = GearState::UNKNOWN;
        _calibrator.reset();
    } else if (cp == CalibPhase::ERROR) {
        _state = GearState::ERROR;
        _lastCalibErrorReason = _calibrator.result().errorReason;
        _calibrator.reset();
    } else if (cp == CalibPhase::CANCELLED) {
        _state = GearState::UNKNOWN;
        _calibrator.reset();
    }

    // Update status LEDs
    updateLEDs();

    // Check for door state transitions and emit events
    uint8_t currentDoorState = _doorSeq.doorState();
    if (currentDoorState != _lastEmittedDoorState) {
        _lastEmittedDoorState = currentDoorState;
        _emitDoorStatus();
    }
}

void LandingGear::shutdown() {
    stopMotor();
    _gearSeq.stop();
    _calibrator.reset();
    _doorSeq.reset();

    // If gear was calibrating when shutdown was called, transition to UNKNOWN
    if (_state == GearState::CALIBRATING) {
        _state = GearState::UNKNOWN;
    }

    // Return door servos to center
    for (int i = 0; i < LandingGearConfig::DOOR_COUNT; i++) {
        if (_doorServos[i].isAttached()) {
            _doorServos[i].setPositionImmediate(1500);
        }
    }

    // Turn off status LEDs
    for (int i = 0; i < LandingGearConfig::LED_COUNT; i++) {
        _statusLeds[i].off();
    }
}

void LandingGear::reset() {
    stopMotor();
    _gearSeq.stop();
    _calibrator.reset();
    _doorSeq.reset();
    _emergencyDeploy = false;
    // State is preserved for the main module to query
}

void LandingGear::clearError() {
    if (_state == GearState::ERROR) {
        _state = GearState::UNKNOWN;
        _lastCalibErrorReason = 0;
    }
}

void LandingGear::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        // Stop any active sequence when disabling
        if (_gearSeq.isActive() || _calibrator.isActive()) {
            stop();
        }
    }
}

// ============================================================================
// State Queries
// ============================================================================

uint16_t LandingGear::doorPosition_us(uint8_t doorIndex) const {
    return _doorSeq.position_us(doorIndex);
}

// ============================================================================
// Gear Sequencing State Machine
// ============================================================================

/**
 * @brief Emit a sequence progress update to the registered callback
 *
 * Maps current queue op to wire-format GearSeqPhase constants and sends
 * a GearControlSeqStatus update. Called at each phase transition during
 * deploy/retract sequences.
 */
void LandingGear::_emitSeqProgress(bool finished) {
    if (!_seqProgressCb) return;

    GearControlSeqStatus ss;
    ss.gearId = _gearId;
    ss.deploying = _gearSeq.deploying();
    ss.finished = finished;
    ss.elapsed_ms = _gearSeq.elapsed_ms();

    // Map queue-derived step to wire-format phase
    if (_state == GearState::ERROR) {
        ss.phase = GearSeqPhase::SEQ_ERROR;
    } else {
        GearSeqStep s = _gearSeq.step();
        switch (s) {
            case GearSeqStep::OPENING_DOORS:    ss.phase = GearSeqPhase::OPENING_DOORS; break;
            case GearSeqStep::SYNC_DOORS_OPEN:  ss.phase = GearSeqPhase::SYNC_WAIT;     break;
            case GearSeqStep::RUNNING_MOTOR:    ss.phase = GearSeqPhase::RUNNING_MOTOR; break;
            case GearSeqStep::SYNC_MOTOR_DONE:  ss.phase = GearSeqPhase::SYNC_WAIT;     break;
            case GearSeqStep::CLOSING_DOORS:    ss.phase = GearSeqPhase::CLOSING_DOORS; break;
            default:                            ss.phase = GearSeqPhase::IDLE;          break;
        }
    }

    _seqProgressCb(ss);
}

/**
 * @brief Emit a door status update to the registered callback
 *
 * Builds a GearControlDoorStatus from the current door sequencer state
 * and calls the registered callback. Called when door state transitions
 * are detected in update().
 */
void LandingGear::_emitDoorStatus() {
    if (!_doorStatusCb) return;

    GearControlDoorStatus ds;
    ds.gearId = _gearId;
    ds.state = _doorSeq.doorState();
    ds.door0Pos_us = _doorSeq.position_us(0);  // µs
    ds.door1Pos_us = _doorSeq.position_us(1);  // µs

    _doorStatusCb(ds);
}

// ============================================================================
// Sync Phase Advancement
// ============================================================================

/**
 * @brief Advance past a sync barrier (called by coordinator)
 *
 * Delegates to GearSequencer which checks the current op and advances.
 */
void LandingGear::advanceSyncPhase() {
    _gearSeq.advanceSyncPhase();
}

// ============================================================================
// In-Flight Drag Heuristic
// ============================================================================

/**
 * @brief Calculate effective stall threshold with in-flight drag headroom
 *
 * Static calibration measures stall current on the ground. In flight,
 * aerodynamic drag on the gear mechanism increases the motor's free-running
 * current, narrowing the margin between normal operation and stall detection.
 *
 * For calibrated gears, this adds a percentage of the measured baseline
 * (free-running) current as headroom:
 *   effectiveThreshold = calibratedStall + (baseline × dragHeadroom%)
 *
 * Example with 200mA baseline, 500mA stall peak, 80% margin, 20% headroom:
 *   calibratedStall = 500 × 0.80 = 400mA
 *   dragHeadroom    = 200 × 0.20 = 40mA
 *   effective        = 400 + 40  = 440mA
 *
 * For uncalibrated gears (manual threshold), no adjustment is applied
 * since the user presumably set an appropriate value.
 */
uint16_t LandingGear::effectiveStallThreshold_mA() const {
    uint16_t threshold = _gearConfig.stallCurrent_mA;

    // Add drag headroom for calibrated gears
    if (_calibratedStall_mA > 0 && _calibBaseline_mA > 0 && _dragHeadroom_pct > 0) {
        threshold += (uint16_t)(_calibBaseline_mA * _dragHeadroom_pct / 100);
    }

    return threshold;
}

// ============================================================================
// Door Mode Configuration
// ============================================================================

void LandingGear::setDoorMode(uint8_t preDeployMode, uint8_t postDeployMode, uint16_t delay_ms) {
    _doorSeq.setMode(preDeployMode, postDeployMode, delay_ms);
}

// ============================================================================
// Door Sequencing Helpers
// ============================================================================

// beginDoorOpen() and beginDoorClose() — removed, logic moved to DoorSequencer

// ============================================================================
// Status LED Update
// ============================================================================

void LandingGear::updateLEDs() {
    LedControl& deployLed  = _statusLeds[0];   // CW indicator
    LedControl& retractLed = _statusLeds[1];    // CCW indicator

    switch (_state) {
        case GearState::DEPLOYED:
            if (_emergencyDeploy) {
                // Emergency deployed: deploy LED solid, retract LED slow blink (warning)
                deployLed.on();
                retractLed.set((millis() / LandingGearConfig::BLINK_EMERGENCY_ms) % 2);
            } else {
                deployLed.on();
                retractLed.off();
            }
            break;

        case GearState::RETRACTED:
            deployLed.off();
            retractLed.on();
            break;

        case GearState::DEPLOYING:
        case GearState::RETRACTING: {
            // Alternate deploy/retract LEDs during transition
            bool phase = (millis() / LandingGearConfig::BLINK_TRANSITION_ms) % 2;
            deployLed.set(phase);
            retractLed.set(!phase);
            break;
        }

        case GearState::ERROR:
            // Both LEDs blink fast on error
            {
                bool on = (millis() / LandingGearConfig::BLINK_ERROR_ms) % 2;
                deployLed.set(on);
                retractLed.set(on);
            }
            break;

        case GearState::CALIBRATING:
            // Chase pattern: alternating LEDs at calibration rate
            {
                bool phase = (millis() / LandingGearConfig::BLINK_CALIBRATE_ms) % 2;
                deployLed.set(phase);
                retractLed.set(!phase);
            }
            break;

        default:  // UNKNOWN
            deployLed.off();
            retractLed.off();
            break;
    }
}
