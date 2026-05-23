/*
 * gun_unit.ipp — single-gun state-machine method definitions.
 */

#ifndef HUBFX_GUN_UNIT_IPP
#define HUBFX_GUN_UNIT_IPP

#include <Arduino.h>
#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw millis()

#include "../lightfx/light_event.h"

namespace hubfx::effects::gunfx {

inline void GunUnit::fireOnce() {
    if (_begin && _sendCtx) _begin(_sendCtx);
    doShot();
    if (_commit && _sendCtx) _commit(_sendCtx);
}

inline void GunUnit::startFiring(uint16_t rpm) {
    _shotIntervalMs = (rpm > 0) ? (60000u / rpm) : _def.defaultIntervalMs;
    if (_shotIntervalMs < 20) _shotIntervalMs = 20;     // cap to 3000 RPM
    _nextShotMs = sfx_core::EffectClock::instance().nowMs();      // first shot immediately
    _firing = true;
    SFX_LOG_INFO("[gun] %u: auto-fire @%u ms / shot", _def.id, (unsigned)_shotIntervalMs);
}

inline void GunUnit::stopFiring() {
    _firing = false;
    SFX_LOG_INFO("[gun] %u: stop firing", _def.id);
}

inline void GunUnit::armSmoke(bool armed) {
    if (armed == _smokeArmed) return;
    _smokeArmed = armed;
    if (_def.smokeHeater.portKind == 0) {
        SFX_LOG_WARN("[gun] %u: smoke arm requested but no heater configured", _def.id);
        return;
    }
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandHeater(armed);
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gun] %u: smoke %s", _def.id, armed ? "armed" : "off");
}

inline void GunUnit::update(uint32_t nowMs) {
    // Auto-fire schedule.
    if (_firing && (int32_t)(nowMs - _nextShotMs) >= 0) {
        fireOnce();
        _nextShotMs = nowMs + _shotIntervalMs;
    }
    // Recoil return.
    if (_recoilReturnAtMs && (int32_t)(nowMs - _recoilReturnAtMs) >= 0) {
        _recoilReturnAtMs = 0;
        if (_def.recoilServo.portKind != 0) {
            if (_begin && _sendCtx) _begin(_sendCtx);
            uint8_t payload[3];
            payload[0] = _def.recoilServo.portIdx;
            SfxWire::putU16LE(&payload[1], _def.recoilCenterUs);
            _send(_sendCtx, _def.recoilServo,
                  RolePacket::SERVO_SET_TARGET, payload, sizeof(payload));
            if (_commit && _sendCtx) _commit(_sendCtx);
        }
    }
}

inline void GunUnit::onTriggerInput(uint16_t pulseUs, bool valid) {
    if (!valid) return;
    if (_def.trigger.portKind == 0) return;

    // 50 µs hysteresis around the threshold.
    const uint16_t hi = _def.triggerThresholdUs + 50;
    const uint16_t lo = (_def.triggerThresholdUs > 50)
                          ? (_def.triggerThresholdUs - 50) : 0;
    const bool nowHeld = _triggerHeld ? (pulseUs >= lo) : (pulseUs >= hi);
    if (nowHeld == _triggerHeld) return;
    _triggerHeld = nowHeld;

    if (nowHeld) startFiring(0);            // 0 → use defaultIntervalMs
    else         stopFiring();
}

// ─── Per-shot atomic burst ──────────────────────────────────────────

inline void GunUnit::doShot() {
    commandFlash();
    commandRecoilJerk();
    if (_def.recoilHoldMs > 0) {
        _recoilReturnAtMs = sfx_core::EffectClock::instance().nowMs() + _def.recoilHoldMs;
    }
    if (_shot && _shotCtx) {
        _shot(_shotCtx, _def.id,
              _def.fireSoundPath[0] ? _def.fireSoundPath : nullptr,
              _def.audioChannel, _def.outputMask);
    }
}

inline void GunUnit::commandFlash() {
    if (!_send) return;
    using hubfx::effects::lightfx::LightEvent;
    using hubfx::effects::lightfx::serializeQueueLoad;

    // Single short ON event, finite duration → role auto-stops at end.
    LightEvent ev = LightEvent::on(_def.flashBrightness, _def.flashDurationMs);
    uint8_t queue[2 + 10];
    size_t qlen = serializeQueueLoad(_def.muzzleFlash.portIdx, &ev, 1,
                                     queue, sizeof(queue));
    if (qlen == 0) return;
    _send(_sendCtx, _def.muzzleFlash,
          RolePacket::LED_QUEUE_LOAD, queue, qlen);
    const uint8_t bright[2] = { _def.muzzleFlash.portIdx, 100 };
    _send(_sendCtx, _def.muzzleFlash,
          RolePacket::LED_SET_BRIGHTNESS, bright, sizeof(bright));
    const uint8_t start[1] = { _def.muzzleFlash.portIdx };
    _send(_sendCtx, _def.muzzleFlash,
          RolePacket::LED_START, start, sizeof(start));
}

inline void GunUnit::commandRecoilJerk() {
    if (!_send) return;
    if (_def.recoilServo.portKind == 0) return;
    // Move by ±jerkUs from center.  Always jerk in one direction
    // (positive) for now — variance + direction randomization is a
    // v2 feature.
    const uint16_t target = _def.recoilCenterUs + _def.recoilJerkUs;
    uint8_t payload[3];
    payload[0] = _def.recoilServo.portIdx;
    SfxWire::putU16LE(&payload[1], target);
    _send(_sendCtx, _def.recoilServo,
          RolePacket::SERVO_SET_TARGET, payload, sizeof(payload));
}

inline void GunUnit::commandHeater(bool on) {
    if (!_send) return;
    if (_def.smokeHeater.portKind == 0) return;
    const int16_t target = on ? _def.smokeTargetCx10 : 0;
    uint8_t payload[3];
    payload[0] = _def.smokeHeater.portIdx;
    SfxWire::putU16LE(&payload[1], static_cast<uint16_t>(target));
    _send(_sendCtx, _def.smokeHeater,
          RolePacket::HEATER_SET_TARGET, payload, sizeof(payload));
}

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUN_UNIT_IPP
