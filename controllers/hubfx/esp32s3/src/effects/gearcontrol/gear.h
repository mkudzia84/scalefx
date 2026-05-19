/*
 * gear.h — single-gear state machine for the GearControl effect.
 *
 *   Drives one H-bridge motor and up to 2 status LEDs through
 *   topology.  Same architectural pattern as `LandingLight`:
 *   construction installs callbacks that the owning service binds
 *   to topology's `sendRoleCommand` + batch primitives + phase
 *   broadcast.  No transport / no template-pack knowledge leaks
 *   into the state machine itself.
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

inline constexpr uint8_t kMaxLedsPerGear = 2;

struct GearDef {
    uint8_t  id            = 0;
    char     name[16]      = {};
    PortRef  motor;                            ///< must have BiDcMotor role
    PortRef  leds[kMaxLedsPerGear];            ///< up to 2 LedAnimator roles
    uint8_t  numLeds       = 0;

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

    /// Service tick — call once per main loop with the current time.
    /// Drives the timeout-based ERROR transition if the motor never
    /// reports stall.
    void update(uint32_t nowMs);

    /// Forwarded by the service when MOTOR_STALL_EVENT arrives for
    /// this gear's motor port.
    void onMotorStall();

private:
    void enterPhase(uint8_t newPhase);
    void commandMotor(int16_t signedDuty);
    void commandMotorBrake();
    void commandLedsOn();
    void commandLedsOff();

    GearDef       _def{};
    uint8_t       _state            = GearPhase::Unconfigured;
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
