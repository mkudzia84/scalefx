/*
 * BiDcMotorRole implementation.
 */

#include "bi_dc_motor_role.h"

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
    _stallThreshold_mA  = threshold_mA;
    _stallWindow_ms     = window_ms;
    _overcurrentStartMs = 0;
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
    const uint32_t now   = millis();
    _seekState      = SeekState::Seeking;
    _seekDuty       = signedDuty;
    _seekStartMs    = now;
    _seekDeadlineMs = (timeout_ms != 0) ? (now + timeout_ms) : 0;   // 0 = no timeout
    _stalled             = false;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
    _commandedSigned     = signedDuty;
    if (_port) _port->setSigned(signedDuty);   // drive directly (not via setSigned → no abort)
}

void BiDcMotorRole::abortSeek() {
    if (_seekState == SeekState::Seeking) {
        _seekState = SeekState::Idle;
        if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Aborted,
                                   (uint16_t)(millis() - _seekStartMs), 0);
    }
}

void BiDcMotorRole::tick() {
    const uint32_t now = millis();

    // ── Endstop seek ──────────────────────────────────────────────────
    if (_seekState == SeekState::Seeking) {
        // Stall = endstop reached → brake locally + report.
        if (_iSense && _stallThreshold_mA != 0) {
            const uint16_t i_mag = (uint16_t)std::abs((int)_iSense->current_mA());
            if (i_mag >= _stallThreshold_mA) {
                if (_overcurrentStartMs == 0) {
                    _overcurrentStartMs  = now;
                    _peakDuringWindow_mA = i_mag;
                } else {
                    if (i_mag > _peakDuringWindow_mA) _peakDuringWindow_mA = i_mag;
                    if (now - _overcurrentStartMs >= _stallWindow_ms) {
                        if (_port) _port->brake();
                        _commandedSigned = 0;
                        _stalled         = true;
                        _seekState       = SeekState::Reached;
                        if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Reached,
                                                   (uint16_t)(now - _seekStartMs),
                                                   _peakDuringWindow_mA);
                        return;
                    }
                }
            } else {
                _overcurrentStartMs  = 0;
                _peakDuringWindow_mA = 0;
            }
        }
        // Optional timeout (deadline 0 = none) → brake + latched fault.
        if (_seekDeadlineMs != 0 && (int32_t)(now - _seekDeadlineMs) >= 0) {
            if (_port) _port->brake();
            _commandedSigned = 0;
            _seekState       = SeekState::TimedOut;
            if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Timeout,
                                       (uint16_t)(now - _seekStartMs), 0);
        }
        return;
    }

    // ── Free-running stall guard (non-seek drive) ──────────────────────
    if (!_iSense || _stallThreshold_mA == 0 || _stalled) return;
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
