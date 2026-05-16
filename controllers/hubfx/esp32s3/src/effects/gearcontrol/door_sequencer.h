/*
 * DoorSequencer (master-side) — mode-aware door servo sequencing.
 *
 * Ported from controllers/gearcontrol/pico/src/door_sequencer.{h,cpp}
 * (slave-side) with one structural change for the new architecture:
 *
 *   The slave version held direct `ServoControl*` pointers and polled
 *   `atTarget()` from update().  In the new master/slave split the
 *   sequencer runs on HubFX, so it cannot read servo state in real
 *   time — instead it:
 *     1. emits servo target commands via an injected `ServoCommandFn`
 *        callback (the orchestrator wires this to SlaveApi::servoSet)
 *     2. is notified of completion via `notifyTargetReached(idx)` from
 *        the orchestrator's onServoTargetReached hook (the master
 *        observes the slave's TAG_ASYNC SERVO_TARGET_REACHED packets)
 *
 *   No `update()` polling of servo state is needed — the state machine
 *   is purely event-driven.  A settle timer still runs in update() so
 *   we can hold COMPLETE for a short duration after the final door
 *   reports target-reached (mirrors the slave-side SETTLE_TIME_ms).
 *
 * Modes (wire-format compatible — uses gearcontrol::DoorMode constants):
 *   NONE        - No door servos (immediate completion, no commands emitted)
 *   SINGLE      - One door servo (door 0 only)
 *   DUAL_SYNC   - Two doors, commanded simultaneously
 *   DUAL_DELAY  - Two doors, door 1 commanded after configurable delay
 *   DUAL_SEQ    - Two doors, door 1 commanded after door 0 reports target-reached
 *
 *   For DUAL_DELAY and DUAL_SEQ: doors open 0->1, close 1->0 (inner door
 *   opens first, closes last — mimics real aircraft).
 *
 * Signal-emission stubs (deferred wiring):
 *   The DoorSequencer never references SlaveApi directly.  All outbound
 *   command emission goes through `ServoCommandFn`, which the
 *   GearControlEffect orchestrator binds at begin() time once the
 *   master->slave servo command path is decided.
 */

#ifndef HUBFX_GEARCONTROL_DOOR_SEQUENCER_H
#define HUBFX_GEARCONTROL_DOOR_SEQUENCER_H

#include <Arduino.h>
#include <cstdint>
#include <functional>

#include <serial/gearcontrol/gearcontrol.h>   // DoorMode + DoorState wire-format constants

namespace hubfx::effects::gearcontrol {

namespace DoorSeqConfig {
    constexpr uint16_t SETTLE_TIME_ms = 200;   ///< hold COMPLETE briefly after final atTarget
}

/// Per-door open/close target microseconds.  The orchestrator owns the
/// configured µs values (typically derived from GearControlConfig
/// doorClosed_us / doorOpen_us, optionally per-door) and supplies them
/// here so the sequencer can pass them straight through to
/// ServoCommandFn without duplicating policy.
struct DoorTargets {
    uint16_t open_us  = 2000;
    uint16_t close_us = 1000;
};

class DoorSequencer {
public:
    /// Emit "command door <doorIndex 0..1> to <pulse_us>" to the slave.
    /// Wired by GearControlEffect to a SlaveApi::servoSet call (or
    /// equivalent passthrough) at begin() time.  Until wired, calls
    /// are silent stubs — the sequencer still drives its state machine
    /// but emits no wire packets.
    using ServoCommandFn = std::function<void(uint8_t doorIndex, uint16_t pulse_us)>;

    DoorSequencer() = default;
    ~DoorSequencer() = default;

    DoorSequencer(const DoorSequencer&)            = delete;
    DoorSequencer& operator=(const DoorSequencer&) = delete;

    // ── Initialization ───────────────────────────────────────────────

    /// Bind the outbound servo-command callback + initial per-door
    /// open/close µs targets.  `door1Targets` may be left default if
    /// SINGLE mode is the only mode that will be used.
    void begin(ServoCommandFn  servoCmd,
               const DoorTargets& door0Targets,
               const DoorTargets& door1Targets) {
        _emit          = std::move(servoCmd);
        _targets[0]    = door0Targets;
        _targets[1]    = door1Targets;
    }

    /// Update door target µs values (e.g. after reconfigure).  Safe to
    /// call mid-sequence; in-flight commands are not rewritten, but
    /// any subsequent phase will use the new values.
    void setTargets(uint8_t doorIndex, const DoorTargets& tgt) {
        if (doorIndex < 2) _targets[doorIndex] = tgt;
    }

    // ── Configuration ────────────────────────────────────────────────

    void setMode(uint8_t preDeployMode,
                 uint8_t postDeployMode = ::DoorMode::NONE,
                 uint16_t delay_ms      = 500) {
        _preDeployMode  = preDeployMode;
        _postDeployMode = postDeployMode;
        _delay_ms       = delay_ms;
    }

    uint8_t  preDeployMode()  const { return _preDeployMode;  }
    uint8_t  postDeployMode() const { return _postDeployMode; }
    uint16_t delay_ms()       const { return _delay_ms;       }

    // ── Sequenced operations ─────────────────────────────────────────

    /// `deploying = true` ⇒ uses preDeployMode (open before deploy motor).
    /// `deploying = false` ⇒ uses postDeployMode (open before retract motor).
    /// Mode == NONE collapses to immediate COMPLETE without emitting commands.
    void startOpen(bool deploying = true);

    /// `deploying = true` ⇒ uses postDeployMode (close after deploy motor).
    /// `deploying = false` ⇒ uses preDeployMode (close after retract motor).
    /// Mode == NONE collapses to immediate COMPLETE without emitting commands.
    void startClose(bool deploying = true);

    /// Drive the timer-based portions of the state machine.  In the
    /// master-side design this only services the DUAL_DELAY phase
    /// transition + the SETTLE_TIME_ms hold; door-reached events come
    /// in via notifyTargetReached().
    void update();

    void reset() { _state = State::IDLE; }

    // ── Async-event ingress ──────────────────────────────────────────

    /// Called by the orchestrator's onServoTargetReached hook when the
    /// slave reports a target-reached event for one of the door servo
    /// indices that this sequencer is interested in.  `doorIndex` is
    /// 0 or 1 — the orchestrator translates from the wire-format servo
    /// channel into the local 0/1 door-slot ordinal before calling.
    void notifyTargetReached(uint8_t doorIndex);

    // ── State queries ────────────────────────────────────────────────

    bool isComplete() const { return _state == State::COMPLETE; }
    bool isIdle()     const { return _state == State::IDLE;     }
    bool isBusy()     const { return _state == State::OPENING ||
                                     _state == State::CLOSING;  }

    /// Returns a wire-format DoorState value (UNKNOWN / CLOSED / OPEN /
    /// OPENING / CLOSING) for inclusion in HubStatusBuilder output.
    uint8_t doorState() const { return _doorState; }

    // ── Direct control (bypasses sequencing) ─────────────────────────

    /// Command all configured doors to open immediately, no settle
    /// timer / state machine.  Uses preDeployMode to decide which
    /// doors to address (matches slave-side semantics).
    void openImmediate();

    /// Command all configured doors to close immediately.
    void closeImmediate();

    void setPosition(uint8_t doorIndex, uint16_t pos_us) {
        if (doorIndex < 2 && _emit) _emit(doorIndex, pos_us);
    }

private:
    enum class State : uint8_t { IDLE, OPENING, CLOSING, COMPLETE };

    ServoCommandFn _emit;
    DoorTargets    _targets[2] = {{}, {}};

    uint8_t  _preDeployMode  = ::DoorMode::DUAL_SYNC;
    uint8_t  _postDeployMode = ::DoorMode::NONE;
    uint8_t  _activeMode     = ::DoorMode::DUAL_SYNC;
    uint16_t _delay_ms       = 500;

    State    _state          = State::IDLE;
    uint8_t  _doorState      = ::DoorState::UNKNOWN;

    // Phase 0 = first door commanded, awaiting reach (or delay timer).
    // Phase 1 = second door commanded, awaiting reach.
    uint8_t  _phase          = 0;
    uint32_t _phaseStart_ms  = 0;
    uint32_t _settleStart_ms = 0;   ///< restart whenever any required door is still pending

    // Per-door target-reached flags for the current sequence — cleared
    // on startOpen/startClose, set by notifyTargetReached, consumed by
    // the state machine.
    bool     _reached[2]     = { false, false };

    // Direction context — encodes door ordering for DUAL_DELAY/DUAL_SEQ.
    //   Opening: first=0, second=1
    //   Closing: first=1, second=0
    struct Direction {
        uint8_t  firstIdx;
        uint8_t  secondIdx;
        uint16_t firstTarget_us;
        uint16_t secondTarget_us;
        uint8_t  finalDoorState;
    } _dir{};

    void _emitCommand(uint8_t doorIndex, uint16_t pulse_us) {
        if (_emit) _emit(doorIndex, pulse_us);
    }
    void _completeIfSettled(uint32_t now);
    bool _allRequiredReached() const;
};

}  // namespace hubfx::effects::gearcontrol

#endif  // HUBFX_GEARCONTROL_DOOR_SEQUENCER_H
