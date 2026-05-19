/*
 * landing_light.h — single landing-light state machine.
 *
 *   A landing light couples one servo with one or more LEDs.  Deploy
 *   raises the servo to its open position and, when the servo signals
 *   `SERVO_TARGET_REACHED`, brings the LEDs on; retract is the mirror
 *   sequence (LEDs off immediately, servo to closed position).
 *
 *   This class is *single-instance state* — it doesn't know about wire
 *   commands or other landing lights.  The owning `LandingLightService`
 *   feeds it `onServoTargetReached(...)` whenever a topology role
 *   event for "its" servo arrives, and dispatches the resulting LED /
 *   servo commands via the shared `TopologyServicePolicy`.
 *
 *   State diagram (mirrors the legacy lightfx/pico/landing_light.h):
 *
 *     UNCONFIGURED ─configure─→ RETRACTED
 *     RETRACTED   ─deploy()──→ DEPLOYING
 *     DEPLOYING   ─servoAtTarget→ DEPLOYED   (then LEDs on)
 *     DEPLOYED    ─retract()─→ RETRACTING   (LEDs off immediately)
 *     RETRACTING  ─servoAtTarget→ RETRACTED
 */

#ifndef HUBFX_LANDING_LIGHT_H
#define HUBFX_LANDING_LIGHT_H

#include <cstdint>

#include "../effect_id.h"
#include "landing_light_protocol.h"

namespace hubfx::effects::landing {

/// Max LEDs grouped under one landing light.  Sized for the typical
/// "main + nose" wing assembly; rev later if a board ever needs more.
inline constexpr uint8_t kMaxLedsPerLanding = 8;

/// Static definition of a landing light — populated once at config /
/// boot time, then never mutated.  The owner field decides which
/// effect family is allowed to call `setState()`.
struct LandingLightDef {
    uint8_t   id            = 0;
    char      name[16]      = {};
    PortRef   servo;                                ///< must have ServoActuator role
    PortRef   leds[kMaxLedsPerLanding];             ///< each must have LedAnimator role
    uint8_t   numLeds       = 0;
    uint16_t  openUs        = 1900;
    uint16_t  closeUs       = 1100;
    uint8_t   brightnessPct = 100;                   ///< LED brightness when deployed
    EffectId  owner         = EffectId::LightFx;     ///< trigger-source owner
};

/// Per-instance state machine.  Constructed once, then driven by
/// `deploy()` / `retract()` from the owner effect and ticked by
/// `onServoTargetReached()` from the service when a matching
/// topology role event arrives.
class LandingLight {
public:
    LandingLight() = default;

    /// Type-erased dispatcher hooks — the service injects these so
    /// the state machine can ship commands and bracket them in a
    /// topology-level batch without depending on the concrete
    /// `TopologyServicePolicyT<TExpander>` template.
    using SendRoleCmdFn = bool (*)(void* ctx, const PortRef& addr,
                                   uint8_t innerType,
                                   const uint8_t* payload, size_t len);
    using BatchFn       = void (*)(void* ctx);
    /// Phase-change callback fired AFTER the new phase is stored.
    using PhaseEventFn  = void (*)(void* ctx, uint8_t id, uint8_t newPhase);

    void configure(const LandingLightDef& def,
                   SendRoleCmdFn sendFn,  void* sendCtx,
                   BatchFn       beginFn, BatchFn commitFn,
                   PhaseEventFn  phaseFn = nullptr, void* phaseCtx = nullptr) {
        _def      = def;
        _send     = sendFn;
        _sendCtx  = sendCtx;
        _begin    = beginFn;
        _commit   = commitFn;
        _phase    = phaseFn;
        _phaseCtx = phaseCtx;
        _state    = LandingLightPhase::Retracted;
    }

    const LandingLightDef& def() const { return _def; }
    uint8_t  id()    const { return _def.id; }
    uint8_t  phase() const { return _state; }
    EffectId owner() const { return _def.owner; }

    /// Begin the deploy sequence (servo → open, LEDs on at target).
    /// No-op when already deployed / deploying.
    void deploy();

    /// Begin the retract sequence (LEDs off, servo → closed).
    /// No-op when already retracted / retracting.
    void retract();

    /// Idempotent state setter — calls `deploy()` or `retract()`.
    void setState(uint8_t state) {
        if (state == LandingLightState::On)  deploy();
        else                                  retract();
    }

    /// Forwarded by the service when a `SERVO_TARGET_REACHED` event
    /// arrives for this landing light's servo.  Drives the
    /// DEPLOYING→DEPLOYED and RETRACTING→RETRACTED edges.
    void onServoTargetReached();

private:
    void enterPhase(uint8_t newPhase);
    void commandServo(uint16_t targetUs);
    void commandLedsOn();
    void commandLedsOff();

    LandingLightDef _def{};
    uint8_t         _state    = LandingLightPhase::Unconfigured;
    SendRoleCmdFn   _send     = nullptr;
    void*           _sendCtx  = nullptr;
    BatchFn         _begin    = nullptr;
    BatchFn         _commit   = nullptr;
    PhaseEventFn    _phase    = nullptr;
    void*           _phaseCtx = nullptr;
};

}  // namespace hubfx::effects::landing

#include "landing_light.ipp"

#endif  // HUBFX_LANDING_LIGHT_H
