/*
 * gear.ipp — target-driven per-strut state machine.
 *
 *   Each strut owns a `_target` (Up/Down) and walks a SINGLE symmetric transit
 *   axis toward it:
 *
 *     settled ⇄ doors_opening ⇄ doors_open ⇄ strut_moving ⇄ strut_done
 *             ⇄ doors_closing ⇄ doors_closed ⇄ settled
 *
 *   `pump()` is the only engine: given the current `_sub` (transit position)
 *   and `_target`, it commands the next leg, parks (step mode), settles, or
 *   REVERSES.  Reversal is not a special path — it is `pump()` recomputing the
 *   next action when `_seekDir != _target` (the strut is committed to / heading
 *   the wrong endstop).  setTarget/stepToward/onServoMotionDone/onEndstopResult/
 *   update all funnel into `pump()`, wrapped in the wire batch.
 *
 *   Motion uses the BiDcMotor role's autonomous endstop seek: re-issuing a seek
 *   the other way OVERWRITES the in-flight one (no brake handshake, no stray
 *   `Aborted`), so a mid-seek reversal is just `startSeek()` again.  Door legs
 *   complete on SERVO_MOTION_DONE (monitored) with a travel-timeout backstop
 *   re-armed on every leg entry (incl. reversals) so a lost event can't hang.
 */

#ifndef HUBFX_GEAR_IPP
#define HUBFX_GEAR_IPP

#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw millis()

namespace hubfx::effects::gearctrl {

// ── Public commands ──────────────────────────────────────────────────

inline void Gear::setTarget(Target t) {
    if (_phase == GearPhase::Unconfigured) return;
    if (_phase == GearPhase::Error) {
        SFX_LOG_WARN("[gear] %u: target rejected — ERROR (GEAR_RESET first)", _def.id);
        return;
    }
    _stepMode = false;
    _target   = t;
    if (_begin && _sendCtx) _begin(_sendCtx);
    if (isCycling()) setPhaseSub(movingPhase(), _sub);   // reflect the new direction now
    pump();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::stepToward(Target t) {
    if (_phase == GearPhase::Unconfigured) return;
    if (_phase == GearPhase::Error) return;
    _stepMode  = true;
    _stepArmed = true;          // permission to cross ONE boundary
    _target    = t;
    if (_begin && _sendCtx) _begin(_sendCtx);
    if (isCycling()) setPhaseSub(movingPhase(), _sub);
    pump();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::emergencyHold() {
    if (_phase == GearPhase::Unconfigured) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _doorSeq.freeze();
    _legDone   = false;
    _stepArmed = false;
    _movingDeadlineMs = 0;
    _doorDeadlineMs   = 0;
    setPhaseSub(GearPhase::Held, GearSubPhase::idle);
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: EMERGENCY HOLD", _def.id);
}

inline void Gear::clearError() {
    if (_phase != GearPhase::Error) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _doorSeq.reset();
    _legDone   = false;
    _stepArmed = false;
    _errorReason = 0;
    _movingDeadlineMs = 0;
    _doorDeadlineMs   = 0;
    setPhaseSub(GearPhase::Unknown, GearSubPhase::idle);   // position uncertain after a fault
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: error cleared → unknown", _def.id);
}

// ── The engine ───────────────────────────────────────────────────────

inline void Gear::pump() {
    // ERROR / Unconfigured do not move autonomously.  Unknown / Held DO resume
    // (the idle case below begins a homing transit toward the new target).
    if (_phase == GearPhase::Unconfigured || _phase == GearPhase::Error) return;

    for (;;) {
        switch (_sub) {
            // ── settled (Up / Down / Unknown / Held) ──
            case GearSubPhase::idle: {
                if (settledAtTarget()) return;            // already there
                if (_stepMode && !_stepArmed) return;     // wait for the coordinator's step
                _stepArmed = false;
                setPhaseSub(movingPhase(), GearSubPhase::doors_opening);
                openDoors();
                if (_doorSeq.isComplete()) _legDone = true;   // NONE / no doors → instant
                if (!_legDone) return;                    // wait for the doors
                _legDone = false;
                setPhaseSub(movingPhase(), GearSubPhase::doors_open);
                continue;
            }

            // ── doors opening (shared by both directions) ──
            case GearSubPhase::doors_opening: {
                if (!_legDone) return;
                _legDone = false;
                setPhaseSub(movingPhase(), GearSubPhase::doors_open);
                continue;
            }

            // ── doors open: park (step mode) or run the strut ──
            case GearSubPhase::doors_open: {
                if (_stepMode && !_stepArmed) return;     // parked for the coordinator
                _stepArmed = false;
                setPhaseSub(movingPhase(), GearSubPhase::strut_moving);
                startSeek();                              // seeks toward _target, sets _seekDir
                return;                                   // wait for the endstop
            }

            // ── strut moving: wait, or re-seek on a target flip ──
            case GearSubPhase::strut_moving: {
                if (!_legDone) {
                    if (_seekDir != _target) startSeek();  // pre-empt: reverse the seek
                    return;
                }
                _legDone = false;
                setPhaseSub(movingPhase(), GearSubPhase::strut_done);
                continue;
            }

            // ── strut reached: reverse if flipped, else park / close ──
            case GearSubPhase::strut_done: {
                if (_seekDir != _target) {                // pre-empt: head back (doors still open)
                    setPhaseSub(movingPhase(), GearSubPhase::strut_moving);
                    startSeek();
                    return;
                }
                if (_stepMode && !_stepArmed) return;     // parked
                _stepArmed = false;
                setPhaseSub(movingPhase(), GearSubPhase::doors_closing);
                closeDoors();
                if (_doorSeq.isComplete()) _legDone = true;
                if (!_legDone) return;
                _legDone = false;
                setPhaseSub(movingPhase(), GearSubPhase::doors_closed);
                continue;
            }

            // ── doors closing: reverse (re-open) if flipped, else wait ──
            case GearSubPhase::doors_closing: {
                if (_seekDir != _target) {                // pre-empt: re-open + reverse
                    setPhaseSub(movingPhase(), GearSubPhase::doors_opening);
                    reverseDoors();
                    if (_doorSeq.isComplete()) _legDone = true;
                    if (!_legDone) return;
                    _legDone = false;
                    setPhaseSub(movingPhase(), GearSubPhase::doors_open);
                    continue;
                }
                if (!_legDone) return;
                _legDone = false;
                setPhaseSub(movingPhase(), GearSubPhase::doors_closed);
                continue;
            }

            // ── doors closed: reverse if flipped, else settle ──
            case GearSubPhase::doors_closed: {
                if (_seekDir != _target) {                // pre-empt: re-open + reverse
                    setPhaseSub(movingPhase(), GearSubPhase::doors_opening);
                    reverseDoors();
                    if (_doorSeq.isComplete()) _legDone = true;
                    if (!_legDone) return;
                    _legDone = false;
                    setPhaseSub(movingPhase(), GearSubPhase::doors_open);
                    continue;
                }
                const uint8_t settled = (_target == Target::Down) ? GearPhase::Down
                                                                  : GearPhase::Up;
                setPhaseSub(settled, GearSubPhase::idle);
                SFX_LOG_INFO("[gear] %u settled → %s", _def.id, GearPhase::getName(settled));
                return;
            }

            default: return;
        }
    }
}

// ── Coordinator predicate ────────────────────────────────────────────

inline bool Gear::legComplete() const {
    if (_phase == GearPhase::Error || _phase == GearPhase::Held) return true;  // never stall the barrier
    if (settledAtTarget()) return true;
    return _sub == GearSubPhase::doors_open || _sub == GearSubPhase::strut_done;
}

// ── Event ingress (all funnel into pump) ─────────────────────────────

inline void Gear::onServoMotionDone(uint8_t portIdx) {
    if (_sub != GearSubPhase::doors_opening && _sub != GearSubPhase::doors_closing) return;
    _doorSeq.onServoMotionDone(portIdx);
    if (!_doorSeq.isComplete()) return;
    _legDone = true;
    _doorDeadlineMs = 0;
    if (_begin && _sendCtx) _begin(_sendCtx);
    pump();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::onEndstopResult(uint8_t outcome) {
    if (_sub != GearSubPhase::strut_moving) return;       // stray result off the motor leg
    // We reverse a seek by re-issuing it (the role overwrites cleanly), never by
    // braking — so an `Aborted` is not part of normal flow; ignore it.
    if (outcome == BiMotorSeekOutcome::Aborted) return;
    if (outcome == BiMotorSeekOutcome::NoLoad) {
        SFX_LOG_WARN("[gear] %u: motor drew no current under load — NO MOTOR", _def.id);
        enterError(GearError::NO_MOTOR);
        return;
    }
    if (outcome == BiMotorSeekOutcome::Timeout) {
        SFX_LOG_WARN("[gear] %u: seek timed out — ERROR", _def.id);
        enterError(GearError::TIMEOUT);
        return;
    }
    // Reached — expander already braked locally; advance.
    _legDone = true;
    _movingDeadlineMs = 0;
    if (_begin && _sendCtx) _begin(_sendCtx);
    pump();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

// ── Tick ─────────────────────────────────────────────────────────────

inline void Gear::update(uint32_t nowMs) {
    if (!isCycling()) return;

    // Door legs: drive the DUAL_DELAY timer + completion + the missed-event
    // backstop (servos have no position feedback).
    if (_sub == GearSubPhase::doors_opening || _sub == GearSubPhase::doors_closing) {
        _doorSeq.update();
        bool done = _doorSeq.isComplete();
        if (!done && _doorDeadlineMs != 0 && (int32_t)(nowMs - _doorDeadlineMs) >= 0) {
            SFX_LOG_WARN("[gear] %u: door leg timed out (no SERVO_MOTION_DONE) — advancing", _def.id);
            done = true;
        }
        if (done) {
            _legDone = true;
            _doorDeadlineMs = 0;
            if (_begin && _sendCtx) _begin(_sendCtx);
            pump();
            if (_commit && _sendCtx) _commit(_sendCtx);
        }
        return;
    }

    // Motor leg: silent-expander backstop (the seek's own timeout is
    // authoritative and arrives as ENDSTOP_RESULT(timeout)).
    if (_sub == GearSubPhase::strut_moving) {
        if (_movingDeadlineMs == 0) return;
        if ((int32_t)(nowMs - _movingDeadlineMs) < 0) return;
        SFX_LOG_WARN("[gear] %u: expander silent past seek timeout — ERROR", _def.id);
        enterError(GearError::TIMEOUT);
    }
}

// ── Helpers ──────────────────────────────────────────────────────────

inline void Gear::enterError(uint8_t reason) {
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    _doorSeq.reset();
    _legDone   = false;
    _stepArmed = false;
    _errorReason = reason;
    _movingDeadlineMs = 0;
    _doorDeadlineMs   = 0;
    setPhaseSub(GearPhase::Error, GearSubPhase::idle);
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::setPhaseSub(uint8_t newPhase, uint8_t newSub) {
    const bool changed = (_phase != newPhase) || (_sub != newSub);
    // Leaving the Error phase clears the latched reason so the next event
    // doesn't carry a stale code.
    if (newPhase != GearPhase::Error) _errorReason = 0;
    _phase = newPhase;
    _sub   = newSub;
    if (changed) {
        SFX_LOG_DEBUG("[gear] %u → %s / %s", _def.id,
                      GearPhase::getName(newPhase), GearSubPhase::getName(newSub));
        if (_phaseEv) _phaseEv(_phaseCtx, _def.id, newPhase, newSub, _errorReason);
    }
}

inline void Gear::startSeek() {
    commandGuard();                 // push the saved stall guard before seeking
    commandSeek(seekDuty());        // toward _target
    _seekDir = _target;
    armMotorBackstop();
}

inline void Gear::openDoors() {
    _doorSeq.open();
    armDoorBackstop();
}

inline void Gear::closeDoors() {
    if (_target == Target::Down) _doorSeq.close();       // deploy → apply close policy
    else                         _doorSeq.closeFull();   // retract → re-stow everything
    armDoorBackstop();
}

inline void Gear::reverseDoors() {
    _doorSeq.reverse();             // re-open (flips the in-flight close)
    armDoorBackstop();
}

inline void Gear::armDoorBackstop() {
    _doorDeadlineMs = sfx_core::EffectClock::instance().nowMs()
                    + kDoorTravelTimeoutMs + _def.doorDelayMs;
}

inline void Gear::armMotorBackstop() {
    // Hub-side silent-expander guard: a bit beyond the seek's own timeout.
    // 0 timeout ⇒ no backstop (seek runs until stall/abort).
    _movingDeadlineMs = (_def.timeoutMs != 0)
        ? (sfx_core::EffectClock::instance().nowMs() + _def.timeoutMs + 1000u) : 0;
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

inline void Gear::commandGuard() {
    if (!_send) return;
    // BIMOTOR_SET_GUARD payload (matches BiMotorRoleHandler::handleSetGuard):
    //   [portIdx][mode][window_ms][a][b][c][d][ceiling_ma]  (all u16LE after mode)
    //   mode 1 (LiveRatio): a=ratio_x100, b=sample_ms, c=inrushBlank(0=default),
    //                       d=maxTravel(0=none); mode 0 (Fixed): a=threshold_ma.
    uint8_t payload[14];
    payload[0] = _def.motor.portIdx;
    payload[1] = _def.guardMode;
    SfxWire::putU16LE(&payload[2], _def.guardWindowMs);
    if (_def.guardMode == 1) {
        SfxWire::putU16LE(&payload[4], _def.guardRatioX100);
        SfxWire::putU16LE(&payload[6], _def.guardSampleMs);
        SfxWire::putU16LE(&payload[8], 0);     // inrushBlank → role default
        SfxWire::putU16LE(&payload[10], 0);    // maxTravel   → none
    } else {
        SfxWire::putU16LE(&payload[4], _def.guardThresholdMa);
        SfxWire::putU16LE(&payload[6], 0);
        SfxWire::putU16LE(&payload[8], 0);
        SfxWire::putU16LE(&payload[10], 0);
    }
    SfxWire::putU16LE(&payload[12], _def.guardCeilingMa);
    _send(_sendCtx, _def.motor,
          RolePacket::BIMOTOR_SET_GUARD, payload, sizeof(payload));
}

inline void Gear::commandMotorBrake() {
    if (!_send) return;
    const uint8_t payload[1] = { _def.motor.portIdx };
    _send(_sendCtx, _def.motor,
          RolePacket::BIMOTOR_BRAKE, payload, sizeof(payload));
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEAR_IPP
