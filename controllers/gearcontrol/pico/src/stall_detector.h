/*
 * Stall Detector Module - Header
 *
 * Motor stall detection with startup ignore, motor presence check,
 * sustained-threshold confirmation, and timeout handling.
 *
 * Encapsulates the stall detection state machine previously embedded
 * in LandingGear's RUNNING_MOTOR sequence step.
 *
 * Usage:
 *   StallDetector detector;
 *   StallDetector::Config cfg = { 500, 20, 500, 200, 10000 };
 *   detector.configure(cfg);
 *   detector.start();
 *
 *   // In loop:
 *   uint16_t current = readCurrent_mA();
 *   auto result = detector.update(current);
 *   switch (result) {
 *       case StallDetector::Result::STALL_CONFIRMED: ...
 *       case StallDetector::Result::NO_MOTOR: ...
 *       case StallDetector::Result::TIMEOUT_ERROR: ...
 *   }
 */

#ifndef STALL_DETECTOR_H
#define STALL_DETECTOR_H

#include <Arduino.h>

// ============================================================================
// StallDetector Class
// ============================================================================

/**
 * @brief Motor stall detection state machine
 *
 * Phases:
 *   1. Startup ignore: Track peak current, ignore stall detection
 *   2. Motor presence check: If peak startup current < threshold, motor absent
 *   3. Stall monitoring: Current must exceed threshold for confirmation period
 *   4. Timeout: Accept partial stall or report fault
 */
class StallDetector {
public:
    /**
     * @brief Result of stall detection update
     */
    enum class Result : uint8_t {
        RUNNING,         ///< Motor running normally, no stall detected
        STALL_CONFIRMED, ///< Stall current sustained above threshold for required duration
        NO_MOTOR,        ///< Motor not detected (startup current below detection threshold)
        TIMEOUT_STALL,   ///< Timed out during stall confirmation — accepted as stall
        TIMEOUT_ERROR    ///< Timed out with no stall indication — fault condition
    };

    /**
     * @brief Configuration for stall detection
     */
    struct Config {
        uint16_t startupIgnore_ms       = 500;   ///< Ignore stall during motor startup
        uint16_t motorDetectThreshold_mA = 20;   ///< Min peak current to consider motor present
        uint16_t stallThreshold_mA       = 500;  ///< Current above this = potential stall
        uint16_t stallConfirm_ms         = 200;  ///< Must stay above threshold this long
        uint16_t timeout_ms              = 10000; ///< Max total motor run time
    };

    StallDetector() = default;
    ~StallDetector() = default;

    /**
     * @brief Configure stall detection parameters
     * @param config Detection thresholds and timing
     */
    void configure(const Config& config);

    /**
     * @brief Start stall detection (resets all internal state)
     *
     * Call this when the motor is started. Then call update() each loop
     * iteration with the current motor current reading.
     */
    void start();

    /**
     * @brief Update stall detection with current reading
     *
     * @param current_mA Current motor draw in milliamps
     * @return Detection result (RUNNING if no conclusion yet)
     */
    Result update(uint16_t current_mA);

    /** @brief Peak current observed during startup ignore period */
    uint16_t peakStartupCurrent_mA() const { return _peakStartup_mA; }

    /** @brief Whether detection is active */
    bool isActive() const { return _active; }

private:
    Config _config;
    uint32_t _startTime_ms = 0;
    uint16_t _peakStartup_mA = 0;
    bool _startupChecked = false;
    bool _stallConfirming = false;
    uint32_t _stallConfirmStart_ms = 0;
    bool _active = false;
};

#endif // STALL_DETECTOR_H
