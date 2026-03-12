/*
 * Stall Calibrator - Header
 *
 * Self-contained calibration state machine for landing gear motor stall
 * current detection.  Extracted from LandingGear to isolate the multi-phase
 * calibration procedure (clear → deploy → settle → retract → finish) from
 * the runtime deploy/retract sequencing.
 *
 * The calibrator operates the motor in both directions, measures peak
 * currents, detects endpoint stalls, and computes a calibrated stall
 * threshold with a safety margin.  It also coordinates door opening/closing
 * around the motor runs when doors are configured.
 *
 * Dependencies are injected via callbacks (motor control, current reading)
 * and a DoorSequencer pointer, keeping the class decoupled from LandingGear.
 *
 * Usage:
 *   StallCalibrator cal;
 *   cal.begin(gearId,
 *       [&](int8_t d){ gear.setMotor(d); },
 *       [&](){ return gear.readMotorCurrent_mA(); },
 *       &doorSeq);
 *   cal.onProgress(progressCb);
 *
 *   // Start calibration:
 *   cal.setExistingCalibration(prevStall_mA);
 *   cal.start(initialGuess_mA);
 *
 *   // In loop():
 *   cal.update();
 *   if (cal.isComplete()) {
 *       auto& r = cal.result();
 *       // apply r.stallThreshold_mA, r.baseline_mA
 *       cal.reset();
 *   }
 */

#ifndef STALL_CALIBRATOR_H
#define STALL_CALIBRATOR_H

#include <Arduino.h>
#include <serial/gearcontrol/gearcontrol.h>  // CalibPhase, GearControlCalibStatus, DoorMode
#include "door_sequencer.h"
#include <functional>

// ============================================================================
// Calibration Constants
// ============================================================================

namespace CalibConfig {
    // Status emission
    constexpr uint16_t STATUS_INTERVAL_ms    = 250;    // Progress update interval to client

    // Endpoint clearing
    constexpr uint16_t CLEAR_TIME_ms         = 2000;   // Max time for endpoint clearing run
    constexpr uint16_t CLEAR_SETTLE_ms       = 500;    // Settle after clearing endpoint

    // Measurement timing
    constexpr uint16_t STARTUP_IGNORE_ms     = 500;    // Total startup ignore (inrush + baseline)
    constexpr uint16_t INRUSH_IGNORE_ms      = 100;    // Skip inrush spike at motor start
    // Baseline window: INRUSH_IGNORE_ms to STARTUP_IGNORE_ms (100-500ms)
    constexpr uint16_t SETTLE_TIME_ms        = 1000;   // Settle between directions
    constexpr uint16_t TIMEOUT_ms            = 60000;  // Max time per direction
    constexpr uint16_t SAMPLE_INTERVAL_ms    = 20;     // Current sampling interval

    // Current thresholds
    constexpr uint16_t SAFETY_LIMIT_mA       = 3000;   // Absolute max current cutoff
    constexpr uint16_t STALL_CONFIRM_ms      = 200;    // Current must stay high this long
    constexpr uint16_t STALL_RISE_mA         = 150;    // Current rise above baseline = stall

    // Motor disconnect detection
    constexpr uint16_t ZERO_CURRENT_ABORT_ms = 1000;   // Abort if current stays 0 this long after baseline
    constexpr uint16_t ZERO_CURRENT_THRESH_mA = 5;     // Below this counts as "zero" (noise floor)

    // Overall calibration timeout
    constexpr uint32_t DEFAULT_OVERALL_TIMEOUT_ms = 60000;  // Default 60s overall timeout

    // Result computation
    constexpr float    MARGIN_FACTOR         = 0.80f;  // Use 80% of detected stall as threshold
}

// ============================================================================
// StallCalibrator Class
// ============================================================================

/**
 * @brief Self-contained stall current calibration state machine
 *
 * Runs the motor in both directions to detect stall current, then computes
 * a calibrated threshold.  Coordinates with DoorSequencer for door
 * opening/closing around calibration motor runs.
 *
 * Phase sequence:
 *   OPENING_DOORS → CLEAR_RUN → CLEAR_SETTLE → DEPLOY_RUN →
 *   MID_SETTLE → RETRACT_RUN → [CLOSING_DOORS →] COMPLETE/ERROR
 */
class StallCalibrator {
public:
    // Callback types for dependency injection
    using MotorControlFn   = std::function<void(int8_t direction)>;
    using CurrentReadFn    = std::function<uint16_t()>;
    using ProgressCallback = std::function<void(const GearControlCalibStatus&)>;

    /**
     * @brief Calibration output values
     *
     * Valid after phase reaches COMPLETE.
     */
    struct Result {
        uint16_t stallThreshold_mA = 0;   // Calibrated threshold (with margin)
        uint16_t baseline_mA = 0;         // Free-running baseline current
        uint16_t peakDeploy_mA = 0;       // Raw deploy peak
        uint16_t peakRetract_mA = 0;      // Raw retract peak
        uint8_t  errorReason = 0;         // GearErrorReason code (0 = none)
    };

    StallCalibrator() = default;
    ~StallCalibrator() = default;

    // ========================================================================
    // Initialization
    // ========================================================================

    /**
     * @brief Initialize calibrator with motor control and current reading callbacks
     * @param gearId Gear index (for status emission)
     * @param motorFn Motor control callback: direction 1=CW, -1=CCW, 0=stop
     * @param currentFn Current reading callback: returns motor current in mA
     * @param doorSeq DoorSequencer for coordinating doors around calibration
     */
    void begin(uint8_t gearId, MotorControlFn motorFn, CurrentReadFn currentFn,
               DoorSequencer* doorSeq);

    // ========================================================================
    // Operations
    // ========================================================================

    /**
     * @brief Start a calibration run
     *
     * Resets all internal state, opens doors (if configured and not already
     * open), then begins the multi-phase calibration sequence.
     * Doors are left open after calibration completes/errors/cancels.
     *
     * @param initialStallGuess_mA Stall threshold for endpoint clearing
     *        (typically the current gear config value)
     * @param overallTimeout_ms Overall timeout for entire calibration
     *        (0 = default 60s, see CalibConfig::DEFAULT_OVERALL_TIMEOUT_ms)
     * @return SerialError::OK on success
     */
    uint8_t start(uint16_t initialStallGuess_mA, uint32_t overallTimeout_ms = 0);

    /**
     * @brief Cancel an in-progress calibration
     *
     * Stops the motor and closes doors (if open). The calibrator transitions
     * to CANCELLED (possibly via CLOSING_DOORS).
     *
     * @return SerialError::OK on success, GearControlError::NOT_CALIBRATING if idle
     */
    uint8_t cancel();

    /**
     * @brief Update the calibration state machine
     *
     * Must be called every loop() iteration while calibration is active.
     */
    void update();

    /** @brief Reset to idle (abort without status emission) */
    void reset() { _phase = CalibPhase::IDLE; }

    // ========================================================================
    // State Queries
    // ========================================================================

    /** @brief Get current calibration phase */
    CalibPhase phase() const { return _phase; }

    /** @brief Check if calibration is actively running (not terminal) */
    bool isActive() const;

    /** @brief Check if calibration completed successfully */
    bool isComplete() const { return _phase == CalibPhase::COMPLETE; }

    /** @brief Check if calibration ended in error */
    bool isError() const { return _phase == CalibPhase::ERROR; }

    /** @brief Check if calibration was cancelled */
    bool isCancelled() const { return _phase == CalibPhase::CANCELLED; }

    /** @brief Get calibration results (valid after COMPLETE) */
    const Result& result() const { return _result; }

    // ========================================================================
    // Configuration
    // ========================================================================

    /** @brief Register callback for calibration progress updates */
    void onProgress(ProgressCallback cb) { _progressCb = cb; }

    /**
     * @brief Set the existing calibrated stall value for status emission
     *
     * This is reported in progress updates as calibratedStall_mA so the
     * client can see the previous/current calibration value.  Updated
     * internally when calibration completes successfully.
     *
     * @param stall_mA Previous calibrated stall threshold (0 if uncalibrated)
     */
    void setExistingCalibration(uint16_t stall_mA) { _existingCalibStall_mA = stall_mA; }

private:
    void _emitStatus(CalibPhase phase);
    void _finish();

    // Dependencies (set via begin())
    uint8_t          _gearId = 0;
    MotorControlFn   _motorFn;
    CurrentReadFn    _currentFn;
    DoorSequencer*   _doorSeq = nullptr;
    ProgressCallback _progressCb;

    // State machine
    CalibPhase _phase = CalibPhase::IDLE;

    // Timing
    uint32_t _phaseStart_ms = 0;
    uint32_t _lastSample_ms = 0;
    uint32_t _lastStatusEmit_ms = 0;
    uint32_t _overallStart_ms = 0;       // Start of entire calibration
    uint32_t _overallTimeout_ms = 0;     // Overall timeout (0 = no timeout)

    // Measurement state
    uint16_t _initialStallGuess_mA = 500;   // For endpoint clearing detection
    uint16_t _baseline_mA = 0;              // No-load current (averaged)
    uint32_t _baselineSum = 0;              // Accumulator for baseline average
    uint16_t _baselineCount = 0;            // Number of baseline samples
    uint16_t _peakDeploy_mA = 0;            // Peak current in deploy direction
    uint16_t _peakRetract_mA = 0;           // Peak current in retract direction
    uint32_t _stallStart_ms = 0;            // When current first exceeded threshold
    bool     _stallDetected = false;         // Stall confirmation in progress
    uint32_t _zeroCurrentStart_ms = 0;       // When current first dropped to ~0
    bool     _zeroCurrentDetected = false;   // Zero-current tracking in progress

    // Status emission
    uint16_t _existingCalibStall_mA = 0;    // Previous calibrated value (for reporting)

    // Output
    Result _result;
};

#endif // STALL_CALIBRATOR_H
