/*
 * BiDcMotorRole implementation — see header for the Fixed vs LiveRatio
 * guard-mode contract and the Strategy A position semantics.
 */

#include "bi_dc_motor_role.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#include <serial/roles.h>     // RolePacket::BiMotorSeekOutcome

namespace sfx_core {

void BiDcMotorRole::setSigned(int16_t signedDuty) {
    abortSeek();                       // explicit drive cancels any seek
    _commandedSigned = signedDuty;
    if (_port) _port->setSigned(signedDuty);
    if (signedDuty == 0) {
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
}

void BiDcMotorRole::brake() {
    abortSeek();                       // brake cancels any seek
    _commandedSigned = 0;
    if (_port) _port->brake();
    clearStall();
}

void BiDcMotorRole::coast() {
    abortSeek();                       // coast cancels any seek
    _commandedSigned = 0;
    if (_port) _port->coast();
    clearStall();
}

void BiDcMotorRole::setStallGuard(uint16_t threshold_mA, uint16_t window_ms) {
    _guardMode          = GuardMode::Fixed;
    _stallThreshold_mA  = threshold_mA;
    if (window_ms != 0) _stallWindow_ms = window_ms;
    _overcurrentStartMs = 0;
}

void BiDcMotorRole::setStallGuardRatio(uint16_t ratio_x100,
                                       uint16_t runSample_ms,
                                       uint16_t inrushBlank_ms,
                                       uint16_t maxTravel_ms) {
    _guardMode      = GuardMode::LiveRatio;
    _ratio_x100     = (ratio_x100   != 0) ? ratio_x100   : 250;   // 2.5× default
    _runSample_ms   = (runSample_ms != 0) ? runSample_ms : 200;
    _inrushBlank_ms = (inrushBlank_ms != 0) ? inrushBlank_ms : 150;
    _maxTravel_ms   = maxTravel_ms;                               // 0 = no failsafe
    // Reuse Fixed's sustained-window; if caller never touched it, use a
    // tighter default appropriate for ratio-tripping noise (80 ms).
    if (_stallWindow_ms == 250 /* Fixed default */) _stallWindow_ms = 80;
    _overcurrentStartMs = 0;
}

void BiDcMotorRole::setStallWindowMs(uint16_t window_ms) {
    if (window_ms != 0) _stallWindow_ms = window_ms;
}

void BiDcMotorRole::clearStall() {
    _stalled             = false;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
    // Clearing the stall also clears a latched seek-timeout fault.
    if (_seekState == SeekState::TimedOut || _seekState == SeekState::Reached) {
        _seekState = SeekState::Idle;
    }
}

void BiDcMotorRole::seekEndstop(int16_t signedDuty, uint16_t timeout_ms) {
    moveToEnd(Position::Unknown, signedDuty, timeout_ms);
}

void BiDcMotorRole::moveToEnd(Position targetEnd, int16_t signedDuty, uint16_t timeout_ms) {
    // Restore-without-moving: silently record the position and report
    // Reached so the caller's flow stays uniform (no special-case in
    // Strategy A "first-command probe" callers).
    if (signedDuty == 0) {
        if (targetEnd != Position::Unknown) _position = targetEnd;
        _seekState = SeekState::Reached;
        if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Reached, 0, 0, _position);
        return;
    }

    const uint32_t now   = SFX_MILLIS();
    _seekState      = SeekState::Seeking;
    _seekDuty       = signedDuty;
    _seekStartMs    = now;
    _targetEnd      = targetEnd;

    // Effective deadline = min(per-seek timeout, LiveRatio maxTravel).
    uint32_t deadline = 0;
    if (timeout_ms != 0)      deadline = now + timeout_ms;
    if (_guardMode == GuardMode::LiveRatio && _maxTravel_ms != 0) {
        const uint32_t maxDeadline = now + _maxTravel_ms;
        if (deadline == 0 || maxDeadline < deadline) deadline = maxDeadline;
    }
    _seekDeadlineMs = deadline;

    // Per-stroke state.
    _stalled             = false;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
    _runAccum_mA         = 0;
    _runCount            = 0;
    _runMean_mA          = 0;

    _commandedSigned     = signedDuty;
    if (_port) _port->setSigned(signedDuty);   // drive directly (not via setSigned → no abort)
}

void BiDcMotorRole::abortSeek() {
    if (_seekState == SeekState::Seeking) {
        _seekState = SeekState::Idle;
        if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Aborted,
                                   (uint16_t)(SFX_MILLIS() - _seekStartMs),
                                   0, _position);
    }
}

bool BiDcMotorRole::stepStallDetect(uint32_t now, uint16_t threshold_mA) {
    if (threshold_mA == 0) return false;
    const uint16_t i_mag = (uint16_t)std::abs((int)_iSense->current_mA());
    if (i_mag >= threshold_mA) {
        if (_overcurrentStartMs == 0) {
            _overcurrentStartMs  = now;
            _peakDuringWindow_mA = i_mag;
        } else {
            if (i_mag > _peakDuringWindow_mA) _peakDuringWindow_mA = i_mag;
            if (now - _overcurrentStartMs >= _stallWindow_ms) {
                // Confirmed stall = endstop.
                if (_port) _port->brake();
                _commandedSigned = 0;
                _stalled         = true;
                _seekState       = SeekState::Reached;
                if (_targetEnd != Position::Unknown) _position = _targetEnd;
                if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Reached,
                                           (uint16_t)(now - _seekStartMs),
                                           _peakDuringWindow_mA, _position);
                return true;
            }
        }
    } else {
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
    return false;
}

void BiDcMotorRole::tick() {
    const uint32_t now = SFX_MILLIS();

    // ── Endstop seek ──────────────────────────────────────────────────
    if (_seekState == SeekState::Seeking) {
        if (_iSense) {
            if (_guardMode == GuardMode::LiveRatio) {
                const uint32_t elapsed = now - _seekStartMs;

                // Phase 1: inrush blanking — accumulate nothing, detect
                // nothing (the start-from-rest current spike looks
                // identical to a true stall).
                if (elapsed < _inrushBlank_ms) {
                    // fall through to deadline check
                } else if (elapsed < (uint32_t)_inrushBlank_ms + _runSample_ms) {
                    // Phase 2: sample baseline running current for THIS stroke.
                    _runAccum_mA += (uint32_t)std::abs((int)_iSense->current_mA());
                    _runCount    += 1;
                } else {
                    // Phase 3: detect.  Compute baseline once on first entry.
                    if (_runMean_mA == 0) {
                        _runMean_mA = (_runCount > 0)
                                          ? (uint16_t)(_runAccum_mA / _runCount)
                                          : 0;
                        // Noise floor so a tiny baseline doesn't yield a
                        // tiny threshold that trips on brush noise.
                        if (_runMean_mA < 50) _runMean_mA = 50;
                    }
                    const uint16_t thr =
                        (uint16_t)((uint32_t)_runMean_mA * _ratio_x100 / 100);
                    if (stepStallDetect(now, thr)) return;
                }
            } else if (_stallThreshold_mA != 0) {
                // Fixed mode — original behaviour.
                if (stepStallDetect(now, _stallThreshold_mA)) return;
            }
        }
        // Optional timeout (deadline 0 = none) → brake + latched fault.
        if (_seekDeadlineMs != 0 && (int32_t)(now - _seekDeadlineMs) >= 0) {
            if (_port) _port->brake();
            _commandedSigned = 0;
            _seekState       = SeekState::TimedOut;
            // Position becomes Unknown on timeout — we lost track of
            // where the motor actually is (the move didn't complete).
            _position        = Position::Unknown;
            if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Timeout,
                                       (uint16_t)(now - _seekStartMs),
                                       0, _position);
        }
        return;
    }

    // ── Free-running stall guard (non-seek drive) ──────────────────────
    // LiveRatio is meaningless without a baseline window — Fixed only here.
    if (!_iSense || _guardMode != GuardMode::Fixed
        || _stallThreshold_mA == 0 || _stalled) return;
    const uint16_t i_mag = (uint16_t)std::abs((int)_iSense->current_mA());
    if (i_mag >= _stallThreshold_mA) {
        if (_overcurrentStartMs == 0) {
            _overcurrentStartMs  = now;
            _peakDuringWindow_mA = i_mag;
        } else {
            if (i_mag > _peakDuringWindow_mA) _peakDuringWindow_mA = i_mag;
            if (now - _overcurrentStartMs >= _stallWindow_ms) {
                _stalled = true;
                if (_onStall) _onStall(_peakDuringWindow_mA, _stallWindow_ms);
            }
        }
    } else {
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
}

}  // namespace sfx_core
