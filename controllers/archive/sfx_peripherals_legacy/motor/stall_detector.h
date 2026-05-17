/*
 * StallDetector — motor-stall state machine, promoted from
 *                 controllers/gearcontrol/pico/src/stall_detector.h
 *                 (2026-05-06 — see motor/README.md).
 *
 * Pure state machine — no GPIO, no driver dependency.  Caller feeds
 * it current readings via `update(uint16_t current_mA)` each loop
 * tick; the state machine returns a `Result` enum describing what
 * the current looks like:
 *
 *   Phases:
 *     1. Startup-ignore window — track peak current, ignore stall
 *     2. Motor-presence check — if peak < threshold, motor is absent
 *     3. Stall confirmation — current must stay above threshold for
 *        `stallConfirm_ms` to count (rejects transient drag spikes)
 *     4. Absolute timeout — accept partial stall or report fault
 *
 * Generic over any motor + current sensor pair.  PwmCollection's
 * stall guard delegates to one of these per channel.
 *
 * Originally extracted from `LandingGear` for runtime stall handling
 * during gear deploy/retract sequences.  Production-tuned with
 * tested defaults (500 ms inrush ignore, 200 ms confirm, 10 s
 * timeout).
 */

#ifndef SFX_STALL_DETECTOR_H
#define SFX_STALL_DETECTOR_H

#include <Arduino.h>
#include <cstdint>

namespace sfx_peripherals {

class StallDetector {
public:
    enum class Result : uint8_t {
        RUNNING,         ///< Motor running normally, no stall detected
        STALL_CONFIRMED, ///< Stall current sustained above threshold for required duration
        NO_MOTOR,        ///< Motor not detected (startup current below detection threshold)
        TIMEOUT_STALL,   ///< Timed out during stall confirmation — accepted as stall
        TIMEOUT_ERROR    ///< Timed out with no stall indication — fault condition
    };

    struct Config {
        uint16_t startupIgnore_ms        = 500;   ///< Ignore stall during motor startup
        uint16_t motorDetectThreshold_mA = 20;    ///< Min peak current to consider motor present
        uint16_t stallThreshold_mA       = 500;   ///< Current above this = potential stall
        uint16_t stallConfirm_ms         = 200;   ///< Must stay above threshold this long
        uint16_t timeout_ms              = 10000; ///< Max total motor run time
    };

    StallDetector() = default;

    void configure(const Config& config) { _config = config; }

    /// Reset all internal state and start tracking.  Call when the
    /// motor begins moving.  After this, feed `update(current_mA)`
    /// from `loop()` until it returns a non-RUNNING result.
    void start() {
        _startTime_ms          = millis();
        _peakStartup_mA        = 0;
        _startupChecked        = false;
        _stallConfirming       = false;
        _stallConfirmStart_ms  = 0;
        _active                = true;
    }

    /// Stop tracking immediately — no result is reported.  Use when
    /// the master cancels a motor command before stall is reached.
    void stop() { _active = false; }

    /// Tick the state machine with the latest current reading.
    /// Returns RUNNING until a terminal state is reached, then the
    /// detector becomes inactive and subsequent `update()` calls
    /// return RUNNING again until the next `start()`.
    Result update(uint16_t current_mA) {
        if (!_active) return Result::RUNNING;
        const uint32_t now     = millis();
        const uint32_t elapsed = now - _startTime_ms;

        // Phase 1 — Startup-ignore window: track peak for motor detection.
        if (elapsed <= _config.startupIgnore_ms) {
            if (current_mA > _peakStartup_mA) _peakStartup_mA = current_mA;
            return Result::RUNNING;
        }

        // Phase 2 — Motor-presence check (once after startup).
        if (!_startupChecked) {
            _startupChecked = true;
            if (_peakStartup_mA < _config.motorDetectThreshold_mA) {
                _active = false;
                return Result::NO_MOTOR;
            }
        }

        // Phase 3 — Stall confirmation: must stay above threshold.
        const bool aboveThreshold = (current_mA > _config.stallThreshold_mA);
        if (aboveThreshold) {
            if (!_stallConfirming) {
                _stallConfirming      = true;
                _stallConfirmStart_ms = now;
            } else if (now - _stallConfirmStart_ms >= _config.stallConfirm_ms) {
                _active = false;
                return Result::STALL_CONFIRMED;
            }
        } else {
            _stallConfirming = false;
        }

        // Phase 4 — Timeout handling.
        if (elapsed > _config.timeout_ms) {
            _active = false;
            return _stallConfirming ? Result::TIMEOUT_STALL
                                    : Result::TIMEOUT_ERROR;
        }
        return Result::RUNNING;
    }

    /// Peak current observed during the startup-ignore window.  Useful
    /// for diagnostics and for the master to confirm "motor is moving"
    /// before declaring an early no-motor result.
    uint16_t peakStartupCurrent_mA() const { return _peakStartup_mA; }

    /// Time the most-recent stall confirmation was started — 0 if no
    /// confirmation is in progress.  Lets callers compute over-
    /// threshold duration without restarting the detector.
    uint32_t stallConfirmStart_ms() const { return _stallConfirmStart_ms; }

    bool isActive() const { return _active; }

    const Config& config() const { return _config; }

private:
    Config   _config;
    uint32_t _startTime_ms          = 0;
    uint16_t _peakStartup_mA        = 0;
    bool     _startupChecked        = false;
    bool     _stallConfirming       = false;
    uint32_t _stallConfirmStart_ms  = 0;
    bool     _active                = false;
};

}  // namespace sfx_peripherals

#endif  // SFX_STALL_DETECTOR_H
