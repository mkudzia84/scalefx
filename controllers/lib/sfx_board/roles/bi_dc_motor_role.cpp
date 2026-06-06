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

void BiDcMotorRole::setProbe(uint8_t probePct, uint16_t windowMs, uint8_t dropPct) {
    _probePct      = probePct;                              // 0 = disabled
    _probeWindowMs = (windowMs != 0) ? windowMs : 250;
    _probeDropPct  = (dropPct  != 0) ? dropPct  : 70;
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

    _lastSeekLogMs       = now;
    _fullSeekDuty        = signedDuty;

    // Dual-stage soft-start: when a probe is configured AND we can sense
    // current, drive a low-power probe FIRST to classify free-vs-already-at-
    // stop before committing full power.  Otherwise go straight to full power.
    if (_probePct > 0 && _iSense) {
        _seekState     = SeekState::Probing;
        _probePeak_mA  = 0;
        int16_t probeDuty = (int16_t)((int32_t)signedDuty * _probePct / 100);
        if (probeDuty == 0) probeDuty = (signedDuty > 0) ? 1 : -1;   // ensure motion attempt
        _commandedSigned = probeDuty;
        if (_port) _port->setSigned(probeDuty);
        SFX_LOG_INFO("[bimotor] PROBE start: end=%u probeDuty=%d (%u%% of %d) window=%ums drop<%u%%  — classify free vs already-at-stop  (V=%dmV)",
                     (unsigned)targetEnd, (int)probeDuty, (unsigned)_probePct, (int)signedDuty,
                     (unsigned)_probeWindowMs, (unsigned)_probeDropPct, (int)voltage_mV());
        return;
    }

    _seekState       = SeekState::Seeking;
    _commandedSigned = signedDuty;
    if (_port) _port->setSigned(signedDuty);   // drive directly (not via setSigned → no abort)

    SFX_LOG_INFO("[bimotor] SEEK start: end=%u duty=%d timeout=%ums  guard=%s thr=%umA win=%ums%s  (I now=%dmA V=%dmV)",
                 (unsigned)targetEnd, (int)signedDuty, (unsigned)timeout_ms,
                 _guardMode == GuardMode::LiveRatio ? "live-ratio" : "fixed",
                 (unsigned)_stallThreshold_mA, (unsigned)_stallWindow_ms,
                 _iSense ? "" : "  [!! NO CURRENT SENSOR — can only time out]",
                 (int)current_mA(), (int)voltage_mV());
}

// Probing → Seeking: commit full power and reset the per-stroke stall state so
// LiveRatio re-baselines the running current at FULL power (the probe baseline
// was at low power and would mis-scale the trip threshold).  Keeps the original
// _seekDeadlineMs (absolute) so the overall timeout budget is unchanged.
void BiDcMotorRole::beginRunPhase(uint32_t now) {
    _seekState           = SeekState::Seeking;
    _seekStartMs         = now;       // fresh — LiveRatio inrush/baseline from full-power start
    _lastSeekLogMs       = now;
    _runAccum_mA         = 0;
    _runCount            = 0;
    _runMean_mA          = 0;
    _overcurrentStartMs  = 0;
    _peakDuringWindow_mA = 0;
    _commandedSigned     = _fullSeekDuty;
    if (_port) _port->setSigned(_fullSeekDuty);
    SFX_LOG_INFO("[bimotor] SEEK start (post-probe): duty=%d  guard=%s thr=%umA win=%ums",
                 (int)_fullSeekDuty, _guardMode == GuardMode::LiveRatio ? "live-ratio" : "fixed",
                 (unsigned)_stallThreshold_mA, (unsigned)_stallWindow_ms);
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

    // ── Soft-start probe (classify free vs already-at-stop) ────────────
    if (_seekState == SeekState::Probing) {
        const uint32_t elapsed = now - _seekStartMs;
        const uint16_t i = _iSense ? (uint16_t)std::abs((int)_iSense->current_mA()) : 0;
        if (i > _probePeak_mA) _probePeak_mA = i;

        if (now - _lastSeekLogMs >= 150) {
            _lastSeekLogMs = now;
            SFX_LOG_INFO("[bimotor] probing t=%ums I=%umA peak=%umA V=%dmV",
                         (unsigned)elapsed, (unsigned)i, (unsigned)_probePeak_mA, (int)voltage_mV());
        }

        // Let the probe current rise first, then look for the back-EMF decay
        // that means the rotor spun up (= mechanism free to move).
        constexpr uint16_t kProbeSettleMs   = 60;
        constexpr uint16_t kProbeNoiseFloor = 30;   // mA — ignore tiny baselines
        if (elapsed >= kProbeSettleMs && _probePeak_mA >= kProbeNoiseFloor) {
            const uint16_t dropThr = (uint16_t)((uint32_t)_probePeak_mA * _probeDropPct / 100);
            if (i < dropThr) {
                SFX_LOG_INFO("[bimotor] PROBE → FREE: I=%umA fell below %u%% of peak %umA (spun up) → applying full power",
                             (unsigned)i, (unsigned)_probeDropPct, (unsigned)_probePeak_mA);
                beginRunPhase(now);
                return;
            }
        }
        // No decay through the window → locked rotor = already hard at this end.
        if (elapsed >= _probeWindowMs) {
            if (_port) _port->brake();
            _commandedSigned = 0;
            _stalled         = true;
            _seekState       = SeekState::Reached;
            if (_targetEnd != Position::Unknown) _position = _targetEnd;
            SFX_LOG_INFO("[bimotor] PROBE → AT-STOP: no decay (I=%umA held near peak %umA) → already at end %u; full power NOT applied",
                         (unsigned)i, (unsigned)_probePeak_mA, (unsigned)_position);
            if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Reached, (uint16_t)elapsed,
                                       _probePeak_mA, _position);
            return;
        }
        // Overall timeout still applies during the probe.
        if (_seekDeadlineMs != 0 && (int32_t)(now - _seekDeadlineMs) >= 0) {
            if (_port) _port->brake();
            _commandedSigned = 0;
            _seekState       = SeekState::TimedOut;
            _position        = Position::Unknown;
            SFX_LOG_INFO("[bimotor] TIMEOUT during probe after %ums", (unsigned)elapsed);
            if (_onEndstop) _onEndstop(BiMotorSeekOutcome::Timeout, (uint16_t)elapsed, 0, _position);
        }
        return;
    }

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
                                  : (elapsed < (uint32_t)_inrushBlank_ms + _runSample_ms) ? "baselining"
                                                                                          : "watching";
                const uint16_t thr = _runMean_mA
                    ? (uint16_t)((uint32_t)_runMean_mA * _ratio_x100 / 100) : 0;
                SFX_LOG_INFO("[bimotor] seeking t=%ums I=%dmA  thr=%umA (%s)  V=%dmV",
                             (unsigned)elapsed, curI, (unsigned)thr, phase, (int)voltage_mV());
            } else {
                SFX_LOG_INFO("[bimotor] seeking t=%ums I=%dmA  thr=%umA (fixed)  V=%dmV",
                             (unsigned)elapsed, curI, (unsigned)_stallThreshold_mA, (int)voltage_mV());
            }
        }
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
                        SFX_LOG_INFO("[bimotor] baseline running current = %umA (over %ums, n=%u) → trip at %umA (%u/100×)",
                                     (unsigned)_runMean_mA, (unsigned)_runSample_ms, (unsigned)_runCount,
                                     (unsigned)((uint32_t)_runMean_mA * _ratio_x100 / 100), (unsigned)_ratio_x100);
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
