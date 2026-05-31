/*
 * gear.ipp — per-gear state-machine method definitions.
 */

#ifndef HUBFX_GEAR_IPP
#define HUBFX_GEAR_IPP

#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw SFX_MILLIS()

namespace hubfx::effects::gearctrl {

inline void Gear::deploy() {
    switch (_state) {
        case GearPhase::Unconfigured:
            SFX_LOG_WARN("[gear] deploy %u — not configured", _def.id);
            return;
        case GearPhase::Deploying:
        case GearPhase::Deployed:
            return;  // idempotent
        case GearPhase::Error:
            SFX_LOG_WARN("[gear] deploy %u rejected — gear in ERROR (GEAR_RESET first)", _def.id);
            return;
        case GearPhase::Calibrating:
            SFX_LOG_WARN("[gear] deploy %u rejected — calibration in progress", _def.id);
            return;
        default:
            break;
    }
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandSeek(_def.deployDuty);              // expander seeks endstop + auto-brakes
    enterPhase(GearPhase::Deploying);
    armBackstop(sfx_core::EffectClock::instance().nowMs());
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::retract() {
    switch (_state) {
        case GearPhase::Unconfigured:
            SFX_LOG_WARN("[gear] retract %u — not configured", _def.id);
            return;
        case GearPhase::Retracting:
        case GearPhase::Retracted:
            return;
        case GearPhase::Error:
            SFX_LOG_WARN("[gear] retract %u rejected — gear in ERROR (GEAR_RESET first)", _def.id);
            return;
        case GearPhase::Calibrating:
            SFX_LOG_WARN("[gear] retract %u rejected — calibration in progress", _def.id);
            return;
        default:
            break;
    }
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandSeek(_def.retractDuty);
    enterPhase(GearPhase::Retracting);
    armBackstop(sfx_core::EffectClock::instance().nowMs());
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::stop() {
    if (_state == GearPhase::Unconfigured) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _calibStep = CalibStep::None;
    // STOP from any moving / calibrating state returns the gear to a
    // known-safe baseline (Retracted), or holds Deployed.  ERROR is
    // NOT cleared by STOP — use GEAR_RESET (clearError) for that.
    enterPhase((_state == GearPhase::Deployed) ? GearPhase::Deployed
                                               : GearPhase::Retracted);
    _movingDeadlineMs = 0;
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::clearError() {
    if (_state != GearPhase::Error) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _calibStep = CalibStep::None;
    enterPhase(GearPhase::Retracted);
    _movingDeadlineMs = 0;
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: error cleared → retracted", _def.id);
}

inline void Gear::calibrate() {
    if (_state == GearPhase::Unconfigured) {
        SFX_LOG_WARN("[gear] calibrate %u — not configured", _def.id);
        return;
    }
    // First leg: seek the retract hard stop.
    if (_begin && _sendCtx) _begin(_sendCtx);
    _calibStep = CalibStep::RetractHome;
    commandSeek(_def.retractDuty);
    enterPhase(GearPhase::Calibrating);
    armBackstop(sfx_core::EffectClock::instance().nowMs());
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: calibration started (retract→deploy→home)", _def.id);
}

inline void Gear::calibrateCancel() {
    if (_state != GearPhase::Calibrating) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _calibStep = CalibStep::None;
    enterPhase(GearPhase::Retracted);
    _movingDeadlineMs = 0;
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: calibration cancelled", _def.id);
}

inline void Gear::update(uint32_t nowMs) {
    // Backstop only.  The expander's seek timeout is authoritative and
    // arrives as an ENDSTOP_RESULT(timeout); this fires solely if the
    // expander goes silent (no result by the seek timeout + margin).
    const bool moving = (_state == GearPhase::Deploying ||
                         _state == GearPhase::Retracting ||
                         _state == GearPhase::Calibrating);
    if (!moving) return;
    if (_movingDeadlineMs == 0) return;                  // no-timeout seek
    if ((int32_t)(nowMs - _movingDeadlineMs) < 0) return;

    SFX_LOG_WARN("[gear] %u: expander silent past seek timeout — ERROR", _def.id);
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _calibStep = CalibStep::None;
    enterPhase(GearPhase::Error);
    _movingDeadlineMs = 0;
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::onEndstopResult(uint8_t outcome) {
    // Aborted results follow our own brake() (stop/clearError/cancel) —
    // the phase is already set; nothing to do.
    if (outcome == BiMotorSeekOutcome::Aborted) return;

    const bool timedOut = (outcome == BiMotorSeekOutcome::Timeout);

    // ── Calibration sweep ─────────────────────────────────────────────
    if (_state == GearPhase::Calibrating) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        if (timedOut) {
            SFX_LOG_WARN("[gear] %u: calib leg %u found no endstop — ERROR",
                         _def.id, (unsigned)_calibStep);
            _calibStep = CalibStep::None;
            _movingDeadlineMs = 0;
            enterPhase(GearPhase::Error);
        } else switch (_calibStep) {        // outcome == Reached
            case CalibStep::RetractHome:
                _calibStep = CalibStep::DeployEnd;
                commandSeek(_def.deployDuty);
                armBackstop(sfx_core::EffectClock::instance().nowMs());
                SFX_LOG_DEBUG("[gear] %u calib: retract stop OK → deploy sweep", _def.id);
                break;
            case CalibStep::DeployEnd:
                _calibStep = CalibStep::RetractFinal;
                commandSeek(_def.retractDuty);
                armBackstop(sfx_core::EffectClock::instance().nowMs());
                SFX_LOG_DEBUG("[gear] %u calib: deploy stop OK → home", _def.id);
                break;
            case CalibStep::RetractFinal:
            default:
                _calibStep = CalibStep::None;
                _movingDeadlineMs = 0;
                enterPhase(GearPhase::Retracted);
                SFX_LOG_INFO("[gear] %u: calibration complete", _def.id);
                break;
        }
        if (_commit && _sendCtx) _commit(_sendCtx);
        return;
    }

    // ── Normal travel ─────────────────────────────────────────────────
    if (_state == GearPhase::Deploying || _state == GearPhase::Retracting) {
        const uint8_t target = timedOut ? GearPhase::Error
                             : (_state == GearPhase::Deploying ? GearPhase::Deployed
                                                               : GearPhase::Retracted);
        if (timedOut)
            SFX_LOG_WARN("[gear] %u: seek timed out — ERROR", _def.id);
        if (_begin && _sendCtx) _begin(_sendCtx);
        // Expander already braked locally on reach/timeout — no brake here.
        enterPhase(target);
        _movingDeadlineMs = 0;
        if (_commit && _sendCtx) _commit(_sendCtx);
    }
    // else: stray result while idle / error — ignore.
}

inline void Gear::enterPhase(uint8_t newPhase) {
    _state = newPhase;
    SFX_LOG_DEBUG("[gear] %u → %s", _def.id, GearPhase::getName(newPhase));
    if (_phase) _phase(_phaseCtx, _def.id, newPhase);
}

inline void Gear::commandSeek(int16_t signedDuty) {
    if (!_send) return;
    uint8_t payload[5];
    payload[0] = _def.motor.portIdx;
    SfxWire::putU16LE(&payload[1], static_cast<uint16_t>(signedDuty));
    SfxWire::putU16LE(&payload[3], (uint16_t)_def.timeoutMs);   // 0 = no timeout
    _send(_sendCtx, _def.motor,
          RolePacket::BIMOTOR_SEEK_ENDSTOP, payload, sizeof(payload));
}

inline void Gear::armBackstop(uint32_t nowMs) {
    // Hub-side silent-expander guard: a bit beyond the seek's own
    // timeout.  0 timeout ⇒ no backstop (seek runs until stall/abort).
    _movingDeadlineMs = (_def.timeoutMs != 0) ? (nowMs + _def.timeoutMs + 1000u) : 0;
}

inline void Gear::commandMotorBrake() {
    if (!_send) return;
    const uint8_t payload[1] = { _def.motor.portIdx };
    _send(_sendCtx, _def.motor,
          RolePacket::BIMOTOR_BRAKE, payload, sizeof(payload));
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEAR_IPP
