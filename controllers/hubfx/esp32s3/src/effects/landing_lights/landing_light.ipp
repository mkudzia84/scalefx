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
    // Single-command batch (just SERVO_SET_TARGET) — still useful so
    // that if we're called from inside a larger batch (e.g. program
    // switch), this one falls into the outer batch.
    if (_begin && _sendCtx) _begin(_sendCtx);
    enterPhase(LandingLightPhase::Deploying);
    commandServo(_def.openUs);
    if (_commit && _sendCtx) _commit(_sendCtx);
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
    // Per the deploy-then-retract spec: LEDs go off IMMEDIATELY on
    // retract, then the servo retracts.  All three subbursts go in
    // one batch so the LEDs don't briefly hang on while the servo
    // command is in flight.
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandLedsOff();
    enterPhase(LandingLightPhase::Retracting);
    commandServo(_def.closeUs);
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void LandingLight::onServoTargetReached() {
    switch (_state) {
        case LandingLightPhase::Deploying:
            // Servo arrived at the open position — bring up the LEDs.
            //   commandLedsOn fans 3 commands per LED × N — batch them.
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

inline void LandingLight::commandServo(uint16_t targetUs) {
    if (!_send) return;
    uint8_t payload[3];
    payload[0] = _def.servo.portIdx;
    SfxWire::putU16LE(&payload[1], targetUs);
    if (!_send(_sendCtx, _def.servo,
               RolePacket::SERVO_SET_TARGET, payload, sizeof(payload))) {
        SFX_LOG_WARN("[ll] %u: servo SET_TARGET failed", _def.id);
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
