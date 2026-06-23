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
    _manualLeg = ManualLeg::None;   // a coordinated cycle takes over any manual op
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
    _manualLeg = ManualLeg::None;   // a coordinated step takes over any manual op
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
    _strutState = GearStrutState::Unknown;   // frozen mid-travel → position uncertain
    _manualLeg  = ManualLeg::None;
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
    _strutState = GearStrutState::Unknown;
    _manualLeg  = ManualLeg::None;
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
    // Manual door op: settle independently — do NOT re-enter the coordinated pump.
    if (_manualLeg == ManualLeg::DoorOpen || _manualLeg == ManualLeg::DoorClose) {
        _doorSeq.onServoMotionDone(portIdx);
        if (!_doorSeq.isComplete()) return;
        if (_begin && _sendCtx) _begin(_sendCtx);
        finishManualDoor(_manualLeg == ManualLeg::DoorOpen);
        if (_commit && _sendCtx) _commit(_sendCtx);
        return;
    }
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
    // Manual strut op: settle independently — do NOT re-enter the coordinated pump.
    if (_manualLeg == ManualLeg::StrutDown || _manualLeg == ManualLeg::StrutUp) {
        if (outcome == BiMotorSeekOutcome::Aborted) return;
        if (outcome == BiMotorSeekOutcome::NoLoad) { enterError(GearError::NO_MOTOR); return; }
        if (outcome == BiMotorSeekOutcome::Timeout) { enterError(GearError::TIMEOUT); return; }
        if (_begin && _sendCtx) _begin(_sendCtx);
        finishManualStrut();
        if (_commit && _sendCtx) _commit(_sendCtx);
        return;
    }
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
    // Reached — expander already braked locally; record the strut end + advance.
    _strutState = (_seekDir == Target::Down) ? GearStrutState::Out : GearStrutState::Up;
    _legDone = true;
    _movingDeadlineMs = 0;
    if (_begin && _sendCtx) _begin(_sendCtx);
    pump();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

// ── Tick ─────────────────────────────────────────────────────────────

inline void Gear::update(uint32_t nowMs) {
    // ── Manual ops: same backstops as the coordinated legs, but they settle via
    //    finishManual* (no pump) so a lost SERVO_MOTION_DONE / ENDSTOP_RESULT
    //    can't hang an independent door/strut move. ──
    if (_manualLeg == ManualLeg::DoorOpen || _manualLeg == ManualLeg::DoorClose) {
        _doorSeq.update();
        bool done = _doorSeq.isComplete();
        if (!done && _doorDeadlineMs != 0 && (int32_t)(nowMs - _doorDeadlineMs) >= 0) {
            SFX_LOG_WARN("[gear] %u: manual door leg timed out — settling", _def.id);
            done = true;
        }
        if (done) {
            if (_begin && _sendCtx) _begin(_sendCtx);
            finishManualDoor(_manualLeg == ManualLeg::DoorOpen);
            if (_commit && _sendCtx) _commit(_sendCtx);
        }
        return;
    }
    if (_manualLeg == ManualLeg::StrutDown || _manualLeg == ManualLeg::StrutUp) {
        if (_movingDeadlineMs != 0 && (int32_t)(nowMs - _movingDeadlineMs) >= 0) {
            SFX_LOG_WARN("[gear] %u: manual strut silent past seek timeout — ERROR", _def.id);
            enterError(GearError::TIMEOUT);
        }
        return;
    }

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
    _strutState = GearStrutState::Unknown;
    _manualLeg  = ManualLeg::None;
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
    _strutState = GearStrutState::Moving;
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
    // Mode-aware budget: the doors don't always travel in parallel.
    //   DUAL_SEQ   — door1 only dispatches on door0's SERVO_MOTION_DONE, so the
    //                two legs travel SERIALLY → budget 2× the single-door travel.
    //   DUAL_DELAY — door1 launches doorDelayMs after door0 → that delay PLUS one
    //                travel window.
    //   SYNC/SINGLE/NONE — both (or the one) door move together → one window.
    uint32_t budget = kDoorTravelTimeoutMs;
    switch (_def.openMode) {
        case DoorMode::DUAL_SEQ:   budget = 2u * kDoorTravelTimeoutMs;        break;
        case DoorMode::DUAL_DELAY: budget = kDoorTravelTimeoutMs + _def.doorDelayMs; break;
        default:                   budget = kDoorTravelTimeoutMs;             break;
    }
    _doorDeadlineMs = sfx_core::EffectClock::instance().nowMs() + budget;
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

// ── Manual / maintenance: drive ONE subsystem independently ──────────────
//
//   These bypass pump()'s coordinated walk but reuse the same door/motor
//   primitives.  The interlock is enforced HERE (the firmware is authoritative;
//   the GUI gate only mirrors it).  A manual op is refused while a coordinated
//   cycle or another manual op is in flight; a later coordinated command clears
//   `_manualLeg` and takes over (setTarget/stepToward), so the emergency
//   connection-loss deploy is never blocked.

inline uint8_t Gear::strutPhase() const {
    switch (_strutState) {
        case GearStrutState::Up:     return GearPhase::Up;
        case GearStrutState::Out:    return GearPhase::Down;
        case GearStrutState::Moving: return (_seekDir == Target::Down) ? GearPhase::MovingDown
                                                                       : GearPhase::MovingUp;
        default:                     return GearPhase::Unknown;
    }
}

inline Gear::ManualResult Gear::checkManualDoors(bool open) const {
    if (_phase == GearPhase::Unconfigured || _phase == GearPhase::Error) return ManualResult::Busy;
    if (isCycling() || _manualLeg != ManualLeg::None)                    return ManualResult::Busy;
    // INTERLOCK: only close the doors when the strut is retracted (UP).
    if (!open && !strutUp()) return ManualResult::StrutNotUp;
    return ManualResult::Ok;
}

inline Gear::ManualResult Gear::checkManualStrut(bool /*down*/) const {
    if (_phase == GearPhase::Unconfigured || _phase == GearPhase::Error) return ManualResult::Busy;
    if (isCycling() || _manualLeg != ManualLeg::None)                    return ManualResult::Busy;
    // INTERLOCK: the leg passes through the door gap either way → doors must be open.
    if (!doorsOpen()) return ManualResult::DoorsClosed;
    return ManualResult::Ok;
}

inline Gear::ManualResult Gear::manualDoors(bool open) {
    const ManualResult chk = checkManualDoors(open);
    if (chk != ManualResult::Ok) return chk;

    if (_begin && _sendCtx) _begin(_sendCtx);
    _manualLeg = open ? ManualLeg::DoorOpen : ManualLeg::DoorClose;
    if (open) _doorSeq.open();
    else      _doorSeq.closeFull();          // manual close = re-stow every door
    armDoorBackstop();
    setPhaseSub(strutPhase(), open ? GearSubPhase::doors_opening : GearSubPhase::doors_closing);
    if (_doorSeq.isComplete()) finishManualDoor(open);   // instant (already at that end / no doors)
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: manual doors → %s", _def.id, open ? "open" : "close");
    return ManualResult::Ok;
}

inline Gear::ManualResult Gear::manualStrut(bool down) {
    const ManualResult chk = checkManualStrut(down);
    if (chk != ManualResult::Ok) return chk;

    if (_begin && _sendCtx) _begin(_sendCtx);
    _target    = down ? Target::Down : Target::Up;   // drives seekDuty() + reporting
    _manualLeg = down ? ManualLeg::StrutDown : ManualLeg::StrutUp;
    startSeek();                                      // guard + seek + _seekDir + _strutState=Moving + backstop
    setPhaseSub(movingPhase(), GearSubPhase::strut_moving);
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gear] %u: manual strut → %s", _def.id, down ? "down" : "up");
    return ManualResult::Ok;
}

inline void Gear::finishManualDoor(bool opened) {
    _manualLeg = ManualLeg::None;
    _doorDeadlineMs = 0;
    // Doors settle open (awaiting nothing) or closed (idle); the strut hasn't
    // moved, so the overall phase still reflects the strut position.
    setPhaseSub(strutPhase(), opened ? GearSubPhase::doors_open : GearSubPhase::idle);
    SFX_LOG_INFO("[gear] %u: manual doors %s", _def.id, opened ? "open" : "closed");
}

inline void Gear::finishManualStrut() {
    _manualLeg = ManualLeg::None;
    _movingDeadlineMs = 0;
    _strutState = (_seekDir == Target::Down) ? GearStrutState::Out : GearStrutState::Up;
    // The doors were open to permit the move and stay open (manual = independent).
    setPhaseSub(strutPhase(), GearSubPhase::doors_open);
    SFX_LOG_INFO("[gear] %u: manual strut %s", _def.id,
                 _strutState == GearStrutState::Out ? "out" : "up");
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEAR_IPP
