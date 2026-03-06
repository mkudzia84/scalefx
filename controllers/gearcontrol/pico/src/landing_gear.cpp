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
    _gearConfig.flags = GearConfigFlags::CLOSE_DOORS_ON_RETRACT;
    _gearConfig.stallCurrent_mA = LandingGearConfig::DEFAULT_STALL_CURRENT_mA;
    _gearConfig.timeout_ms = LandingGearConfig::DEFAULT_MOTOR_TIMEOUT_ms;

    _doorConfig.gearId = gearId;
    _doorConfig.open0_us = 2000;
    _doorConfig.close0_us = 1000;
    _doorConfig.open1_us = 2000;
    _doorConfig.close1_us = 1000;

    // Initialize door sequencer with pointers to our servo objects
    _doorSeq.begin(&_doorServos[0], &_doorServos[1]);
    _doorSeq.setConfig(_doorConfig);

    // Initialize stall calibrator with motor control and current reading callbacks
    _calibrator.begin(gearId,
        [this](int8_t dir) { setMotor(dir); },
        [this]() { return readMotorCurrent_mA(); },
        &_doorSeq);

    _state = GearState::UNKNOWN;
    _seq.step = GearSeqStep::IDLE;
    _seq.motorRunning = false;
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

void LandingGear::setDoorConfig(const GearControlDoorConfig& config) {
    _doorConfig = config;
    _doorConfig.gearId = _gearId;  // Enforce our gear ID
    _doorSeq.setConfig(_doorConfig);
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
    // Already deployed
    if (_state == GearState::DEPLOYED) return SerialError::OK;

    // Busy with another sequence
    if (_seq.step != GearSeqStep::IDLE) return GearControlError::GEAR_BUSY;

    _seq.deploying = true;
    _seq.motorRunning = false;
    _seq.sequenceStartTime_ms = millis();
    _state = GearState::DEPLOYING;
    _emergencyDeploy = false;  // Normal deploy clears emergency flag

    if (_doorSeq.mode() == DoorMode::NONE) {
        if (_seq.syncMode) {
            // No doors but sync mode — wait at barrier for other gears' doors
            _seq.step = GearSeqStep::SYNC_DOORS_OPEN;
            _seq.stepStartTime_ms = millis();
            _emitSeqProgress();
        } else {
            // No doors, no sync — start motor directly
            advanceToMotor(millis());
        }
    } else {
        _doorSeq.startOpen();
        _seq.step = GearSeqStep::OPENING_DOORS;
        _seq.stepStartTime_ms = millis();
        _emitSeqProgress();  // Opening doors phase started
    }

    return SerialError::OK;
}

uint8_t LandingGear::retract() {
    // Already retracted
    if (_state == GearState::RETRACTED) return SerialError::OK;

    // Busy with another sequence
    if (_seq.step != GearSeqStep::IDLE) return GearControlError::GEAR_BUSY;

    _seq.deploying = false;
    _seq.motorRunning = false;
    _seq.sequenceStartTime_ms = millis();
    _state = GearState::RETRACTING;

    if (_doorSeq.mode() == DoorMode::NONE) {
        if (_seq.syncMode) {
            _seq.step = GearSeqStep::SYNC_DOORS_OPEN;
            _seq.stepStartTime_ms = millis();
            _emitSeqProgress();
        } else {
            advanceToMotor(millis());
        }
    } else {
        _doorSeq.startOpen();
        _seq.step = GearSeqStep::OPENING_DOORS;
        _seq.stepStartTime_ms = millis();
        _emitSeqProgress();  // Opening doors phase started
    }

    return SerialError::OK;
}

void LandingGear::stop() {
    stopMotor();
    _seq.step = GearSeqStep::IDLE;
    _seq.motorRunning = false;
    _seq.syncMode = false;
    _calibrator.reset();
    // Don't change state - leave as last known
}

uint8_t LandingGear::calibrate() {
    // Must not be busy with a sequence
    if (_seq.step != GearSeqStep::IDLE) return GearControlError::GEAR_BUSY;

    // Must not already be calibrating
    if (_calibrator.isActive()) return GearControlError::GEAR_BUSY;

    // Must have a current monitor
    if (!_currentMonitor) return GearControlError::NO_CURRENT_MONITOR;

    _state = GearState::CALIBRATING;
    _calibrator.setExistingCalibration(_calibratedStall_mA);
    return _calibrator.start(_gearConfig.stallCurrent_mA);
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
    updateSequence();

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
        _calibrator.reset();
    } else if (cp == CalibPhase::CANCELLED) {
        _state = GearState::UNKNOWN;
        _calibrator.reset();
    }

    // Update status LEDs
    updateLEDs();
}

void LandingGear::shutdown() {
    stopMotor();
    _seq.step = GearSeqStep::IDLE;
    _seq.motorRunning = false;
    _calibrator.reset();
    _doorSeq.reset();

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
    _seq.step = GearSeqStep::IDLE;
    _seq.motorRunning = false;
    _calibrator.reset();
    _doorSeq.reset();
    _emergencyDeploy = false;
    // State is preserved for the main module to query
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
 * Maps internal GearSeqStep to wire-format GearSeqPhase constants and
 * sends a GearControlSeqStatus update. Called at each phase transition
 * during deploy/retract sequences.
 */
void LandingGear::_emitSeqProgress(bool finished) {
    if (!_seqProgressCb) return;

    GearControlSeqStatus ss;
    ss.gearId = _gearId;
    ss.deploying = _seq.deploying;
    ss.finished = finished;
    ss.elapsed_ms = millis() - _seq.sequenceStartTime_ms;

    // Map internal enum to wire-format phase constants
    switch (_seq.step) {
        case GearSeqStep::OPENING_DOORS:    ss.phase = GearSeqPhase::OPENING_DOORS; break;
        case GearSeqStep::SYNC_DOORS_OPEN:  ss.phase = GearSeqPhase::SYNC_WAIT;     break;
        case GearSeqStep::RUNNING_MOTOR:    ss.phase = GearSeqPhase::RUNNING_MOTOR; break;
        case GearSeqStep::SYNC_MOTOR_DONE:  ss.phase = GearSeqPhase::SYNC_WAIT;     break;
        case GearSeqStep::CLOSING_DOORS:    ss.phase = GearSeqPhase::CLOSING_DOORS; break;
        case GearSeqStep::ERROR:            ss.phase = GearSeqPhase::SEQ_ERROR;     break;
        default:                            ss.phase = GearSeqPhase::IDLE;          break;
    }

    _seqProgressCb(ss);
}

/**
 * @brief Complete the motor phase of a gear sequence
 *
 * Stops motor, checks close-doors flag, transitions to CLOSING_DOORS or
 * directly to IDLE/DEPLOYED/RETRACTED. Used by:
 *   - RUNNING_MOTOR: motor not detected (unplugged, peak < 20mA)
 *   - RUNNING_MOTOR: stall detected (normal completion)
 */
void LandingGear::_completeSequence(uint32_t now) {
    stopMotor();
    _seq.motorRunning = false;

    if (_seq.syncMode) {
        // Wait at sync barrier for other gears' motors to finish
        _seq.step = GearSeqStep::SYNC_MOTOR_DONE;
        _seq.stepStartTime_ms = now;
        _emitSeqProgress();
        return;
    }

    bool shouldCloseDoors = false;
    if (_seq.deploying) {
        shouldCloseDoors = (_gearConfig.flags & GearConfigFlags::CLOSE_DOORS_ON_DEPLOY) != 0;
    } else {
        shouldCloseDoors = (_gearConfig.flags & GearConfigFlags::CLOSE_DOORS_ON_RETRACT) != 0;
    }

    if (shouldCloseDoors && _doorSeq.mode() != DoorMode::NONE) {
        _doorSeq.startClose();
        _seq.step = GearSeqStep::CLOSING_DOORS;
        _seq.stepStartTime_ms = now;
        _emitSeqProgress();  // Closing doors phase started
    } else {
        completeDoorClose();
    }
}

void LandingGear::updateSequence() {
    if (_seq.step == GearSeqStep::IDLE) return;

    uint32_t now = millis();

    switch (_seq.step) {
        case GearSeqStep::OPENING_DOORS: {
            _doorSeq.update();
            if (_doorSeq.isComplete()) {
                if (_seq.syncMode) {
                    // Wait at sync barrier for other gears
                    _seq.step = GearSeqStep::SYNC_DOORS_OPEN;
                    _seq.stepStartTime_ms = now;
                    _emitSeqProgress();
                } else {
                    advanceToMotor(now);
                }
            }
            break;
        }

        case GearSeqStep::SYNC_DOORS_OPEN:
            // Waiting for coordinator — no-op
            break;

        case GearSeqStep::SYNC_MOTOR_DONE:
            // Waiting for coordinator — no-op
            break;

        case GearSeqStep::RUNNING_MOTOR: {
            uint16_t current_mA = readMotorCurrent_mA();
            auto result = _stallDetector.update(current_mA);
            switch (result) {
                case StallDetector::Result::RUNNING:
                    break;
                case StallDetector::Result::STALL_CONFIRMED:
                case StallDetector::Result::TIMEOUT_STALL:
                case StallDetector::Result::NO_MOTOR:
                    _completeSequence(now);
                    break;
                case StallDetector::Result::TIMEOUT_ERROR:
                    stopMotor();
                    _seq.motorRunning = false;
                    _state = GearState::ERROR;
                    _seq.step = GearSeqStep::ERROR;
                    _emitSeqProgress(true);  // Sequence error, finished
                    break;
            }
            break;
        }

        case GearSeqStep::CLOSING_DOORS: {
            _doorSeq.update();
            if (_doorSeq.isComplete()) {
                completeDoorClose();  // Also emits finished progress
            }
            break;
        }

        case GearSeqStep::ERROR:
            // Stay in error until reset via stop() or reset()
            break;

        default:
            break;
    }
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

void LandingGear::setDoorMode(uint8_t mode, uint16_t delay_ms) {
    _doorSeq.setMode(mode, delay_ms);
}

// ============================================================================
// Door Sequencing Helpers
// ============================================================================

// beginDoorOpen() and beginDoorClose() — removed, logic moved to DoorSequencer

/**
 * @brief Transition from door-opening phase to motor-running phase
 */
void LandingGear::advanceToMotor(uint32_t now) {
    setMotor(_seq.deploying ? 1 : -1);
    _seq.motorRunning = true;

    // Configure and start stall detector
    StallDetector::Config cfg;
    cfg.startupIgnore_ms = LandingGearConfig::STARTUP_IGNORE_ms;
    cfg.motorDetectThreshold_mA = LandingGearConfig::MOTOR_DETECT_THRESHOLD_mA;
    cfg.stallThreshold_mA = effectiveStallThreshold_mA();
    cfg.stallConfirm_ms = LandingGearConfig::STALL_CONFIRM_ms;
    cfg.timeout_ms = _gearConfig.timeout_ms;
    _stallDetector.configure(cfg);
    _stallDetector.start();

    _seq.step = GearSeqStep::RUNNING_MOTOR;
    _seq.stepStartTime_ms = now;
    _emitSeqProgress();  // Running motor phase started
}

/**
 * @brief Complete the door-closing phase and finalize the sequence
 */
void LandingGear::completeDoorClose() {
    _state = _seq.deploying ? GearState::DEPLOYED : GearState::RETRACTED;
    _seq.step = GearSeqStep::IDLE;
    _seq.syncMode = false;  // Clear sync mode when sequence finishes
    _emitSeqProgress(true);  // Sequence complete
}

/**
 * @brief Advance past a sync barrier (called by coordinator)
 *
 * SYNC_DOORS_OPEN → start motor
 * SYNC_MOTOR_DONE → close doors (or complete if no close configured)
 */
void LandingGear::advanceSyncPhase() {
    uint32_t now = millis();

    if (_seq.step == GearSeqStep::SYNC_DOORS_OPEN) {
        advanceToMotor(now);
    } else if (_seq.step == GearSeqStep::SYNC_MOTOR_DONE) {
        bool shouldCloseDoors = false;
        if (_seq.deploying) {
            shouldCloseDoors = (_gearConfig.flags & GearConfigFlags::CLOSE_DOORS_ON_DEPLOY) != 0;
        } else {
            shouldCloseDoors = (_gearConfig.flags & GearConfigFlags::CLOSE_DOORS_ON_RETRACT) != 0;
        }

        if (shouldCloseDoors && _doorSeq.mode() != DoorMode::NONE) {
            _doorSeq.startClose();
            _seq.step = GearSeqStep::CLOSING_DOORS;
            _seq.stepStartTime_ms = now;
            _emitSeqProgress();
        } else {
            completeDoorClose();
        }
    }
}

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
