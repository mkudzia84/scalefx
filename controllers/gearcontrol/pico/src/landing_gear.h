/*
 * Landing Gear Module - Header
 *
 * Encapsulates a single landing gear unit with all associated hardware:
 *   - 0–2× ServoControl for door servos (optional, mode-configurable)
 *   - 2× LedControl for status LEDs (deploy/retract indicators)
 *   - 1× Motor H-bridge pair (CW/CCW GPIOs)
 *   - 1× INA226 reference for motor current monitoring
 *
 * Door modes:
 *   NONE       - No door servos (motor only)
 *   SINGLE     - One door servo (servo 0 only)
 *   DUAL_SYNC  - Two doors, activated simultaneously (default)
 *   DUAL_DELAY - Two doors, door 1 starts after configurable delay
 *   DUAL_SEQ   - Two doors, door 1 starts after door 0 completes
 *
 * For DUAL_DELAY and DUAL_SEQ: doors open 0→1, close 1→0
 * (inner door opens first, closes last — mimics real aircraft).
 *
 * Provides the gear deploy/retract state machine, door open/close,
 * motor stall detection, and status LED updates.
 *
 * In-flight drag heuristic:
 *   Static calibration measures stall current on the ground. In flight,
 *   aerodynamic drag increases the motor's running current, narrowing the
 *   gap between free-running and stall. Two compensations are applied:
 *   1. Drag headroom: raises the effective stall threshold by a percentage
 *      of the calibrated baseline (free-running) current.
 *   2. Stall confirmation: requires current to stay above threshold for
 *      a sustained period before declaring stall, preventing false triggers
 *      from transient drag gusts.
 *
 * Usage:
 *   LandingGear gear;
 *   gear.begin(0, motorCwPin, motorCcwPin);
 *   gear.attachDoorServo(0, doorPin0);
 *   gear.attachDoorServo(1, doorPin1);
 *   gear.attachStatusLed(0, ledCwPin);
 *   gear.attachStatusLed(1, ledCcwPin);
 *   gear.attachCurrentMonitor(&ina226);
 *   gear.setDoorConfig(doorCfg);
 *   gear.setGearConfig(gearCfg);
 *
 *   // In loop:
 *   gear.update();
 */

#ifndef LANDING_GEAR_H
#define LANDING_GEAR_H

#include <Arduino.h>
#include <srv_control.h>
#include <led_control.h>
#include <ina226.h>
#include <serial_gearcontrol.h>  // GearState, config structs, CalibPhase, DoorMode
#include "gear_sequencer.h"
#include "stall_calibrator.h"

// ============================================================================
// Constants
// ============================================================================

namespace LandingGearConfig {
    constexpr uint8_t DOOR_COUNT = 2;         // Door servos per gear
    constexpr uint8_t LED_COUNT  = 2;         // Status LEDs per gear (CW/CCW)

    // Default stall detection
    constexpr uint16_t STARTUP_IGNORE_ms         = 500;      // Ignore stall during motor startup

    // Default stall detection
    constexpr uint16_t DEFAULT_STALL_CURRENT_mA = 500;
    constexpr uint16_t DEFAULT_MOTOR_TIMEOUT_ms = 60000;
    constexpr uint16_t MOTOR_DETECT_THRESHOLD_mA = 20;   // Min current to consider motor present
    constexpr uint16_t STALL_CONFIRM_ms          = 200;   // Stall current must be sustained this long

    // In-flight drag headroom (applied to calibrated gears only)
    constexpr uint8_t  DRAG_HEADROOM_PCT         = 20;    // % of baseline added to stall threshold

    // LED blink rates
    constexpr uint16_t BLINK_TRANSITION_ms = 250;       // Blink rate during deploy/retract
    constexpr uint16_t BLINK_ERROR_ms      = 100;       // Fast blink rate on error
    constexpr uint16_t BLINK_EMERGENCY_ms  = 500;       // Slow blink rate for emergency deploy
    constexpr uint16_t BLINK_CALIBRATE_ms  = 150;       // Blink rate during calibration
}

// ============================================================================
// LandingGear Class
// ============================================================================

/**
 * @brief Encapsulates a single landing gear unit
 *
 * Binds together door servos (via DoorSequencer), status LEDs, motor H-bridge,
 * current monitoring (via StallDetector), and calibration into a cohesive module
 * with deploy/retract sequencing.
 *
 * Door servos use ServoControl from the components library for smooth
 * motion profiling (speed, acceleration, deceleration). The DoorSequencer
 * handles mode-aware open/close sequencing.
 */
class LandingGear {
public:
    LandingGear() = default;
    ~LandingGear() = default;

    // Non-copyable (contains ServoControl objects)
    LandingGear(const LandingGear&) = delete;
    LandingGear& operator=(const LandingGear&) = delete;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize the landing gear module
     * @param gearId Gear index (0-2)
     * @param motorCwPin GPIO pin for motor CW (deploy/forward)
     * @param motorCcwPin GPIO pin for motor CCW (retract/reverse)
     */
    void begin(uint8_t gearId, uint8_t motorCwPin, uint8_t motorCcwPin);

    /**
     * @brief Attach a door servo
     * @param doorIndex Door index (0 or 1)
     * @param pin GPIO pin for servo signal
     * @param minUs Minimum pulse width (default 500)
     * @param maxUs Maximum pulse width (default 2500)
     * @param initialUs Initial position (default 1500)
     * @return true if attachment succeeded
     */
    bool attachDoorServo(uint8_t doorIndex, int pin,
                         int minUs = 500, int maxUs = 2500, int initialUs = 1500);

    /**
     * @brief Attach a status LED
     * @param ledIndex LED index: 0 = deploy (CW), 1 = retract (CCW)
     * @param pin GPIO pin for LED
     * @return true if attachment succeeded
     */
    bool attachStatusLed(uint8_t ledIndex, int pin);

    /**
     * @brief Attach an INA226 current monitor for motor stall detection
     * @param monitor Pointer to initialized INA226 instance
     */
    void attachCurrentMonitor(INA226* monitor);

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set gear behavior configuration
     * @param config Gear configuration (flags, stall current, timeout)
     */
    void setGearConfig(const GearControlGearConfig& config);

    /**
     * @brief Set door servo positions for open/close
     * @param config Door configuration (open/close positions for both servos)
     */
    void setDoorConfig(const GearControlDoorConfig& config);

    /**
     * @brief Configure a door servo's motion profile
     *
     * Matches the GunFX/LightFX SRV_SETTINGS pattern:
     * sets position limits and trapezoidal motion profile.
     *
     * @param doorIndex Door index (0 or 1)
     * @param minUs Minimum pulse width limit
     * @param maxUs Maximum pulse width limit
     * @param maxSpeed_usPerSec Maximum speed in µs/second
     * @param accel_usPerSec2 Acceleration in µs/second²
     * @param decel_usPerSec2 Deceleration in µs/second²
     */
    void configureDoorServo(uint8_t doorIndex, int minUs, int maxUs,
                            int maxSpeed_usPerSec, int accel_usPerSec2,
                            int decel_usPerSec2);

    /**
     * @brief Set door activation modes
     *
     * Two modes control the full deploy/retract cycle:
     *   doorPreDeploy  - Doors opened before deploy motor, closed after retract motor.
     *   doorPostDeploy - Doors closed after deploy motor, opened before retract motor.
     *                    (Retract is the reverse of the deploy operation.)
     *                    NONE = skip post-deploy close and pre-retract open.
     *
     * @param preDeployMode Pre-deploy DoorMode (deploy: open doors, retract: close doors)
     * @param postDeployMode Post-deploy DoorMode (deploy: close doors, retract: open doors; NONE=skip)
     * @param delay_ms Delay between doors in ms (only used for DUAL_DELAY mode)
     */
    void setDoorMode(uint8_t preDeployMode, uint8_t postDeployMode = DoorMode::NONE, uint16_t delay_ms = 500);

    /** @brief Get pre-deploy door mode (deploy: open doors, retract: close doors) */
    uint8_t doorPreDeploy() const { return _doorSeq.preDeployMode(); }

    /** @brief Get post-deploy door mode (deploy: close doors, retract: open doors) */
    uint8_t doorPostDeploy() const { return _doorSeq.postDeployMode(); }

    /** @brief Get current door delay (for DUAL_DELAY mode) */
    uint16_t doorDelay_ms() const { return _doorSeq.delay_ms(); }

    /**
     * @brief Get reference to a door ServoControl
     * @param doorIndex Door index (0 or 1)
     * @return Reference to the door ServoControl
     */
    ServoControl& doorServo(uint8_t doorIndex);

    /**
     * @brief Get const reference to a door ServoControl
     * @param doorIndex Door index (0 or 1)
     * @return Const reference to the door ServoControl
     */
    const ServoControl& doorServo(uint8_t doorIndex) const;

    // ========================================================================
    // Gear Operations
    // ========================================================================

    /**
     * @brief Start deploy sequence (open doors → run motor → optional close)
     * @return Error code (SerialError::OK on success)
     */
    uint8_t deploy();

    /**
     * @brief Start retract sequence (open doors → run motor → close doors)
     * @return Error code (SerialError::OK on success)
     */
    uint8_t retract();

    /**
     * @brief Emergency stop motor and abort any sequence
     */
    void stop();

    /**
     * @brief Start stall current calibration
     *
     * Opens doors first (if configured), then runs the motor in each
     * direction to detect stall current. On completion, doors are closed
     * and the detected stall threshold is saved to gearConfig.
     *
     * The gear must not be busy. An INA226 current monitor must be attached.
     *
     * @param overallTimeout_ms Overall timeout for calibration (0 = no timeout)
     * @return Error code (SerialError::OK on success)
     */
    uint8_t calibrate(uint32_t overallTimeout_ms = 0);

    /**
     * @brief Cancel an in-progress calibration
     *
     * Stops the motor and closes doors (if open). Returns the gear to
     * UNKNOWN state. A CANCELLED status update is emitted via the
     * progress callback after doors finish closing.
     *
     * @return Error code (SerialError::OK on success, NOT_CALIBRATING if idle)
     */
    uint8_t cancelCalibration();

    /**
     * @brief Clear error state
     *
     * If gear is in ERROR state, transitions to UNKNOWN and clears the
     * error reason. Safe to call on non-errored gears (no-op).
     */
    void clearError();

    /**
     * @brief Enable or disable this gear channel
     *
     * When disabled, deploy/retract/calibrate commands are rejected.
     * stop() always works (safety). Active sequences are stopped.
     *
     * @param enabled true to enable, false to disable
     */
    void setEnabled(bool enabled);

    /** @brief Check if gear channel is enabled */
    bool isEnabled() const { return _enabled; }

    /**
     * @brief Register callback for calibration progress updates
     *
     * Called on phase transitions and periodically during motor runs.
     * @param cb Callback receiving GearControlCalibStatus
     */
    void onCalibrationProgress(StallCalibrator::ProgressCallback cb) { _calibrator.onProgress(cb); }

    /**
     * @brief Callback for deploy/retract sequence progress updates
     *
     * Called on each phase transition (OPENING_DOORS, RUNNING_MOTOR,
     * CLOSING_DOORS, complete/error). Receives a GearControlSeqStatus
     * struct matching the GEAR_SEQ_STATUS wire format.
     */
    using SeqProgressCallback = std::function<void(const GearControlSeqStatus&)>;

    /**
     * @brief Register callback for deploy/retract sequence progress
     * @param cb Callback receiving GearControlSeqStatus
     */
    void onSequenceProgress(SeqProgressCallback cb) { _seqProgressCb = cb; }

    /**
     * @brief Callback for door state transition updates
     *
     * Called when door state changes (OPENING, OPEN, CLOSING, CLOSED).
     * Receives a GearControlDoorStatus struct matching the
     * GEAR_DOOR_STATUS wire format.
     */
    using DoorStatusCallback = std::function<void(const GearControlDoorStatus&)>;

    /**
     * @brief Register callback for door state transitions
     * @param cb Callback receiving GearControlDoorStatus
     */
    void onDoorStatus(DoorStatusCallback cb) { _doorStatusCb = cb; }

    /**
     * @brief Open all configured door servos immediately
     *
     * Commands all doors based on current door mode:
     *   NONE: no-op, SINGLE: door 0 only, DUAL_*: both doors.
     * For direct control — does not use delay/sequential timing.
     */
    void openDoors();

    /**
     * @brief Close all configured door servos immediately
     *
     * Commands all doors based on current door mode:
     *   NONE: no-op, SINGLE: door 0 only, DUAL_*: both doors.
     * For direct control — does not use delay/sequential timing.
     */
    void closeDoors();

    /**
     * @brief Set a door servo to an explicit position (direct control)
     * @param doorIndex Door index (0 or 1)
     * @param pos_us Position in microseconds
     */
    void setDoorPosition(uint8_t doorIndex, uint16_t pos_us);

    // ========================================================================
    // Motor Control
    // ========================================================================

    /**
     * @brief Set motor direction
     * @param direction 1=deploy(CW), -1=retract(CCW), 0=stop
     */
    void setMotor(int8_t direction);

    /**
     * @brief Stop motor (coast - both pins LOW)
     */
    void stopMotor();

    /**
     * @brief Read current motor draw from INA226
     * @return Current in milliamps (0 if no monitor attached)
     */
    uint16_t readMotorCurrent_mA() const;

    // ========================================================================
    // Update
    // ========================================================================

    /**
     * @brief Update gear sequencing, door servos, and status LEDs
     *
     * Must be called regularly in loop().
     */
    void update();

    /**
     * @brief Perform safe shutdown (stop motor, return servos to default)
     */
    void shutdown();

    /**
     * @brief Reset state for re-initialization
     */
    void reset();

    // ========================================================================
    // State Queries
    // ========================================================================

    /** @brief Get gear index */
    uint8_t gearId() const { return _gearId; }

    /** @brief Get current gear state */
    GearState state() const { return _state; }

    /** @brief Check if a sequence is in progress */
    bool isBusy() const { return _gearSeq.isActive(); }

    /** @brief Check if gear is deployed */
    bool isDeployed() const { return _state == GearState::DEPLOYED; }

    /** @brief Check if gear is retracted */
    bool isRetracted() const { return _state == GearState::RETRACTED; }

    /** @brief Check if gear is in error state */
    bool isError() const { return _state == GearState::ERROR; }

    /** @brief Flag INA226 board fault (I2C init failed) */
    void flagMonitorFault() { _state = GearState::ERROR; }

    /** @brief Mark this gear as emergency-deployed (low voltage / safety trigger) */
    void markEmergencyDeploy() { _emergencyDeploy = true; }

    /** @brief Clear emergency deploy flag (e.g. after manual re-deploy) */
    void clearEmergencyDeploy() { _emergencyDeploy = false; }

    /** @brief Check if gear was deployed by an emergency/safety trigger */
    bool isEmergencyDeployed() const { return _emergencyDeploy && _state == GearState::DEPLOYED; }

    // ========================================================================
    // Sync Mode (for coordinated all-gear operations)
    // ========================================================================

    /** @brief Set sync mode for the NEXT deploy/retract call (not the active sequence) */
    void setSyncMode(bool enabled) { _nextSyncMode = enabled; }

    /** @brief Check if waiting at doors-open sync barrier */
    bool isWaitingSyncDoorsOpen() const { return _gearSeq.isWaitingSyncDoorsOpen(); }

    /** @brief Check if waiting at motor-done sync barrier */
    bool isWaitingSyncMotorDone() const { return _gearSeq.isWaitingSyncMotorDone(); }

    /** @brief Check if still opening doors in sync mode */
    bool isSyncOpeningDoors() const { return _gearSeq.isSyncOpeningDoors(); }

    /** @brief Check if still running motor in sync mode */
    bool isSyncRunningMotor() const { return _gearSeq.isSyncRunningMotor(); }

    /** @brief Advance past a sync barrier (called by coordinator when all gears ready) */
    void advanceSyncPhase();

    /** @brief Check if gear is calibrating */
    bool isCalibrating() const { return _state == GearState::CALIBRATING; }

    /** @brief Get the calibrated stall current (0 if not calibrated) */
    uint16_t calibratedStall_mA() const { return _calibratedStall_mA; }

    /** @brief Get the calibrated free-running baseline (0 if not calibrated) */
    uint16_t calibBaseline_mA() const { return _calibBaseline_mA; }

    /** @brief Get the error reason from the last failed calibration (GearErrorReason code) */
    uint8_t lastCalibErrorReason() const { return _lastCalibErrorReason; }

    /**
     * @brief Get the effective stall threshold with drag headroom
     *
     * For calibrated gears: calibratedStall + (baseline × dragHeadroom%)
     * For uncalibrated gears: configured stallCurrent_mA (no adjustment)
     */
    uint16_t effectiveStallThreshold_mA() const;

    /** @brief Check if calibration has been performed */
    bool isCalibrated() const { return _calibratedStall_mA > 0; }

    /** @brief Set drag headroom percentage (0-200, applied to calibrated baseline) */
    void setDragHeadroom_pct(uint8_t pct) { _dragHeadroom_pct = pct; }

    /** @brief Get drag headroom percentage */
    uint8_t dragHeadroom_pct() const { return _dragHeadroom_pct; }

    /** @brief Get current door servo position */
    uint16_t doorPosition_us(uint8_t doorIndex) const;

    /** @brief Get current door state (DoorState wire value) */
    uint8_t doorState() const { return _doorSeq.doorState(); }

    /** @brief Get the current gear configuration */
    const GearControlGearConfig& gearConfig() const { return _gearConfig; }

    /** @brief Get the current door configuration */
    const GearControlDoorConfig& doorConfig() const { return _doorConfig; }

private:
    void updateLEDs();

    // Identity
    uint8_t _gearId = 0;

    // Door servos (owned here, operated on by DoorSequencer via pointers)
    ServoControl _doorServos[LandingGearConfig::DOOR_COUNT];

    // Door sequencer (mode-aware open/close state machine)
    DoorSequencer _doorSeq;

    // Motor stall detector
    StallDetector _stallDetector;

    // Status LEDs (CW = deploy, CCW = retract)
    LedControl _statusLeds[LandingGearConfig::LED_COUNT];

    // Motor H-bridge pins
    uint8_t _motorCwPin = 0;
    uint8_t _motorCcwPin = 0;
    bool _motorInitialized = false;

    // Current monitor (optional, for stall detection)
    INA226* _currentMonitor = nullptr;

    // State
    GearState _state = GearState::UNKNOWN;
    bool _emergencyDeploy = false;  // Set by emergency/safety deploy triggers
    bool _enabled = true;           // Channel enabled (disabled rejects deploy/retract/calibrate)

    // Op-queue sequencer for deploy/retract lifecycle
    GearSequencer _gearSeq;

    // Pending sync mode — applied when a new sequence starts (not during active sequence)
    bool _nextSyncMode = false;

    // Stall current calibrator (handles calibration state machine)
    StallCalibrator _calibrator;

    // Calibrated stall current (0 = not yet calibrated)
    uint16_t _calibratedStall_mA = 0;

    // Calibrated free-running baseline (persisted from calibration)
    uint16_t _calibBaseline_mA = 0;

    // Error reason from last failed calibration (GearErrorReason code)
    uint8_t _lastCalibErrorReason = 0;

    // In-flight drag headroom (percentage of baseline to add to threshold)
    uint8_t _dragHeadroom_pct = LandingGearConfig::DRAG_HEADROOM_PCT;

    // Configuration
    GearControlGearConfig _gearConfig;
    GearControlDoorConfig _doorConfig;

    // Deploy/retract sequence progress callback
    SeqProgressCallback _seqProgressCb;

    // Door state transition callback
    DoorStatusCallback _doorStatusCb;
    uint8_t _lastEmittedDoorState = DoorState::UNKNOWN;

    // Emit sequence progress to registered callback
    void _emitSeqProgress(bool finished = false);

    // Emit door status to registered callback
    void _emitDoorStatus();
};

#endif // LANDING_GEAR_H
