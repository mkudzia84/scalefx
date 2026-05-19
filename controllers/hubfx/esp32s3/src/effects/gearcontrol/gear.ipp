/*
 * gear.ipp — per-gear state-machine method definitions.
 */

#ifndef HUBFX_GEAR_IPP
#define HUBFX_GEAR_IPP

#include <Arduino.h>          // millis()
#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>

#include "../lightfx/light_event.h"

namespace hubfx::effects::gearctrl {

inline void Gear::deploy() {
    switch (_state) {
        case GearPhase::Unconfigured:
            SFX_LOG_WARN("[gear] deploy %u — not configured", _def.id);
            return;
        case GearPhase::Deploying:
        case GearPhase::Deployed:
            return;  // idempotent
        case GearPhase::Error:
            SFX_LOG_WARN("[gear] deploy %u rejected — gear in ERROR (issue STOP first)", _def.id);
            return;
        default:
            break;
    }
    if (_begin && _sendCtx) _begin(_sendCtx);
    enterPhase(GearPhase::Deploying);
    commandMotor(_def.deployDuty);
    commandLedsOn();
    _movingDeadlineMs = millis() + _def.timeoutMs;
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::retract() {
    switch (_state) {
        case GearPhase::Unconfigured:
            SFX_LOG_WARN("[gear] retract %u — not configured", _def.id);
            return;
        case GearPhase::Retracting:
        case GearPhase::Retracted:
            return;
        case GearPhase::Error:
            SFX_LOG_WARN("[gear] retract %u rejected — gear in ERROR", _def.id);
            return;
        default:
            break;
    }
    if (_begin && _sendCtx) _begin(_sendCtx);
    enterPhase(GearPhase::Retracting);
    commandMotor(_def.retractDuty);
    commandLedsOn();
    _movingDeadlineMs = millis() + _def.timeoutMs;
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::stop() {
    if (_state == GearPhase::Unconfigured) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    commandLedsOff();
    // STOP from Error returns the gear to Retracted (a known-safe
    // baseline) so the operator can retry without needing a separate
    // reset packet.
    enterPhase((_state == GearPhase::Deployed) ? GearPhase::Deployed
                                               : GearPhase::Retracted);
    _movingDeadlineMs = 0;
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::update(uint32_t nowMs) {
    if (_state != GearPhase::Deploying && _state != GearPhase::Retracting) return;
    if (_movingDeadlineMs == 0) return;
    if ((int32_t)(nowMs - _movingDeadlineMs) < 0) return;

    SFX_LOG_WARN("[gear] %u: motor timeout @%u ms — ERROR", _def.id, _def.timeoutMs);
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandMotorBrake();
    commandLedsOff();
    enterPhase(GearPhase::Error);
    _movingDeadlineMs = 0;
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void Gear::onMotorStall() {
    if (_state == GearPhase::Deploying) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandMotorBrake();
        commandLedsOff();
        enterPhase(GearPhase::Deployed);
        _movingDeadlineMs = 0;
        if (_commit && _sendCtx) _commit(_sendCtx);
    } else if (_state == GearPhase::Retracting) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandMotorBrake();
        commandLedsOff();
        enterPhase(GearPhase::Retracted);
        _movingDeadlineMs = 0;
        if (_commit && _sendCtx) _commit(_sendCtx);
    }
    // else: stray stall while idle / in error — ignore.
}

inline void Gear::enterPhase(uint8_t newPhase) {
    _state = newPhase;
    SFX_LOG_DEBUG("[gear] %u → %s", _def.id, GearPhase::getName(newPhase));
    if (_phase) _phase(_phaseCtx, _def.id, newPhase);
}

inline void Gear::commandMotor(int16_t signedDuty) {
    if (!_send) return;
    uint8_t payload[3];
    payload[0] = _def.motor.portIdx;
    SfxWire::putU16LE(&payload[1], static_cast<uint16_t>(signedDuty));
    _send(_sendCtx, _def.motor,
          RolePacket::BIMOTOR_SET_SIGNED, payload, sizeof(payload));
}

inline void Gear::commandMotorBrake() {
    if (!_send) return;
    const uint8_t payload[1] = { _def.motor.portIdx };
    _send(_sendCtx, _def.motor,
          RolePacket::BIMOTOR_BRAKE, payload, sizeof(payload));
}

inline void Gear::commandLedsOn() {
    if (!_send) return;
    using hubfx::effects::lightfx::LightEvent;
    using hubfx::effects::lightfx::serializeQueueLoad;

    LightEvent ev = LightEvent::on(/*brightness=*/100, /*durationMs=*/0);
    for (uint8_t i = 0; i < _def.numLeds; ++i) {
        const PortRef& led = _def.leds[i];
        uint8_t queue[2 + 10];
        size_t qlen = serializeQueueLoad(led.portIdx, &ev, 1,
                                         queue, sizeof(queue));
        if (qlen == 0) continue;
        _send(_sendCtx, led, RolePacket::LED_QUEUE_LOAD, queue, qlen);
        const uint8_t bright[2] = { led.portIdx, 100 };
        _send(_sendCtx, led, RolePacket::LED_SET_BRIGHTNESS,
              bright, sizeof(bright));
        const uint8_t start[1] = { led.portIdx };
        _send(_sendCtx, led, RolePacket::LED_START, start, sizeof(start));
    }
}

inline void Gear::commandLedsOff() {
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

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEAR_IPP
