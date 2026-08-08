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

#include <serial/roles.h>          // RolePacket::kPosNormFull (normalised-pos full scale)
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
    /// ABSOLUTE deploy/retract pulse targets (2.46.0 — live again, as real
    /// µs numbers set by the endpoints widget).  Defaults clamp to the
    /// servo's calibrated ends at the role (0xFFFF → max, 0 → min).
    uint16_t  openUs        = 0xFFFF;
    uint16_t  closeUs       = 0;
    uint8_t   brightnessPct = 100;                   ///< LED brightness when deployed
    /// Soft-start ramp for the LEDs once the servo is fully deployed:
    /// 0 = hard on (legacy), >0 = fade in 0→brightness over this many ms
    /// before holding steady (a [FadeIn, On] LedAnimator queue).
    uint16_t  fadeInMs      = 0;
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

    /// Per-pass tick — travel-timeout backstop.  A dropped
    /// `SERVO_TARGET_REACHED` would otherwise hang DEPLOYING/RETRACTING
    /// forever (servos have no position feedback to the hub), so once the
    /// deploy/retract deadline passes we force-complete the phase
    /// (LEDs on for deploy, RETRACTED for retract) exactly as if every
    /// servo had reported in.  `nowMs` is the EffectClock time latched
    /// once per board pass (Rule 40).
    void update(uint32_t nowMs);

private:
    void enterPhase(uint8_t newPhase);
    /// Settle the in-flight deploy/retract — advance the phase (LEDs on
    /// for deploy, RETRACTED for retract) and disarm the travel backstop.
    /// Shared by the all-servos-arrived path and the timeout backstop.
    void completeTravel();
    void commandAllServos(uint16_t targetUs);  ///< ABSOLUTE µs → SERVO_SET_TARGET (role clamps to calibration)
    void commandLedsOn();
    void commandLedsOff();

    /// Deploy / retract drive each servo to the group's configured ABSOLUTE
    /// open/close µs via `SERVO_SET_TARGET` (2.46.0 — explicit beats
    /// implicit).  The servo's calibration is a pure motion cap: the role
    /// clamps the target, so the 0xFFFF/0 defaults land on the calibrated
    /// ends and a later re-calibration re-caps automatically.  No direction
    /// flag anywhere — which number is bigger IS the direction.
    /// Travel-timeout backstop for a servo move with no position feedback —
    /// mirrors the gear door sequencer's kDoorTravelTimeoutMs.  If every
    /// servo hasn't reported SERVO_TARGET_REACHED within this window the
    /// phase is force-completed (see update()).
    static constexpr uint32_t kTravelTimeoutMs = 4000;
    uint16_t deployTargetUs()  const { return _def.openUs;  }
    uint16_t retractTargetUs() const { return _def.closeUs; }
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
    /// EffectClock deadline (ms) for the in-flight deploy/retract; 0 =
    /// no move in flight (no backstop armed).  Armed on deploy()/retract()
    /// entry, cleared when the phase settles (all servos in OR timeout).
    uint32_t        _travelDeadlineMs = 0;
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
