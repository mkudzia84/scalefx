/*
 * landing_light.ipp — landing-light state-machine method definitions.
 *
 *   Included at the bottom of `landing_light.h` so the file builds as
 *   part of whichever translation unit pulls the header (no separate
 *   .cpp / build_src_filter wrangling).
 */

#ifndef HUBFX_LANDING_LIGHT_IPP
#define HUBFX_LANDING_LIGHT_IPP

#include <serial/roles.h>
#include <serial/wire.h>      // SfxWire::putU16LE
#include <serial/diag_log.h>

#include "../lightfx/light_event.h"

namespace hubfx::effects::landing {

inline void LandingLight::deploy() {
    switch (_state) {
        case LandingLightPhase::Unconfigured:
            SFX_LOG_WARN("[ll] deploy %u — not configured", _def.id);
            return;
        case LandingLightPhase::Deploying:
        case LandingLightPhase::Deployed:
            return;  // idempotent
        default:
            break;
    }
    // Reset the arrival bitset BEFORE dispatching so a fast servo
    // can't report in before we're tracking — once we enter DEPLOYING,
    // every onServoTargetReached() ORs into a known-zero starting mask.
    _servoArrived = 0;
    // Single batch — all `numServos` SERVO_SET_TARGET packets ship
    // together so the servos start moving in lock-step.
    if (_begin && _sendCtx) _begin(_sendCtx);
    enterPhase(LandingLightPhase::Deploying);
    commandAllServos(_def.openUs);
    if (_commit && _sendCtx) _commit(_sendCtx);
    // Edge case: zero servos configured (LEDs-only landing light) —
    // the arrival mask is already 0, allServosMask() is 0, so we're
    // immediately "all arrived" and skip straight to LEDs on.
    if (allServosMask() == 0) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        enterPhase(LandingLightPhase::Deployed);
        commandLedsOn();
        if (_commit && _sendCtx) _commit(_sendCtx);
    }
}

inline void LandingLight::retract() {
    switch (_state) {
        case LandingLightPhase::Unconfigured:
            SFX_LOG_WARN("[ll] retract %u — not configured", _def.id);
            return;
        case LandingLightPhase::Retracting:
        case LandingLightPhase::Retracted:
            return;  // idempotent
        default:
            break;
    }
    _servoArrived = 0;
    // Per the deploy-then-retract spec: LEDs go off IMMEDIATELY on
    // retract, then all servos retract.  Both fan into one batch so
    // the LEDs don't briefly hang on while servo commands are in flight.
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandLedsOff();
    enterPhase(LandingLightPhase::Retracting);
    commandAllServos(_def.closeUs);
    if (_commit && _sendCtx) _commit(_sendCtx);
    // LEDs-only edge case mirror — no servos to wait on, go straight
    // to RETRACTED so the operator's UI doesn't show a stuck phase.
    if (allServosMask() == 0) {
        enterPhase(LandingLightPhase::Retracted);
    }
}

inline void LandingLight::onServoTargetReached(uint8_t servoIdx) {
    if (servoIdx >= _def.numServos) return;          // stray event
    _servoArrived |= static_cast<uint8_t>(1u << servoIdx);
    const uint8_t mask = allServosMask();
    if ((_servoArrived & mask) != mask) {
        // Some servos still in motion — wait for them.  Logged at
        // DEBUG so a slow servo's lagging arrival shows up in traces
        // without spamming INFO.
        SFX_LOG_DEBUG("[ll] %u: servo %u arrived (mask 0x%02x/0x%02x)",
                      _def.id, (unsigned)servoIdx,
                      (unsigned)_servoArrived, (unsigned)mask);
        return;
    }
    // All servos in.  Advance the phase + (for deploy) fan the LEDs on.
    switch (_state) {
        case LandingLightPhase::Deploying:
            if (_begin && _sendCtx) _begin(_sendCtx);
            enterPhase(LandingLightPhase::Deployed);
            commandLedsOn();
            if (_commit && _sendCtx) _commit(_sendCtx);
            return;
        case LandingLightPhase::Retracting:
            enterPhase(LandingLightPhase::Retracted);
            return;
        default:
            return;  // stray event — ignore
    }
}

inline void LandingLight::enterPhase(uint8_t newPhase) {
    _state = newPhase;
    SFX_LOG_DEBUG("[ll] %u → %s", _def.id, LandingLightPhase::getName(newPhase));
    if (_phase) _phase(_phaseCtx, _def.id, newPhase);
}

inline void LandingLight::commandAllServos(uint16_t targetUs) {
    if (!_send) return;
    for (uint8_t i = 0; i < _def.numServos; ++i) {
        const PortRef& s = _def.servos[i];
        uint8_t payload[3];
        payload[0] = s.portIdx;
        SfxWire::putU16LE(&payload[1], targetUs);
        if (!_send(_sendCtx, s,
                   RolePacket::SERVO_SET_TARGET, payload, sizeof(payload))) {
            SFX_LOG_WARN("[ll] %u: servo[%u] SET_TARGET failed", _def.id, (unsigned)i);
        }
    }
}

inline void LandingLight::commandLedsOn() {
    if (!_send) return;
    using hubfx::effects::lightfx::LightEvent;
    using hubfx::effects::lightfx::serializeQueueLoad;

    // Build a one-event "on, infinite duration" queue + scale via
    // LED_SET_BRIGHTNESS.  The actual hold-bright behaviour is the
    // LedAnimator role's job on the target board.
    LightEvent ev = LightEvent::on(/*brightness=*/100, /*durationMs=*/0);
    for (uint8_t i = 0; i < _def.numLeds; ++i) {
        const PortRef& led = _def.leds[i];

        uint8_t queue[2 + 10];  // 1 event
        size_t qlen = serializeQueueLoad(led.portIdx, &ev, 1,
                                         queue, sizeof(queue));
        if (qlen == 0) continue;
        if (!_send(_sendCtx, led, RolePacket::LED_QUEUE_LOAD, queue, qlen)) {
            SFX_LOG_WARN("[ll] %u: LED_QUEUE_LOAD failed on %s:%u",
                         _def.id, led.guid[0] ? led.guid : "hub",
                         (unsigned)led.portIdx);
            continue;
        }
        const uint8_t bright[2] = { led.portIdx, _def.brightnessPct };
        _send(_sendCtx, led, RolePacket::LED_SET_BRIGHTNESS,
              bright, sizeof(bright));
        const uint8_t start[1] = { led.portIdx };
        _send(_sendCtx, led, RolePacket::LED_START, start, sizeof(start));
    }
}

inline void LandingLight::commandLedsOff() {
    if (!_send) return;
    for (uint8_t i = 0; i < _def.numLeds; ++i) {
        const PortRef& led = _def.leds[i];
        const uint8_t stop[1]   = { led.portIdx };
        const uint8_t bright[2] = { led.portIdx, 0 };
        _send(_sendCtx, led, RolePacket::LED_STOP, stop, sizeof(stop));
        _send(_sendCtx, led, RolePacket::LED_SET_BRIGHTNESS,
              bright, sizeof(bright));
    }
}

}  // namespace hubfx::effects::landing

#endif  // HUBFX_LANDING_LIGHT_IPP
