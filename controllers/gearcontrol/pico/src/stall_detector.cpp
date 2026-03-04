/*
 * Stall Detector Module - Implementation
 *
 * Motor stall detection state machine extracted from LandingGear.
 */

#include "stall_detector.h"

void StallDetector::configure(const Config& config) {
    _config = config;
}

void StallDetector::start() {
    _startTime_ms = millis();
    _peakStartup_mA = 0;
    _startupChecked = false;
    _stallConfirming = false;
    _stallConfirmStart_ms = 0;
    _active = true;
}

StallDetector::Result StallDetector::update(uint16_t current_mA) {
    if (!_active) return Result::RUNNING;

    uint32_t now = millis();
    uint32_t elapsed = now - _startTime_ms;

    // ----------------------------------------------------------------
    // Phase 1: Startup ignore — track peak current for motor detection
    // ----------------------------------------------------------------
    if (elapsed <= _config.startupIgnore_ms) {
        if (current_mA > _peakStartup_mA) {
            _peakStartup_mA = current_mA;
        }
        return Result::RUNNING;
    }

    // ----------------------------------------------------------------
    // Phase 2: Motor presence check (once after startup)
    // ----------------------------------------------------------------
    if (!_startupChecked) {
        _startupChecked = true;
        if (_peakStartup_mA < _config.motorDetectThreshold_mA) {
            // INA226 present but no current drawn → motor not connected
            _active = false;
            return Result::NO_MOTOR;
        }
    }

    // ----------------------------------------------------------------
    // Phase 3: Stall confirmation — current must stay above threshold
    // for stallConfirm_ms to prevent false triggers from transient drag
    // ----------------------------------------------------------------
    bool aboveThreshold = (current_mA > _config.stallThreshold_mA);

    if (aboveThreshold) {
        if (!_stallConfirming) {
            _stallConfirming = true;
            _stallConfirmStart_ms = now;
        } else if (now - _stallConfirmStart_ms >= _config.stallConfirm_ms) {
            // Stall confirmed — normal completion
            _active = false;
            return Result::STALL_CONFIRMED;
        }
    } else {
        // Current dropped below threshold — reset confirmation
        _stallConfirming = false;
    }

    // ----------------------------------------------------------------
    // Phase 4: Timeout handling
    // ----------------------------------------------------------------
    if (elapsed > _config.timeout_ms) {
        _active = false;
        if (_stallConfirming) {
            // Timeout during stall confirmation — accept as stall
            return Result::TIMEOUT_STALL;
        } else {
            // Motor was detected but no stall at all = fault
            return Result::TIMEOUT_ERROR;
        }
    }

    return Result::RUNNING;
}
