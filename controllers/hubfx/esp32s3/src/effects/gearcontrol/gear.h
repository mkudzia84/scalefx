/*
 * gear.h — single-gear state machine for the GearControl effect.
 *
 *   Drives one H-bridge motor through topology.  Same architectural
 *   pattern as `LandingLight`: construction installs callbacks that the
 *   owning service binds to topology's `sendRoleCommand` + batch
 *   primitives + phase broadcast.  No transport / no template-pack
 *   knowledge leaks into the state machine itself.
 *
 *   Motion uses the BiDcMotor role's autonomous endstop seek
 *   (`BIMOTOR_SEEK_ENDSTOP`): the gear sends one seek command per leg
 *   and the expander drives → detects the stall → brakes locally, then
 *   reports `BIMOTOR_ENDSTOP_RESULT`.  The stall→stop loop never crosses
 *   the wire (no endstop hammering); the seek's own timeout (= the
 *   gear's `timeoutMs`, 0 = none) is authoritative and surfaces as the
 *   `Timeout` outcome → the gear enters ERROR.  Deploy / retract are
 *   single seeks; calibration is a three-leg sweep (retract → deploy →
 *   home).
 *
 *   Status LEDs are NOT driven from here — the GearControl expander
 *   board lights its own small per-motor direction LEDs locally from
 *   the H-bridge drive state (see controllers/gearcontrol/pico).
 *
 *   Deploy / retract are timeout-bounded: the motor runs in the
 *   right direction until either:
 *     - `MOTOR_STALL_EVENT` arrives for our motor port (= mechanical
 *       endpoint reached), or
 *     - `timeoutMs` elapses (the service ticks `update(now)` in its
 *       update() callback).
 *   Either path triggers the matching done-transition; the timeout
 *   path additionally enters ERROR.
 */

#ifndef HUBFX_GEAR_H
#define HUBFX_GEAR_H

#include <cstdint>

#include "../effect_id.h"
#include "gearcontrol_protocol.h"

namespace hubfx::effects::gearctrl {

struct GearDef {
    uint8_t  id            = 0;
    char     name[16]      = {};
    PortRef  motor;                            ///< must have BiDcMotor role

    // Motor speed when running (-32767..32767 signed duty).  Negative
    // values reverse the H-bridge polarity at runtime.
    int16_t  deployDuty    = +20000;            ///< signed duty for "going down"
    int16_t  retractDuty   = -20000;            ///< signed duty for "going up"
    uint32_t timeoutMs     = 4000;              ///< full-travel timeout
};

class Gear {
public:
    using SendRoleCmdFn = bool (*)(void* ctx, const PortRef& addr,
                                   uint8_t innerType,
                                   const uint8_t* payload, size_t len);
    using BatchFn       = void (*)(void* ctx);
    using PhaseEventFn  = void (*)(void* ctx, uint8_t id, uint8_t newPhase);

    Gear() = default;

    void configure(const GearDef& def,
                   SendRoleCmdFn sendFn, void* sendCtx,
                   BatchFn beginFn, BatchFn commitFn,
                   PhaseEventFn phaseFn = nullptr, void* phaseCtx = nullptr) {
        _def      = def;
        _send     = sendFn;
        _sendCtx  = sendCtx;
        _begin    = beginFn;
        _commit   = commitFn;
        _phase    = phaseFn;
        _phaseCtx = phaseCtx;
        _state    = GearPhase::Retracted;
        _movingDeadlineMs = 0;
    }

    const GearDef& def() const { return _def; }
    uint8_t  id()    const { return _def.id; }
    uint8_t  phase() const { return _state; }

    void deploy();
    void retract();
    void stop();

    /// Clear an ERROR state → RETRACTED so deploy/retract are accepted
    /// again.  No-op when not in ERROR.  (GEAR_RESET.)
    void clearError();

    /// Begin stall-endpoint calibration: drive to the retract stop,
    /// then the deploy stop, then home — each leg confirmed by a motor
    /// stall.  Validates both mechanical endpoints.  (GEAR_CALIBRATE.)
    void calibrate();

    /// Abort an in-progress calibration → RETRACTED.  (GEAR_CALIB_CANCEL.)
    void calibrateCancel();

    bool isCalibrating() const { return _state == GearPhase::Calibrating; }

    /// Service tick — call once per main loop with the current time.
    /// Backstop only: the expander's seek timeout is authoritative; this
    /// fires ERROR only if the expander goes silent (no result within the
    /// seek timeout + margin).  Disabled when `timeoutMs == 0`.
    void update(uint32_t nowMs);

    /// Forwarded by the service when BIMOTOR_ENDSTOP_RESULT arrives for
    /// this gear's motor port.  `outcome` is RolePacket::BiMotorSeekOutcome
    /// (0=reached, 1=timeout, 2=aborted).
    void onEndstopResult(uint8_t outcome);

private:
    /// Calibration leg — drives the stall-confirmed endpoint sweep.
    enum class CalibStep : uint8_t {
        None        = 0,
        RetractHome = 1,   ///< retract until stall (find "up" stop)
        DeployEnd   = 2,   ///< deploy until stall (find "down" stop)
        RetractFinal= 3,   ///< retract again → leave gear homed
    };

    void enterPhase(uint8_t newPhase);
    /// Send BIMOTOR_SEEK_ENDSTOP — drive `signedDuty` until the expander
    /// hits a stall or `_def.timeoutMs` elapses, then it brakes locally.
    void commandSeek(int16_t signedDuty);
    void commandMotorBrake();
    /// Arm the hub-side backstop deadline for the current seek (silent-
    /// expander guard).  0 timeout ⇒ disabled.
    void armBackstop(uint32_t nowMs);

    GearDef       _def{};
    uint8_t       _state            = GearPhase::Unconfigured;
    CalibStep     _calibStep        = CalibStep::None;
    uint32_t      _movingDeadlineMs = 0;       ///< millis() at which timeout fires

    SendRoleCmdFn _send     = nullptr;
    void*         _sendCtx  = nullptr;
    BatchFn       _begin    = nullptr;
    BatchFn       _commit   = nullptr;
    PhaseEventFn  _phase    = nullptr;
    void*         _phaseCtx = nullptr;
};

}  // namespace hubfx::effects::gearctrl

#include "gear.ipp"

#endif  // HUBFX_GEAR_H
