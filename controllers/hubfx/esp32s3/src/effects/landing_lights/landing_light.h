/*
 * landing_light.h — single landing-light state machine.
 *
 *   A landing light couples one OR MORE servos with one or more LEDs.
 *   Deploy raises every servo to its open position; once EVERY servo
 *   has signalled `SERVO_TARGET_REACHED` (tracked via the per-servo
 *   `_servoArrived` bitset), the LEDs come on.  Retract is the mirror
 *   sequence (LEDs off immediately, every servo retracts; arrival
 *   tracking same as deploy).
 *
 *   Multi-servo support (2026-05-24) is the common case for nose +
 *   wing gear or twin landing doors that swing in unison — operator
 *   doesn't want the LEDs to come on while half the doors are still
 *   moving.  Single-servo configurations still work unchanged: a
 *   1-of-1 bitset masks 0b01 and arrives on the first event.
 *
 *   This class is *single-instance state* — it doesn't know about wire
 *   commands or other landing lights.  The owning `LandingLightService`
 *   feeds it `onServoTargetReached(servoIdx)` whenever a topology role
 *   event for ANY of "its" servos arrives, and dispatches the
 *   resulting LED / servo commands via the shared `TopologyServicePolicy`.
 *
 *   State diagram:
 *
 *     UNCONFIGURED ─configure─→ RETRACTED
 *     RETRACTED   ─deploy()──→ DEPLOYING
 *     DEPLOYING   ─allServosAtTarget→ DEPLOYED   (then LEDs on)
 *     DEPLOYED    ─retract()─→ RETRACTING   (LEDs off immediately)
 *     RETRACTING  ─allServosAtTarget→ RETRACTED
 */

#ifndef HUBFX_LANDING_LIGHT_H
#define HUBFX_LANDING_LIGHT_H

#include <cstdint>

#include "../effect_id.h"
#include "landing_light_protocol.h"

namespace hubfx::effects::landing {

/// Max LEDs grouped under one landing light.  Sized for the typical
/// "main + nose" wing assembly; rev later if a board ever needs more.
inline constexpr uint8_t kMaxLedsPerLanding   = 8;
/// Max servos grouped under one landing light.  Sized for the typical
/// "main + nose gear" setup (1–3 servos); cap at 4 so the
/// `_servoArrived` bitset fits in a u8 with room to spare.
inline constexpr uint8_t kMaxServosPerLanding = 4;

/// Static definition of a landing light — populated once at config /
/// boot time, then never mutated.  The owner field decides which
/// effect family is allowed to call `setState()`.
struct LandingLightDef {
    uint8_t   id            = 0;
    char      name[16]      = {};
    /// Multi-servo support (2026-05-24): all servos in this group
    /// deploy / retract together; the state machine waits for ALL of
    /// them to reach the target before bringing the LEDs on.  Each
    /// must have the ServoActuator role attached.
    PortRef   servos[kMaxServosPerLanding];
    uint8_t   numServos     = 0;
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
    /// arrives for ANY of this landing light's servos.  `servoIdx` is
    /// the slot within `def.servos[]` so we can mark off arrivals.
    /// Drives the DEPLOYING→DEPLOYED and RETRACTING→RETRACTED edges
    /// only AFTER every servo in the group has reported in.
    void onServoTargetReached(uint8_t servoIdx);

private:
    void enterPhase(uint8_t newPhase);
    void commandAllServos(uint16_t targetUs);
    void commandLedsOn();
    void commandLedsOff();
    /// All-arrived mask = (1 << numServos) - 1.  Computed once per
    /// deploy/retract to avoid recomputing on every event.
    uint8_t allServosMask() const {
        return _def.numServos >= 8 ? 0xFF
             : static_cast<uint8_t>((1u << _def.numServos) - 1);
    }

    LandingLightDef _def{};
    uint8_t         _state    = LandingLightPhase::Unconfigured;
    /// Bitset — bit N set when servo N has reported reaching its
    /// target since the last deploy() / retract().  Cleared by both.
    /// LEDs come on (deploy) / phase advances (retract) only when
    /// `_servoArrived == allServosMask()`.
    uint8_t         _servoArrived = 0;
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
