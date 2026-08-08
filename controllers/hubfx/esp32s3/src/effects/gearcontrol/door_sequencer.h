/*
 * door_sequencer.h — per-gear door servo sequencing (HubFX master).
 *
 *   Ported from controllers/archive/hubfx-esp32s3/.../door_sequencer.{h,cpp}
 *   and adapted to the role/topology architecture (instructions/29):
 *
 *     - Drives ≤2 door servos via SERVO_SET_POS_NORM (normalised intent,
 *       Rule "servo intent is normalised") through the SAME
 *       `Gear::SendRoleCmdFn` the motor leg uses (topology forwards to the
 *       GUID'd port).
 *     - Completion is MONITORED, not timed (decision #1): each commanded
 *       door reports SERVO_MOTION_DONE; the sequencer records which door
 *       PortRefs it commanded and which have reported done.  `isComplete()`
 *       once every commanded door is done.  `onServoMotionDone(portIdx)`
 *       is called by the gear service when a SERVO_MOTION_DONE async
 *       arrives for one of this gear's door ports.
 *     - Modes (DoorMode): NONE / SINGLE / DUAL_SYNC / DUAL_DELAY / DUAL_SEQ.
 *       DUAL_SEQ dispatches door1 on door0's SERVO_MOTION_DONE; DUAL_DELAY
 *       dispatches door1 after `doorDelayMs` (EffectClock — Rule 40, never
 *       millis()).  SINGLE = door0 only; DUAL_SYNC = both at once.
 *     - closePolicy (post-deploy close): BOTH / FIRST (door0 only) / NONE.
 *
 *   The sequencer owns no transport: it is handed the gear's send callback
 *   + door defs at configure() time and emits role commands through it.
 */

#ifndef HUBFX_GEARCONTROL_DOOR_SEQUENCER_H
#define HUBFX_GEARCONTROL_DOOR_SEQUENCER_H

#include <cstdint>
#include <cstring>

#include <serial/roles.h>
#include <serial/wire.h>
#include <server/effect_clock.h>   // Rule 40 — EffectClock, not raw millis()

#include "../effect_id.h"          // PortRef
#include "gear.h"                  // DoorDef + SendRoleCmdFn (shared types)
#include "gearcontrol_protocol.h"  // DoorMode / ClosePolicy

namespace hubfx::effects::gearctrl {

class DoorSequencer {
public:
    using SendRoleCmdFn = gearctrl::SendRoleCmdFn;

    DoorSequencer() = default;

    /// Bind the door defs + the role-command send path.  `doors`/`numDoors`
    /// + the pairing config come straight from the owning GearDef.
    void configure(const DoorDef* doors, uint8_t numDoors,
                   uint8_t openMode, uint16_t doorDelayMs, uint8_t closePolicy,
                   SendRoleCmdFn sendFn, void* sendCtx) {
        _numDoors    = (numDoors > 2) ? 2 : numDoors;
        for (uint8_t i = 0; i < 2; ++i) {
            _doors[i] = (i < _numDoors) ? doors[i] : DoorDef{};
        }
        _openMode    = openMode;
        _doorDelayMs = doorDelayMs;
        _closePolicy = closePolicy;
        _send        = sendFn;
        _sendCtx     = sendCtx;
        _state       = State::Idle;
        _doorEnd[0]  = _doorEnd[1] = -1;   // position unknown until first commanded
    }

    /// Open the doors before the motor seek.  Honours `openMode`.  NONE /
    /// numDoors==0 completes instantly.
    void open();

    /// Close the doors after a deploy.  Honours `closePolicy` (BOTH / FIRST /
    /// NONE).  Used after the motor leg of a deploy; retract's close re-closes
    /// everything (closePolicy == BOTH semantics) — see closeFull().
    void close();

    /// Close every commanded door regardless of policy (retract path — a
    /// retract always re-stows the doors it opened).
    void closeFull();

    /// Tick — drives the DUAL_DELAY timer leg only (EffectClock).  Door
    /// completion arrives via onServoMotionDone(), not polling.
    void update();

    /// Called by the gear service when a SERVO_MOTION_DONE async arrives for
    /// `portIdx` (already matched to this gear's board GUID).  Marks the
    /// matching door done + dispatches the deferred door for DUAL_SEQ.
    void onServoMotionDone(uint8_t portIdx);

    /// Reverse an in-flight open↔close: re-command every door to the OPPOSITE
    /// target + reset the per-door bookkeeping.  Used by the strut FSM when the
    /// gear target flips while the doors are moving (a closing set re-opens).
    void reverse() { begin(!_opening, /*doorMask=*/0x03); }

    /// Emergency freeze: stop driving the sequence.  The servos hold their last
    /// commanded position (there is no feedback to halt mid-travel, so a door
    /// caught mid-move completes that small move, then holds).  The motor brake
    /// in `Gear::emergencyHold()` is the load-bearing safety action.
    void freeze() { _state = State::Idle; _doorEnd[0] = _doorEnd[1] = -1; }

    bool isComplete() const { return _state == State::Complete; }
    bool isIdle()     const { return _state == State::Idle; }
    bool isBusy()     const { return _state == State::Opening || _state == State::Closing; }

    /// True when EVERY configured door is at its OPEN end (vacuously true when
    /// there are no doors).  The manual-control interlock reads this: the strut
    /// may only move while the doors are clear of its travel.  Uses the
    /// persistent per-door end (`_doorEnd`, 1=open), so it's valid when settled.
    bool allAtOpen() const {
        for (uint8_t i = 0; i < _numDoors; ++i) if (_doorEnd[i] != 1) return false;
        return true;
    }
    /// True when every configured door is at its CLOSED end (false if any is
    /// open or unknown; vacuously true with no doors).
    bool allAtClosed() const {
        for (uint8_t i = 0; i < _numDoors; ++i) if (_doorEnd[i] != 0) return false;
        return true;
    }

    void reset() { _state = State::Idle; _doorEnd[0] = _doorEnd[1] = -1; }

private:
    enum class State : uint8_t { Idle, Opening, Closing, Complete };

    /// Send SERVO_SET_TARGET to door `i` — the door's configured ABSOLUTE
    /// open/close µs (2.46.0) — and mark it as commanded awaiting completion.
    void commandDoor(uint8_t i, bool toOpen);

    /// True once every door we commanded this sequence has reported done.
    bool allCommandedDone() const;

    /// Begin an open/close sequence: clear flags, set ordering, dispatch the
    /// first door(s) per mode.  `opening` selects open vs close targets +
    /// door ordering (open 0→1, close 1→0).  `closeMask` restricts which
    /// doors participate on close (for closePolicy).
    void begin(bool opening, uint8_t doorMask);

    DoorDef       _doors[2]   = {};
    uint8_t       _numDoors   = 0;
    uint8_t       _openMode   = DoorMode::DUAL_SYNC;
    uint16_t      _doorDelayMs= 500;
    uint8_t       _closePolicy= ClosePolicy::BOTH;

    SendRoleCmdFn _send       = nullptr;
    void*         _sendCtx    = nullptr;

    State         _state      = State::Idle;
    bool          _opening    = true;       ///< current sequence direction

    // Per-door bookkeeping for the active sequence.
    bool          _commanded[2] = { false, false };  ///< did we command this door?
    bool          _done[2]      = { false, false };  ///< has it reported SERVO_MOTION_DONE?

    // Persistent (cross-sequence) door end: 1 = open, 0 = closed, -1 = unknown.
    // Lets a repeat command (e.g. open already-open doors on a close_policy=none
    // retract) complete instantly instead of waiting on a SERVO_MOTION_DONE that
    // never comes (the servo doesn't move) — the door-pause bug.
    int8_t        _doorEnd[2]   = { -1, -1 };

    // DUAL_DELAY / DUAL_SEQ deferred-second-door bookkeeping.
    bool          _secondPending = false;   ///< second door not yet dispatched
    uint8_t       _firstIdx      = 0;       ///< first door dispatched
    uint8_t       _secondIdx     = 1;       ///< second door (deferred)
    uint32_t      _delayDeadlineMs = 0;     ///< EffectClock deadline for DUAL_DELAY
};

}  // namespace hubfx::effects::gearctrl

#include "door_sequencer.ipp"

#endif  // HUBFX_GEARCONTROL_DOOR_SEQUENCER_H
