/*
 * door_sequencer.ipp — DoorSequencer method bodies.
 */

#ifndef HUBFX_GEARCONTROL_DOOR_SEQUENCER_IPP
#define HUBFX_GEARCONTROL_DOOR_SEQUENCER_IPP

#include <serial/diag_log.h>

namespace hubfx::effects::gearctrl {

inline void DoorSequencer::commandDoor(uint8_t i, uint16_t posNorm) {
    if (i >= _numDoors) return;
    _commanded[i] = true;
    const int8_t targetEnd = (posNorm == RolePacket::kPosNormFull) ? 1 : 0;
    const bool   noop      = (_doorEnd[i] == targetEnd);   // already at this end
    _doorEnd[i] = targetEnd;                               // optimistic: heading there
    _done[i]    = false;
    if (!_send) { _done[i] = true; return; }   // no transport — treat as instant
    // Re-assert the position even on a no-op (cheap, guarantees the door is at
    // the commanded end before the strut moves) — but DON'T wait for a
    // SERVO_MOTION_DONE that won't come because the servo doesn't actually move
    // (the close_policy=none retract re-opening already-open doors = the pause).
    uint8_t payload[3];
    payload[0] = _doors[i].servo.portIdx;
    SfxWire::putU16LE(&payload[1], posNorm);
    _send(_sendCtx, _doors[i].servo,
          RolePacket::SERVO_SET_POS_NORM, payload, sizeof(payload));
    if (noop) {
        _done[i] = true;
        SFX_LOG_DEBUG("[gear-door] door %u already %s — no-op (no wait)",
                      (unsigned)i, targetEnd ? "open" : "closed");
    }
}

inline bool DoorSequencer::allCommandedDone() const {
    if (_secondPending) return false;   // deferred door not even dispatched yet
    for (uint8_t i = 0; i < _numDoors; ++i) {
        if (_commanded[i] && !_done[i]) return false;
    }
    return true;
}

inline void DoorSequencer::begin(bool opening, uint8_t doorMask) {
    _opening        = opening;
    _commanded[0]   = _commanded[1] = false;
    _done[0]        = _done[1]      = false;
    _secondPending  = false;
    _delayDeadlineMs= 0;

    // No doors / NONE mode → instant completion.
    if (_numDoors == 0 || _openMode == DoorMode::NONE || doorMask == 0) {
        _state = State::Complete;
        return;
    }

    _state = opening ? State::Opening : State::Closing;

    // Open → calibrated MAX-µs end, close → MIN-µs end (the servo role's REV
    // flag flips direction); travel lives in the servo calibration, not here.
    auto targetFor = [&](uint8_t /*i*/) -> uint16_t {
        return opening ? RolePacket::kPosNormFull : (uint16_t)0;
    };

    const bool d0 = (doorMask & 0x01) != 0;
    const bool d1 = (doorMask & 0x02) != 0 && _numDoors >= 2;

    switch (_openMode) {
        case DoorMode::SINGLE:
            if (d0) commandDoor(0, targetFor(0));
            break;

        case DoorMode::DUAL_SYNC:
            if (d0) commandDoor(0, targetFor(0));
            if (d1) commandDoor(1, targetFor(1));
            break;

        case DoorMode::DUAL_DELAY:
        case DoorMode::DUAL_SEQ: {
            // Ordering: open 0→1, close 1→0 (inner door opens first /
            // closes last — mimics real aircraft).  Honour the mask so a
            // FIRST close policy that excludes a door collapses gracefully.
            uint8_t first  = opening ? 0 : 1;
            uint8_t second = opening ? 1 : 0;
            const bool firstWanted  = (first  == 0) ? d0 : d1;
            const bool secondWanted = (second == 0) ? d0 : d1;
            _firstIdx  = first;
            _secondIdx = second;
            if (firstWanted) {
                commandDoor(first, targetFor(first));
                if (secondWanted) {
                    _secondPending = true;
                    _secondTarget  = targetFor(second);
                    if (_openMode == DoorMode::DUAL_DELAY) {
                        _delayDeadlineMs =
                            sfx_core::EffectClock::instance().nowMs() + _doorDelayMs;
                    }
                    // DUAL_SEQ: dispatched on first door's SERVO_MOTION_DONE.
                }
            } else if (secondWanted) {
                // Only the second door participates (masked first) — just run it.
                commandDoor(second, targetFor(second));
            }
            break;
        }

        default:
            if (d0) commandDoor(0, targetFor(0));
            if (d1) commandDoor(1, targetFor(1));
            break;
    }

    // Everything we commanded may already be instant (no transport / mode
    // collapse) — settle immediately if so.
    if (allCommandedDone()) _state = State::Complete;
}

inline void DoorSequencer::open() {
    begin(/*opening=*/true, /*doorMask=*/0x03);   // open every door
}

inline void DoorSequencer::close() {
    // Post-deploy close policy.
    uint8_t mask = 0x03;
    switch (_closePolicy) {
        case ClosePolicy::BOTH:  mask = 0x03; break;
        case ClosePolicy::FIRST: mask = 0x01; break;  // door0 only; door1 stays open
        case ClosePolicy::NONE:  mask = 0x00; break;  // no doors close
        default:                 mask = 0x03; break;
    }
    begin(/*opening=*/false, mask);
}

inline void DoorSequencer::closeFull() {
    begin(/*opening=*/false, /*doorMask=*/0x03);  // retract re-stows everything
}

inline void DoorSequencer::update() {
    if (_state != State::Opening && _state != State::Closing) return;

    // DUAL_DELAY: dispatch the deferred second door once the timer elapses.
    if (_secondPending && _openMode == DoorMode::DUAL_DELAY) {
        const uint32_t now = sfx_core::EffectClock::instance().nowMs();
        if (_delayDeadlineMs != 0 && (int32_t)(now - _delayDeadlineMs) >= 0) {
            _secondPending = false;
            commandDoor(_secondIdx, _secondTarget);
        }
    }

    if (allCommandedDone()) _state = State::Complete;
}

inline void DoorSequencer::onServoMotionDone(uint8_t portIdx) {
    if (_state != State::Opening && _state != State::Closing) return;

    for (uint8_t i = 0; i < _numDoors; ++i) {
        if (_commanded[i] && !_done[i] && _doors[i].servo.portIdx == portIdx) {
            _done[i] = true;
            SFX_LOG_DEBUG("[gear-door] door %u motion done (portIdx=%u)",
                          (unsigned)i, (unsigned)portIdx);
            // DUAL_SEQ: dispatch the deferred second door on the first's done.
            if (_secondPending && _openMode == DoorMode::DUAL_SEQ &&
                i == _firstIdx) {
                _secondPending = false;
                commandDoor(_secondIdx, _secondTarget);
            }
            break;
        }
    }

    if (allCommandedDone()) _state = State::Complete;
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEARCONTROL_DOOR_SEQUENCER_IPP
