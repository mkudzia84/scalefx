/*
 * BiDcMotorRole implementation — see header for the Fixed vs LiveRatio
 * guard-mode contract and the Strategy A position semantics.
 */

#include "bi_dc_motor_role.h"
#include <platform/sfx_platform.h>   // SFX_MILLIS()

#include <serial/roles.h>     // RolePacket::BiMotorSeekOutcome
#include <serial/diag_log.h>  // SFX_LOG_INFO — verbose seek-stage trace to the console

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
    _runMin_mA           = 0xFFFF;
    _belowFloorStartMs   = 0;     // no-load (open-circuit) detector, this stroke

    _lastSeekLogMs       = now;
    _seekState           = SeekState::Seeking;
    _commandedSigned     = signedDuty;
    if (_port) _port->setSigned(signedDuty);   // drive directly (not via setSigned → no abort)

    SFX_LOG_INFO("[bimotor] SEEK start: end=%u duty=%d timeout=%ums  guard=%s thr=%umA win=%ums%s  (I now=%dmA V=%dmV)",
                 (unsigned)targetEnd, (int)signedDuty, (unsigned)timeout_ms,
                 _guardMode == GuardMode::LiveRatio ? "live-ratio" : "fixed",
                 (unsigned)_stallThreshold_mA, (unsigned)_stallWindow_ms,
                 _iSense ? "" : "  [!! NO CURRENT SENSOR — can only time out]",
                 (int)current_mA(), (int)voltage_mV());
}

void BiDcMotorRole::abortSeek() {
    if (_seekState == SeekState::Seeking) {
        _seekState = SeekState::Idle;
        SFX_LOG_INFO("[bimotor] seek ABORTED at %ums (explicit drive/brake/coast)",
                     (unsigned)(SFX_MILLIS() - _seekStartMs));
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
            SFX_LOG_INFO("[bimotor] over-threshold: I=%umA >= thr=%umA — confirming over %ums",
                         (unsigned)i_mag, (unsigned)threshold_mA, (unsigned)_stallWindow_ms);
        } else {
            if (i_mag > _peakDuringWindow_mA) _peakDuringWindow_mA = i_mag;
            if (now - _overcurrentStartMs >= _stallWindow_ms) {
                // Confirmed stall = endstop.
                if (_port) _port->brake();
                _commandedSigned = 0;
                _stalled         = true;
                _seekState       = SeekState::Reached;
                if (_targetEnd != Position::Unknown) _position = _targetEnd;
                SFX_LOG_INFO("[bimotor] STALL confirmed → endstop: peak=%umA over %ums  travel=%ums  pos=%u",
                             (unsigned)_peakDuringWindow_mA, (unsigned)_stallWindow_ms,
                             (unsigned)(now - _seekStartMs), (unsigned)_position);
                if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Reached,
                                           (uint16_t)(now - _seekStartMs),
                                           _peakDuringWindow_mA, _position);
                return true;
            }
        }
    } else {
        if (_overcurrentStartMs != 0) {
            SFX_LOG_INFO("[bimotor] over-threshold cleared (I=%umA < thr=%umA) — not a stall",
                         (unsigned)i_mag, (unsigned)threshold_mA);
        }
        _overcurrentStartMs  = 0;
        _peakDuringWindow_mA = 0;
    }
    return false;
}

void BiDcMotorRole::tick() {
    const uint32_t now = SFX_MILLIS();

    // ── Endstop seek ──────────────────────────────────────────────────
    if (_seekState == SeekState::Seeking) {
        // Throttled progress trace (~300 ms) so the operator sees the
        // current track vs. the live trip threshold while the motor runs.
        if (now - _lastSeekLogMs >= 300) {
            _lastSeekLogMs = now;
            const uint32_t elapsed = now - _seekStartMs;
            const int curI = _iSense ? _iSense->current_mA() : 0;
            if (_guardMode == GuardMode::LiveRatio) {
                const char* phase = (elapsed < _inrushBlank_ms)                       ? "inrush-blank"
                                  : (elapsed < (uint32_t)_inrushBlank_ms + _runSample_ms) ? "warmup"
                                                                                          : "watching";
                uint16_t thr = _runMean_mA
                    ? (uint16_t)((uint32_t)_runMean_mA * _ratio_x100 / 100) : 0;
                if (_absMax_mA && (thr == 0 || _absMax_mA < thr)) thr = _absMax_mA;
                const uint16_t floor = (_runMin_mA == 0xFFFF) ? 0 : _runMin_mA;
                SFX_LOG_INFO("[bimotor] seeking t=%ums I=%dmA  floor=%umA thr=%umA (%s)  V=%dmV",
                             (unsigned)elapsed, curI, (unsigned)floor, (unsigned)thr, phase, (int)voltage_mV());
            } else {
                SFX_LOG_INFO("[bimotor] seeking t=%ums I=%dmA  thr=%umA (fixed)  V=%dmV",
                             (unsigned)elapsed, curI, (unsigned)_stallThreshold_mA, (int)voltage_mV());
            }
        }
        if (_iSense) {
            // ── No-load / open-circuit detector ────────────────────────────
            // A driven, connected motor always draws well above a few tens of
            // mA; an open circuit (no motor, unplugged, blown H-bridge output)
            // reads ~0 mA the whole stroke.  After the inrush-blank window, if
            // |I| stays below the present-floor continuously for the confirm
            // window, conclude "no motor" — a DISTINCT fault from stall
            // (over-current) and timeout (ran the full time).  Skipped when the
            // floor is disabled (0).  Independent of the guard mode.
            if (_presentFloor_mA != 0) {
                const uint32_t elapsedNl = now - _seekStartMs;
                if (elapsedNl >= _inrushBlank_ms) {
                    const uint16_t i_mag_nl = (uint16_t)std::abs((int)_iSense->current_mA());
                    if (i_mag_nl < _presentFloor_mA) {
                        if (_belowFloorStartMs == 0) _belowFloorStartMs = now;
                        else if (now - _belowFloorStartMs >= kNoLoadConfirmMs) {
                            if (_port) _port->brake();
                            _commandedSigned = 0;
                            _seekState       = SeekState::TimedOut;   // latched fault
                            _position        = Position::Unknown;
                            SFX_LOG_INFO("[bimotor] NO-LOAD: I stayed <%umA for %ums under drive — "
                                         "no motor / open circuit / blown output (braked, fault latched)",
                                         (unsigned)_presentFloor_mA, (unsigned)kNoLoadConfirmMs);
                            if (_onEndstop) _onEndstop(BiMotorSeekOutcome::NoLoad,
                                                       (uint16_t)(now - _seekStartMs), 0, _position);
                            return;
                        }
                    } else {
                        _belowFloorStartMs = 0;   // current present — reset the window
                    }
                }
            }

            if (_guardMode == GuardMode::LiveRatio) {
                const uint32_t elapsed = now - _seekStartMs;
                const uint16_t i_mag   = (uint16_t)std::abs((int)_iSense->current_mA());

                // Phase 1: inrush blanking — ignore the start-from-rest spike.
                if (elapsed < _inrushBlank_ms) {
                    // fall through to deadline check
                } else {
                    // Track the TRAILING MINIMUM = the free-running floor for
                    // THIS stroke.  Unlike a fixed-window average, a high
                    // break-away / loaded start can't poison it: the floor only
                    // follows the current DOWN as the motor reaches free-run.
                    if (i_mag < _runMin_mA) _runMin_mA = i_mag;

                    // Warm up briefly so the floor settles before we arm.
                    if (elapsed >= (uint32_t)_inrushBlank_ms + _runSample_ms) {
                        uint16_t base = (_runMin_mA < 50) ? 50 : _runMin_mA;
                        // Adaptive ratio trip…
                        uint16_t thr = (uint16_t)((uint32_t)base * _ratio_x100 / 100);
                        // …OR the absolute ceiling (references' I-TRIP backstop):
                        // trip on min(ratioThr, ceiling) so a poisoned-high floor
                        // can't hide a real stall.
                        if (_absMax_mA && _absMax_mA < thr) thr = _absMax_mA;

                        if (_runMean_mA == 0) {
                            SFX_LOG_INFO("[bimotor] armed: floor=%umA (trailing min) ratio=%u/100× → ratio-thr=%umA%s%u; floor adapts down as it runs",
                                         (unsigned)base, (unsigned)_ratio_x100,
                                         (unsigned)((uint32_t)base * _ratio_x100 / 100),
                                         _absMax_mA ? "  ceiling=" : "  (no ceiling)=",
                                         (unsigned)_absMax_mA);
                        }
                        _runMean_mA = base;   // live floor for the trace
                        if (stepStallDetect(now, thr)) return;
                    }
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
            SFX_LOG_INFO("[bimotor] TIMEOUT after %ums — no stall detected, braked (fault latched). "
                         "Last I=%dmA; lower the ratio/threshold if the motor did reach the stop.",
                         (unsigned)(now - _seekStartMs), (int)current_mA());
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
