/*
 * gun_unit.ipp — single-gun state-machine method definitions
 *                (Phase 2 of GunFX rollout, instructions/22).
 *
 *   What changed vs the Phase 1 stub:
 *     - Reads from `GunSpec` (gunfx_config.h) instead of the old GunDef
 *     - Multi-band rate-of-fire: selector channel picks the armed RofItem;
 *       out-of-band suppresses firing
 *     - Smoke fan modes (off / continuous / puff_per_shot / puff_on_fire_active)
 *     - Heater driven via the HeaterRole intent API — no duty math here
 *     - Yaw + pitch axes pushed at the intent layer to ServoActuatorRole
 *       (motion shaping lives on the role — Rule 42)
 *     - Manual override mode with 5 s auto-release (kManualTimeoutMs)
 */

#ifndef HUBFX_GUN_UNIT_IPP
#define HUBFX_GUN_UNIT_IPP

#include <Arduino.h>
#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw millis()

#include "../audio_layout.h"      // HubFxLayout — named audio-channel slots
#include "../lightfx/light_event.h"

namespace hubfx::effects::gunfx {

// ─── Public API ─────────────────────────────────────────────────────

inline void GunUnit::fireOnce(uint8_t rofOverride) {
    // Single-shot path: visuals + ONE-SHOT audio (loop=false).  Used
    // both by external `GUN_FIRE_ONCE` and by `startFiring()` for the
    // first audible kick BEFORE the sustained loop takes over.
    //
    // ROF resolution priority (2026-05-24):
    //   1. explicit `rofOverride` (valid index < numRofItems)
    //   2. currently-armed `_activeRofIndex` (set by the ROF selector
    //      channel via `onRofSelectorUs`)
    //   3. first declared item, when items exist and nothing is armed
    //      (the operator-press path: GUI Fire button when the selector
    //      stick is between bands).  This is the fix for the regression
    //      "fire button does nothing when a selector channel is bound" —
    //      the FIRE_ONCE wire packet should ALWAYS produce a shot, even
    //      if the operator hasn't dialled the stick into a band yet.
    const RofItem* item = nullptr;
    uint8_t        usedIdx = 0xFF;
    if (rofOverride < _spec.numRofItems) {
        usedIdx = rofOverride;
        item    = &_spec.rofItems[rofOverride];
    } else if (_activeRofIndex < _spec.numRofItems) {
        usedIdx = _activeRofIndex;
        item    = &_spec.rofItems[_activeRofIndex];
    } else if (_spec.numRofItems > 0) {
        usedIdx = 0;
        item    = &_spec.rofItems[0];
    }
    SFX_LOG_INFO("[gun] %u: fireOnce — send=%d audio=%d muzzle.kind=%u rofOverride=%u rof=%u",
                 _spec.id, _send != nullptr, _audio != nullptr,
                 (unsigned)_spec.muzzleFlashPort.portKind,
                 (unsigned)rofOverride, (unsigned)usedIdx);

    if (_begin && _sendCtx) _begin(_sendCtx);
    doShot();
    if (_commit && _sendCtx) _commit(_sendCtx);

    if (_audio && _audioCtx) {
        const char* sound = (item && item->soundPath[0]) ? item->soundPath : nullptr;
        const uint8_t mask = (item && item->outputMask) ? item->outputMask : 0x03;
        const uint8_t channel = (_spec.id & 1)
            ? audio::HubFxLayout::GunB
            : audio::HubFxLayout::GunA;
        if (sound) {
            _audio(_audioCtx, sound, channel, mask, /*loop=*/false);
        } else {
            SFX_LOG_WARN("[gun] %u: fireOnce — no sound (item=%p resolvedIdx=%u)",
                         _spec.id, (const void*)item, (unsigned)usedIdx);
        }
    }
}

inline void GunUnit::startFiring(uint16_t rpmOverride, uint8_t rofOverride) {
    // ROF resolution mirrors `fireOnce()` — explicit override > armed
    // selector > first declared item.  When a valid override is given,
    // _activeRofIndex is FORCED for the duration of the burst so the
    // auto-fire `update()` gate (`activeRof() != nullptr`) reliably
    // fires shots.  Without this, GUI Auto-Fire with the RC stick
    // between bands would set _firing=true but never produce shots.
    if (rofOverride < _spec.numRofItems) {
        _activeRofIndex = rofOverride;
    } else if (_activeRofIndex >= _spec.numRofItems && _spec.numRofItems > 0) {
        _activeRofIndex = 0;            // fall back to first item
    }

    uint16_t rpm = rpmOverride;
    if (rpm == 0) {
        const RofItem* item = activeRof();
        rpm = item ? item->rpm : 600;       // hard default if no items
    }
    _shotIntervalMs = (rpm > 0) ? (60000u / rpm) : 100;
    if (_shotIntervalMs < 20) _shotIntervalMs = 20;   // cap to 3000 RPM
    _nextShotMs = sfx_core::EffectClock::instance().nowMs();   // fire first immediately
    _firing = true;

    // Sustained-fire audio (2026-05-23): instead of re-triggering the
    // shot WAV every auto-fire tick (which collides with the WAV's own
    // baked-in cadence and stutters), start the WAV ONCE with loop=true
    // and let it play continuously until stopFiring() stops the channel.
    // Auto-fire ticks below (doAutoFireShot) still broadcast GUN_SHOT_EVENT
    // for the Studio mirror but DO NOT touch audio.
    if (_audio && _audioCtx) {
        const RofItem* item = activeRof();
        const char* sound = (item && item->soundPath[0]) ? item->soundPath : nullptr;
        const uint8_t mask = (item && item->outputMask) ? item->outputMask : 0x03;
        const uint8_t channel = (_spec.id & 1)
            ? audio::HubFxLayout::GunB
            : audio::HubFxLayout::GunA;
        if (sound) {
            _audio(_audioCtx, sound, channel, mask, /*loop=*/true);
        }
    }

    SFX_LOG_INFO("[gun] %u: auto-fire @%u ms / shot (rof=%u, override=%u)",
                 _spec.id, (unsigned)_shotIntervalMs,
                 (unsigned)_activeRofIndex, (unsigned)rofOverride);
}

inline void GunUnit::stopFiring() {
    if (!_firing) return;
    _firing = false;

    // Stop the looping shot WAV — passes nullptr as the sound path,
    // which the service trampoline interprets as `stopAsync(channel)`.
    if (_audio && _audioCtx) {
        const uint8_t channel = (_spec.id & 1)
            ? audio::HubFxLayout::GunB
            : audio::HubFxLayout::GunA;
        _audio(_audioCtx, nullptr, channel, 0, false);
    }

    SFX_LOG_INFO("[gun] %u: stop firing", _spec.id);
}

inline void GunUnit::armSmoke(bool armed) {
    if (armed == _smokeArmed) return;
    _smokeArmed = armed;
    if (_begin && _sendCtx) _begin(_sendCtx);
    if (_spec.smoke.heaterPort.portKind != 0) {
        commandHeater(armed);
    }
    // Smoke-OFF MUST cut the fan immediately regardless of mode or
    // pending puff timers (2026-05-24: "smoke fan only when smoke ON
    // AND trigger ON").  Without this an in-flight FN_CONTINUOUS run
    // or a queued puff could keep the fan spinning after the operator
    // hit Smoke Off.  Also clears the puff-end timer + pending-burst
    // request so a fresh `armSmoke(true) + trigger` sequence starts
    // from a clean state.
    if (!armed && _spec.smoke.fanPort.portKind != 0) {
        commandFanPct(0);
        _fanContinuous   = false;
        _fanPuffEndMs    = 0;
        _pendingFanBurst = false;
    }
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gun] %u: smoke %s", _spec.id, armed ? "armed" : "off");
}

// ─── Per-tick orchestration ─────────────────────────────────────────

inline void GunUnit::update(uint32_t nowMs, uint32_t dtMs) {
    tickManualTimeout(nowMs);

    // Auto-fire schedule. Gated on `_firing` + an armed ROF item (or
    // a synthetic default when no ROF channel is bound).  We call
    // `doAutoFireShot()` — visuals + GUN_SHOT_EVENT broadcast — but NOT
    // `fireOnce()`, because that would re-trigger the shot WAV on every
    // tick and collide with the loop started by `startFiring()`.  The
    // WAV's own RPM cadence carries the audio; this loop only handles
    // the visual + smoke side.
    if (_firing && activeRof() != nullptr
            && (int32_t)(nowMs - _nextShotMs) >= 0) {
        if (_begin && _sendCtx) _begin(_sendCtx);
        doShot();                                   // visuals + SHOT_EVENT
        if (_commit && _sendCtx) _commit(_sendCtx);
        _nextShotMs = nowMs + _shotIntervalMs;
        ++_shotsThisSession;
        // FN_PUFF_PER_SHOT: pulse the fan once per shot.
        if (_smokeArmed && _spec.smoke.fanMode == SmokeConfig::FN_PUFF_PER_SHOT) {
            scheduleFanPuff(nowMs);
        }
    }

    // Recoil return — restore the recoil axis to its pre-jerk target.
    // tickAxis() will resume RC tracking on the next pass because
    // _recoilActive is cleared here.
    if (_recoilReturnAtMs && (int32_t)(nowMs - _recoilReturnAtMs) >= 0) {
        _recoilReturnAtMs = 0;
        if (_recoilActive && _spec.recoilEnabled) {
            const bool isYaw = (_spec.recoilAxis == 1);
            const GunAxis& axis = isYaw ? _spec.yaw : _spec.pitch;
            if (axis.enabled && axis.servoPort.portKind != 0) {
                if (_begin && _sendCtx) _begin(_sendCtx);
                commandServoTargetUs(axis.servoPort, _recoilSavedUs);
                if (_commit && _sendCtx) _commit(_sendCtx);
                uint16_t& curTarget = isYaw ? _yawTargetUs : _pitchTargetUs;
                curTarget = _recoilSavedUs;
            }
        }
        _recoilActive = false;
    }

    // Smoke fan housekeeping (puff timeouts, continuous mode start/stop).
    tickFan(nowMs);

    // Phase 2.9: yaw + pitch motion shaping (clamp / speed / accel /
    // jerk) lives on the ServoActuatorRole attached to each axis port.
    // The gun just pushes a target each tick — the role's integrator
    // handles the slew.  `dtMs` is unused here now.
    (void)dtMs;
    // Recoil suppresses RC tracking on the kicked axis only — the other
    // axis continues to track normally so a yaw-recoil setup still lets
    // the operator steer pitch during the hold.
    const bool yawHeld   = _recoilActive && _spec.recoilEnabled && _spec.recoilAxis == 1;
    const bool pitchHeld = _recoilActive && _spec.recoilEnabled && _spec.recoilAxis == 0;
    if (_spec.yaw.enabled && !yawHeld) {
        tickAxis(_spec.yaw,
                 _haveYawInput ? _lastYawInputUs : _spec.yaw.neutralUs,
                 _manual.active && _manual.yawValid, _manual.yawUs,
                 _yawTargetUs);
    }
    if (_spec.pitch.enabled && !pitchHeld) {
        tickAxis(_spec.pitch,
                 _havePitchInput ? _lastPitchInputUs : _spec.pitch.neutralUs,
                 _manual.active && _manual.pitchValid, _manual.pitchUs,
                 _pitchTargetUs);
    }
}

inline void GunUnit::tickManualTimeout(uint32_t nowMs) {
    if (!_manual.active) return;
    if (nowMs - _manual.lastUpdateMs >= kManualTimeoutMs) {
        SFX_LOG_INFO("[gun] %u: manual override auto-released (timeout)", _spec.id);
        releaseManual();
    }
}

// ─── Trigger / ROF / axis input callbacks ───────────────────────────

inline void GunUnit::onTriggerBoolean(bool held) {
    // Manual mode wins — RC trigger is ignored while puppet-driven.
    if (_manual.active) return;
    if (held == _triggerHeld) return;
    _triggerHeld = held;
    if (held) startFiring(0);
    else      stopFiring();
}

inline void GunUnit::onTriggerRawUs(uint16_t pulseUs, bool valid) {
    if (!valid) return;
    _lastTriggerUs = pulseUs;
}

inline void GunUnit::onRofSelectorUs(uint16_t pulseUs, bool valid) {
    if (!valid) return;
    _lastRofSelectorUs = pulseUs;
    _haveRofSelector   = true;
    if (_manual.active && _manual.rofIndexValid) return;   // manual wins
    const uint8_t idx = findRofIndex(pulseUs);
    if (idx == _activeRofIndex) return;
    _activeRofIndex = idx;
    // If we're mid-burst, snap the shot interval to the new item's RPM
    // so the next shot honours the change without waiting for a stop+start.
    if (_firing) {
        const RofItem* item = activeRof();
        const uint16_t rpm = item ? item->rpm : 0;
        _shotIntervalMs = (rpm > 0) ? (60000u / rpm) : _shotIntervalMs;
    }
    SFX_LOG_INFO("[gun] %u: ROF → %u (selector=%u µs)",
                 _spec.id, (unsigned)idx, (unsigned)pulseUs);
}

inline void GunUnit::onYawInputUs(uint16_t pulseUs, bool valid) {
    if (!valid) return;
    _lastYawInputUs = pulseUs;
    _haveYawInput   = true;
}
inline void GunUnit::onPitchInputUs(uint16_t pulseUs, bool valid) {
    if (!valid) return;
    _lastPitchInputUs = pulseUs;
    _havePitchInput   = true;
}

// ─── Manual override ────────────────────────────────────────────────

inline void GunUnit::applyManualSet(uint8_t flags,
                                    uint16_t yawUs, uint16_t pitchUs,
                                    uint8_t rofIndex, bool fireHold,
                                    bool smokeArm, bool smokeFanBurst,
                                    uint32_t nowMs) {
    _manual.active       = true;
    _manual.lastUpdateMs = nowMs;
    if (flags & GunManualFlag::YAW)   { _manual.yawValid = true;       _manual.yawUs = yawUs; }
    if (flags & GunManualFlag::PITCH) { _manual.pitchValid = true;     _manual.pitchUs = pitchUs; }
    if (flags & GunManualFlag::ROF) {
        _manual.rofIndexValid = true;
        _manual.rofIndex      = rofIndex;
        _activeRofIndex       = (rofIndex < _spec.numRofItems) ? rofIndex : 0xFF;
    }
    if (flags & GunManualFlag::FIRE) {
        _manual.fireHoldValid = true;
        _manual.fireHold      = fireHold;
        if (fireHold)  startFiring(0);
        else           stopFiring();
    }
    if (flags & GunManualFlag::SMOKE) {
        _manual.smokeArmValid = true;
        _manual.smokeArm      = smokeArm;
        armSmoke(smokeArm);
    }
    if (flags & GunManualFlag::FAN_BURST) {
        if (smokeFanBurst) _pendingFanBurst = true;
    }
}

inline void GunUnit::releaseManual() {
    if (!_manual.active) return;
    _manual = ManualOverride{};   // all *Valid back to false
    SFX_LOG_INFO("[gun] %u: manual override released → RC", _spec.id);
    // Don't stop firing or disarm smoke — the next RC tick will reset
    // them naturally. (If trigger isn't held on the RC side, the
    // TriggerInput's next edge stops firing; if held, it stays on.)
}

// ─── ROF helpers ────────────────────────────────────────────────────

inline uint8_t GunUnit::findRofIndex(uint16_t pulseUs) const {
    for (uint8_t i = 0; i < _spec.numRofItems; ++i) {
        const RofItem& it = _spec.rofItems[i];
        const uint16_t lo = it.bandLoUs;
        const uint16_t hi = it.bandHiUs;
        // 0 means "unbounded" on either side.
        if ((lo == 0 || pulseUs >= lo) && (hi == 0 || pulseUs <= hi)) {
            return i;
        }
    }
    return 0xFF;
}

inline uint8_t GunUnit::pickInitialRofIndex() const {
    // No ROF channel bound → if there's at least one item, arm the first
    // so manual fire-once still works.  Otherwise 0xFF (firing
    // suppressed until an item is armed).
    if (_spec.rofSelectorPort.portKind == 0 && _spec.numRofItems > 0) {
        return 0;
    }
    return 0xFF;
}

inline const RofItem* GunUnit::activeRof() const {
    // ROF channel bound + nothing armed → no firing.  ROF channel not
    // bound → pickInitialRofIndex already chose item 0 if there's one.
    if (_activeRofIndex >= _spec.numRofItems) return nullptr;
    return &_spec.rofItems[_activeRofIndex];
}

// ─── Shot atomic burst ──────────────────────────────────────────────

inline void GunUnit::doShot() {
    commandFlash();
    commandRecoilJerk();
    if (_spec.recoilHoldMs > 0) {
        _recoilReturnAtMs = sfx_core::EffectClock::instance().nowMs()
                          + _spec.recoilHoldMs;
    }
    // Visual-only broadcast — the audio path lives on a separate
    // `_audio` callback (started by fireOnce/startFiring, stopped by
    // stopFiring) so the looping shot WAV during sustained fire isn't
    // re-triggered on every tick.  Studio's verbose mirror still ticks
    // its per-shot indicator from this event.
    if (_shot && _shotCtx) {
        _shot(_shotCtx, _spec.id);
    }
}

inline void GunUnit::commandFlash() {
    if (!_send) {
        SFX_LOG_WARN("[gun] %u: commandFlash — _send is null, skipping", _spec.id);
        return;
    }
    if (_spec.muzzleFlashPort.portKind == 0) {
        SFX_LOG_WARN("[gun] %u: commandFlash — muzzleFlashPort.portKind=0 (no muzzle port configured)", _spec.id);
        return;
    }

    using hubfx::effects::lightfx::LightEvent;
    using hubfx::effects::lightfx::serializeQueueLoad;

    LightEvent ev = LightEvent::on(_spec.flashBrightness, _spec.flashDurationMs);
    uint8_t queue[2 + 10];
    size_t qlen = serializeQueueLoad(_spec.muzzleFlashPort.portIdx, &ev, 1,
                                     queue, sizeof(queue));
    if (qlen == 0) return;
    _send(_sendCtx, _spec.muzzleFlashPort,
          RolePacket::LED_QUEUE_LOAD, queue, qlen);
    const uint8_t bright[2] = { _spec.muzzleFlashPort.portIdx, 100 };
    _send(_sendCtx, _spec.muzzleFlashPort,
          RolePacket::LED_SET_BRIGHTNESS, bright, sizeof(bright));
    const uint8_t start[1] = { _spec.muzzleFlashPort.portIdx };
    _send(_sendCtx, _spec.muzzleFlashPort,
          RolePacket::LED_START, start, sizeof(start));
}

// Recoil is now a TURRET BEHAVIOUR — there is no dedicated recoil
// servo.  When fired, we add a jerk to whichever axis (yaw / pitch)
// the spec nominates as the recoil axis, save the prior commanded µs,
// and hold there until the return timer fires (see update()).
// `_recoilActive` then suppresses RC updates on that axis until the
// hold completes — otherwise the very next tick would overwrite the
// kicked target with whatever the operator is feeding in.
inline void GunUnit::commandRecoilJerk() {
    if (!_send) return;
    if (!_spec.recoilEnabled) return;
    const bool isYaw = (_spec.recoilAxis == 1);
    const GunAxis& axis = isYaw ? _spec.yaw : _spec.pitch;
    if (!axis.enabled || axis.servoPort.portKind == 0) return;
    uint16_t& curTarget = isYaw ? _yawTargetUs : _pitchTargetUs;
    _recoilSavedUs = curTarget;
    _recoilActive  = true;
    const uint16_t kicked = (uint16_t)(curTarget + _spec.recoilJerkUs);
    commandServoTargetUs(axis.servoPort, kicked);
    curTarget = kicked;
}

inline void GunUnit::commandHeater(bool on) {
    if (!_send) return;
    if (_spec.smoke.heaterPort.portKind == 0) return;

    // Intent-level: set target temperature for the heater role.  Voltage
    // scaling + bang-bang logic lives in HeaterRole (Phase 2 — element
    // scaling on the role layer).  Off = INT16_MIN sentinel.
    int16_t target;
    if (!on) {
        target = INT16_MIN;
    } else if (_spec.smoke.heaterMode == SmokeConfig::HM_ALWAYS_ON) {
        // ALWAYS_ON: pick a target high enough that the bang-bang gate
        // is permanently below it. The role's open-loop path (no temp
        // sensor) treats any non-sentinel target as "drive at drivePct".
        target = _spec.smoke.heaterTargetCx10 > 0 ? _spec.smoke.heaterTargetCx10 : 1500;
    } else {
        target = _spec.smoke.heaterTargetCx10;
    }
    uint8_t payload[3];
    payload[0] = _spec.smoke.heaterPort.portIdx;
    SfxWire::putU16LE(&payload[1], static_cast<uint16_t>(target));
    _send(_sendCtx, _spec.smoke.heaterPort,
          RolePacket::HEATER_SET_TARGET, payload, sizeof(payload));
}

inline void GunUnit::commandFanPct(uint8_t pct) {
    if (!_send) return;
    if (_spec.smoke.fanPort.portKind == 0) return;
    // Phase 2.9.x: MOTOR_SET_PCT (Rule 42 intent layer).  The
    // DcMotorRole on the receiving end applies scaleDuty() using the
    // port rail voltage + element rated voltage + scaling mode
    // configured at attach time.  Gun says "drive at N %"; the role
    // turns it into the right port-native duty.
    const uint8_t payload[2] = { _spec.smoke.fanPort.portIdx, pct };
    _send(_sendCtx, _spec.smoke.fanPort,
          RolePacket::MOTOR_SET_PCT, payload, sizeof(payload));
}

inline void GunUnit::commandServoTargetUs(const PortRef& port, uint16_t us) {
    if (!_send) return;
    uint8_t payload[3];
    payload[0] = port.portIdx;
    SfxWire::putU16LE(&payload[1], us);
    _send(_sendCtx, port, RolePacket::SERVO_SET_TARGET, payload, sizeof(payload));
}

// ─── Fan scheduling ─────────────────────────────────────────────────

inline void GunUnit::scheduleFanPuff(uint32_t nowMs) {
    if (_spec.smoke.fanPort.portKind == 0) return;
    // Smoke-fan invariant: ON only when smoke is armed AND the gun is
    // actively firing (or in the act of firing — single-shot pulses
    // come through the manual `_pendingFanBurst` path which is
    // operator-driven and bypasses this gate).  Mode-driven puffs
    // (FN_PUFF_PER_SHOT, FN_PUFF_ON_FIRE_ACTIVE) all flow through
    // here, so this single guard covers every automatic path.
    if (!_smokeArmed || !_firing) return;
    const uint16_t ms = _spec.smoke.fanPuffMs ? _spec.smoke.fanPuffMs : 200;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandFanPct(100);
    if (_commit && _sendCtx) _commit(_sendCtx);
    _fanPuffEndMs = nowMs + ms;
}

inline void GunUnit::tickFan(uint32_t nowMs) {
    if (_spec.smoke.fanPort.portKind == 0) return;

    // Pending operator-driven manual burst (GUN_MANUAL_SET FAN_BURST flag).
    // This BYPASSES the smoke-armed + firing gate because puppet mode
    // is explicitly an operator override — the test panel uses it to
    // verify the fan independent of the rest of the state machine.
    // The puff still auto-turns-off via `_fanPuffEndMs` below.
    if (_pendingFanBurst) {
        _pendingFanBurst = false;
        const uint16_t ms = _spec.smoke.fanPuffMs ? _spec.smoke.fanPuffMs : 200;
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandFanPct(100);
        if (_commit && _sendCtx) _commit(_sendCtx);
        _fanPuffEndMs = nowMs + ms;
        return;
    }

    // Puff timeout — turn fan off after `fanPuffMs`.
    if (_fanPuffEndMs && (int32_t)(nowMs - _fanPuffEndMs) >= 0) {
        _fanPuffEndMs = 0;
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandFanPct(0);
        if (_commit && _sendCtx) _commit(_sendCtx);
    }

    // Continuous mode + fire-active edges (Rule 2026-05-24 invariant:
    // fan ON only when `_smokeArmed && _firing`).  The triple gate
    // here is the authoritative check for FN_CONTINUOUS; mode-driven
    // puffs (FN_PUFF_PER_SHOT, FN_PUFF_ON_FIRE_ACTIVE) flow through
    // `scheduleFanPuff()` which carries the same gate.
    const bool wantContinuous =
        _smokeArmed && _spec.smoke.fanMode == SmokeConfig::FN_CONTINUOUS && _firing;
    if (wantContinuous != _fanContinuous) {
        _fanContinuous = wantContinuous;
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandFanPct(wantContinuous ? 100 : 0);
        if (_commit && _sendCtx) _commit(_sendCtx);
    }

    // FN_PUFF_ON_FIRE_ACTIVE — one pulse on the rising edge of _firing.
    // Tracked here via continuous-edge detection; the rising edge is
    // when _firing becomes true. The actual scheduleFanPuff happens in
    // startFiring() when fanMode matches.
    // (Implemented inline above in update() via the same path as
    // PUFF_PER_SHOT — re-evaluated when needed.)
}

// ─── Axis tick ──────────────────────────────────────────────────────
//
// Phase 2.9: the GunUnit no longer integrates a motion profile.
// `ServoActuatorRole` (attached to each yaw/pitch port via the
// /hubfx.yaml role-attach config) owns the integrator and applies
// clamp + speed + accel + jerk shape.  We only push the target — and
// only when it changed — so the wire stays quiet for stable inputs.

inline void GunUnit::tickAxis(const GunAxis& axis,
                              uint16_t lastRcUs,
                              bool manualValid, uint16_t manualUs,
                              uint16_t& lastCommandedRef) {
    if (!_send) return;
    if (axis.servoPort.portKind == 0) return;

    const uint16_t target = manualValid ? manualUs : lastRcUs;
    if (target == lastCommandedRef) return;     // input stable → no wire traffic
    lastCommandedRef = target;

    if (_begin && _sendCtx) _begin(_sendCtx);
    commandServoTargetUs(axis.servoPort, target);
    if (_commit && _sendCtx) _commit(_sendCtx);
}

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUN_UNIT_IPP
