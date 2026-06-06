/*
 * gear.ipp — per-gear op-queue + door-bracket state-machine bodies.
 *
 *   deploy  : OPEN_DOORS → [SYNC] → RUN_MOTOR(deploy) → [SYNC] → CLOSE_DOORS(policy)
 *   retract : OPEN_DOORS → [SYNC] → RUN_MOTOR(retract) → [SYNC] → CLOSE_DOORS(all)
 *
 *   Door legs wait on SERVO_MOTION_DONE (monitored, decision #1); the
 *   motor leg waits on BIMOTOR_ENDSTOP_RESULT.  SYNC barriers park the
 *   gear for the multi-gear coordinator (Rule 40 — EffectClock only).
 */

#ifndef HUBFX_GEAR_IPP
#define HUBFX_GEAR_IPP

#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw millis()

namespace hubfx::effects::gearctrl {

// ── Public entry points ──────────────────────────────────────────────

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
        default:
            break;
    }
    startCycle(/*deploying=*/true);
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
        default:
            break;
    }
    startCycle(/*deploying=*/false);
}

inline void Gear::stop() {
    if (_state == GearPhase::Unconfigured) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _doorSeq.reset();
    _leg            = Leg::None;
    _atDoorsBarrier = false;
    _atMotorBarrier = false;
    _movingDeadlineMs = 0;
    // STOP from any moving state returns the gear to a known-safe baseline
    // (Retracted), or holds Deployed.  ERROR is NOT cleared by STOP.
    setPhaseSub((_state == GearPhase::Deployed) ? GearPhase::Deployed
                                                : GearPhase::Retracted,
                GearSubPhase::idle);
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::clearError() {
    if (_state != GearPhase::Error) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _doorSeq.reset();
    _leg            = Leg::None;
    _atDoorsBarrier = false;
    _atMotorBarrier = false;
    _movingDeadlineMs = 0;
    setPhaseSub(GearPhase::Retracted, GearSubPhase::idle);
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: error cleared → retracted", _def.id);
}

// ── Op-queue ─────────────────────────────────────────────────────────

inline void Gear::startCycle(bool deploying) {
    _deploying      = deploying;
    _atDoorsBarrier = false;
    _atMotorBarrier = false;
    _movingDeadlineMs = 0;
    if (_begin && _sendCtx) _begin(_sendCtx);
    setPhaseSub(deploying ? GearPhase::Deploying : GearPhase::Retracting,
                GearSubPhase::idle);
    beginLeg(Leg::OpenDoors);
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::beginLeg(Leg leg) {
    _leg = leg;
    switch (leg) {
        case Leg::OpenDoors:
            _doorSeq.open();
            setPhaseSub(_state, GearSubPhase::doors_opening);
            if (_doorSeq.isComplete()) { advanceLeg(); return; }   // NONE / no doors
            break;

        case Leg::SyncDoors:
            // Barrier after doors-open.  If this gear doesn't sync at the
            // door boundary, skip straight through.
            setPhaseSub(_state, GearSubPhase::doors_open);
            if (!_syncDoors) { advanceLeg(); return; }
            _atDoorsBarrier = true;        // coordinator releases via advanceBarrier()
            break;

        case Leg::RunMotor:
            setPhaseSub(_state, GearSubPhase::motor_running);
            commandSeek(_deploying ? _def.deployDuty : _def.retractDuty);
            armBackstop(sfx_core::EffectClock::instance().nowMs());
            break;

        case Leg::SyncMotor:
            setPhaseSub(_state, GearSubPhase::motor_done);
            if (!_syncMotor) { advanceLeg(); return; }
            _atMotorBarrier = true;
            break;

        case Leg::CloseDoors:
            setPhaseSub(_state, GearSubPhase::doors_closing);
            // Deploy applies the close policy; retract always re-stows.
            if (_deploying) _doorSeq.close();
            else            _doorSeq.closeFull();
            if (_doorSeq.isComplete()) { advanceLeg(); return; }
            break;

        case Leg::Done:
            finishCycle();
            break;

        case Leg::None:
        default:
            break;
    }
}

inline void Gear::advanceLeg() {
    switch (_leg) {
        case Leg::OpenDoors:  beginLeg(Leg::SyncDoors);  break;
        case Leg::SyncDoors:  _atDoorsBarrier = false; beginLeg(Leg::RunMotor); break;
        case Leg::RunMotor:   beginLeg(Leg::SyncMotor);  break;
        case Leg::SyncMotor:  _atMotorBarrier = false; beginLeg(Leg::CloseDoors); break;
        case Leg::CloseDoors: beginLeg(Leg::Done);       break;
        default:              finishCycle();             break;
    }
}

inline void Gear::finishCycle() {
    _leg              = Leg::None;
    _movingDeadlineMs = 0;
    _atDoorsBarrier   = false;
    _atMotorBarrier   = false;
    setPhaseSub(_deploying ? GearPhase::Deployed : GearPhase::Retracted,
                GearSubPhase::idle);
    SFX_LOG_INFO("[gear] %u: cycle complete → %s", _def.id,
                 GearPhase::getName(_state));
}

inline void Gear::enterError() {
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _doorSeq.reset();
    _leg              = Leg::None;
    _movingDeadlineMs = 0;
    _atDoorsBarrier   = false;
    _atMotorBarrier   = false;
    setPhaseSub(GearPhase::Error, GearSubPhase::idle);
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::advanceBarrier() {
    if (_atDoorsBarrier && _sub == GearSubPhase::doors_open) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        advanceLeg();
        if (_commit && _sendCtx) _commit(_sendCtx);
    } else if (_atMotorBarrier && _sub == GearSubPhase::motor_done) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        advanceLeg();
        if (_commit && _sendCtx) _commit(_sendCtx);
    }
}

// ── Event ingress ────────────────────────────────────────────────────

inline void Gear::onServoMotionDone(uint8_t portIdx) {
    if (_leg != Leg::OpenDoors && _leg != Leg::CloseDoors) return;
    _doorSeq.onServoMotionDone(portIdx);
    if (_doorSeq.isComplete()) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        advanceLeg();
        if (_commit && _sendCtx) _commit(_sendCtx);
    }
}

inline void Gear::onEndstopResult(uint8_t outcome) {
    // Aborted results follow our own brake() (stop/clearError) — the phase
    // is already set; nothing to do.
    if (outcome == BiMotorSeekOutcome::Aborted) return;
    if (_leg != Leg::RunMotor) return;   // stray result while not on the motor leg

    if (outcome == BiMotorSeekOutcome::Timeout) {
        SFX_LOG_WARN("[gear] %u: seek timed out — ERROR", _def.id);
        enterError();
        return;
    }
    // Reached — expander already braked locally; advance to the next leg.
    if (_begin && _sendCtx) _begin(_sendCtx);
    advanceLeg();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

// ── Tick ─────────────────────────────────────────────────────────────

inline void Gear::update(uint32_t nowMs) {
    if (!isCycling()) return;

    // Door legs: drive the DUAL_DELAY timer; completion arrives via
    // onServoMotionDone(), but a timer-dispatched second door + an
    // already-settled set must still settle here.
    if (_leg == Leg::OpenDoors || _leg == Leg::CloseDoors) {
        _doorSeq.update();
        if (_doorSeq.isComplete()) {
            if (_begin && _sendCtx) _begin(_sendCtx);
            advanceLeg();
            if (_commit && _sendCtx) _commit(_sendCtx);
        }
        return;
    }

    // Motor leg: silent-expander backstop only (the expander's seek timeout
    // is authoritative and arrives as ENDSTOP_RESULT(timeout)).
    if (_leg == Leg::RunMotor) {
        if (_movingDeadlineMs == 0) return;            // no-timeout seek
        if ((int32_t)(nowMs - _movingDeadlineMs) < 0) return;
        SFX_LOG_WARN("[gear] %u: expander silent past seek timeout — ERROR", _def.id);
        enterError();
    }
}

// ── Helpers ──────────────────────────────────────────────────────────

inline void Gear::setPhaseSub(uint8_t newPhase, uint8_t newSub) {
    const bool changed = (_state != newPhase) || (_sub != newSub);
    _state = newPhase;
    _sub   = newSub;
    if (changed) {
        SFX_LOG_DEBUG("[gear] %u → %s / %s", _def.id,
                      GearPhase::getName(newPhase), GearSubPhase::getName(newSub));
        if (_phase) _phase(_phaseCtx, _def.id, newPhase, newSub);
    }
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
    // Hub-side silent-expander guard: a bit beyond the seek's own timeout.
    // 0 timeout ⇒ no backstop (seek runs until stall/abort).
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
