/*
 * gun_unit.ipp — single-gun state-machine method definitions
 *                (Phase 2 of GunFX rollout, instructions/22).
 *
 *   What changed vs the Phase 1 stub:
 *     - Reads from `GunSpec` (gunfx_config.h) instead of the old GunDef
 *     - Multi-band rate-of-fire: selector channel picks the armed RofItem;
 *       out-of-band suppresses firing
 *     - Smoke fan modes (continuous / puff_per_fire)
 *     - Heater driven via the HeaterRole intent API — no duty math here
 *     - Yaw + pitch axes pushed at the intent layer to ServoActuatorRole
 *       (motion shaping lives on the role — Rule 42)
 *     - Manual override mode with 5 s auto-release (kManualTimeoutMs)
 */

#ifndef HUBFX_GUN_UNIT_IPP
#define HUBFX_GUN_UNIT_IPP

#include <Arduino.h>
#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <cmath>                   // sinf / M_PI for FN_PULSE sinusoidal envelope
#include <serial/roles.h>
#include <serial/wire.h>
#include <serial/diag_log.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw SFX_MILLIS()

// Rate-limit the FN_PULSE sinusoidal MOTOR_SET_PCT updates.  At 20 ms
// (50 Hz) we get ≈5 samples across a 100 ms sinusoid — plenty smooth
// for fan inertia, and keeps wire chatter manageable when sustained
// firing reset-restarts the sinusoid every 100 ms.
static constexpr uint32_t kFanUpdatePeriodMs = 20;

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
        }
        // Silent when the resolved item has no soundPath — that's a
        // valid config (visuals-only shot); the GUI flags missing
        // sounds in the ROF row itself, no per-fire warning needed.
    }
    (void)usedIdx;     // resolved index — kept for the comment trail
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

    SFX_LOG_INFO("[gun] %u: auto-fire @%u ms / shot (rof=%u)",
                 _spec.id, (unsigned)_shotIntervalMs,
                 (unsigned)_activeRofIndex);
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
    // OFF is ALWAYS idempotent — even if `_smokeArmed` already says
    // "off", we re-send HEATER_SET_TARGET(INT16_MIN) + cut the fan.
    // Rationale (2026-05-26): the Studio "Smoke OFF" button is a
    // safety control; if the firmware state ever drifts from Studio's
    // view (boot, re-apply, reconnect), an OFF click MUST land on the
    // wire.  Same-state ON, on the other hand, is cheap to skip
    // because the heater is already driving.
    if (armed && armed == _smokeArmed) return;
    _smokeArmed = armed;
    if (_begin && _sendCtx) _begin(_sendCtx);
    if (_spec.smoke.heaterPort.portKind != 0) {
        // HM_CYCLE arming kicks the cycle clock at the ON-phase so
        // smoke starts immediately on Smoke-On press; the OFF-phase
        // is scheduled `cycleOnMs` later.  HM_CONTINUOUS just calls
        // commandHeater(true) straight through.
        if (armed && _spec.smoke.heaterMode == SmokeConfig::HM_CYCLE) {
            _heaterCyclePhase  = true;
            const uint32_t onMs = _spec.smoke.heaterCycleOnMs ? _spec.smoke.heaterCycleOnMs : 1000;
            _heaterCycleNextMs = sfx_core::EffectClock::instance().nowMs() + onMs;
        } else if (!armed) {
            // Clear the cycle clock so a stale schedule can't drive
            // the heater after Smoke-Off.
            _heaterCyclePhase  = false;
            _heaterCycleNextMs = 0;
        }
        // commandHeater() ANDs `armed` with `_heaterActive` (Rule 43
        // activation gate) — when the activation channel is below
        // threshold the heater stays off even on armSmoke(true).
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
        _fanPulseStartMs = 0;
        _fanBurstEndMs   = 0;
        _pendingFanBurst = false;
        _fanLastPct      = 0;
    }
    if (_commit && _sendCtx) _commit(_sendCtx);
    SFX_LOG_INFO("[gun] %u: smoke %s", _spec.id, armed ? "armed" : "off");
}

// Composed driving-state predicate (Phase 4 polish 2026-05-26).  Used
// by the verbose-status reporter so heaterDutyPct reflects the actual
// wire-level drive, including HM_CYCLE OFF-phase.  The smoke heater is a
// single on/off (smokeArmed) — driven by the gun_smoke RC channel (which
// arms it) or the Studio button.  The separate "activation" gate was removed
// (2026-05-29): it was a redundant second gate on top of arm.
inline bool GunUnit::heaterDriving() const {
    if (!_smokeArmed) return false;
    if (_spec.smoke.heaterMode == SmokeConfig::HM_CYCLE) return _heaterCyclePhase;
    return true;     // HM_CONTINUOUS — driving any time smoke is armed/on
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
        // Fan pulse + continuous modes are now driven by `tickFan()`,
        // which runs every loop iteration — no per-shot puff
        // scheduling here.  The pulse cycle is independent of bullet
        // cadence: it runs while `_smokeArmed && _firing && mode ==
        // FN_PULSE`, flipping fan on/off on `fanPulseOnMs` / `fanPulseOffMs`
        // intervals.  See `tickFan()` for the scheduler.
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

    // HM_CYCLE — heater duty-cycle scheduler (Phase 4 polish 2026-05-26).
    // Gated on `_smokeArmed` so the cycle only runs when smoke is
    // armed.  The phase clock marches independently of the Rule 43
    // activation gate: if activation goes low mid-ON-phase, the wire
    // command is gated off by commandHeater()'s `effective` check, but
    // the next phase flip still happens on schedule.  Result: cycle
    // resumes cleanly when activation returns high.
    if (_smokeArmed
            && _spec.smoke.heaterMode == SmokeConfig::HM_CYCLE
            && _spec.smoke.heaterPort.portKind != 0
            && (int32_t)(nowMs - _heaterCycleNextMs) >= 0) {
        _heaterCyclePhase = !_heaterCyclePhase;
        const uint32_t nextPhaseMs = _heaterCyclePhase
            ? (_spec.smoke.heaterCycleOnMs  ? _spec.smoke.heaterCycleOnMs  : 1000)
            : (_spec.smoke.heaterCycleOffMs ? _spec.smoke.heaterCycleOffMs : 1000);
        _heaterCycleNextMs = nowMs + nextPhaseMs;
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandHeater(_heaterCyclePhase);
        if (_commit && _sendCtx) _commit(_sendCtx);
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

// (onHeaterActivationBoolean removed 2026-05-29 — the separate activation
//  gate is gone; the gun_smoke channel now arms the heater directly via
//  GunUnit::armSmoke(), routed in gunfx_service's inputChange handler.)

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
    // FN_PULSE — restart the sinusoidal envelope clock on every shot.
    // tickFan() reads _fanPulseStartMs to compute the current sample
    // along the 50% → 100% → 50% curve.  Resetting here means
    // sustained fire keeps the envelope "alive" with each shot
    // re-kicking the rise; the motor's inertia smooths the resulting
    // waveform when shots come faster than the envelope's duration.
    if (_smokeArmed && _spec.smoke.fanMode == SmokeConfig::FN_PULSE
            && _spec.smoke.fanPort.portKind != 0) {
        _fanPulseStartMs = sfx_core::EffectClock::instance().nowMs();
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
    if (!_send) return;
    if (_spec.muzzleFlashPort.portKind == 0) return;
    // Both early-returns are normal-config states (gun without a flash
    // LED, gun unit unbound before begin()).  The GUI flags missing
    // muzzle ports in the section header — no per-shot warning here
    // or the diag log floods on every burst.

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

    // Single on/off: the heater follows smoke-arm only (the redundant
    // activation gate was removed 2026-05-29).
    const bool effective = on;

    // Open-loop on/off.  HubFX has no temperature sensor wired to the
    // smoke heater, so HEATER_SET_TARGET toggles between INT16_MIN
    // (off) and an arbitrary non-sentinel value (on) — the role's
    // open-loop path treats any non-sentinel target as "drive at
    // drivePct" and applies voltage scaling (Rule 42 / scaleDuty())
    // against the port rail using the element_mv we pushed via
    // HEATER_SET_ELEMENT at configure time.  The literal "1" carries
    // no temperature meaning; it's just != INT16_MIN.
    //
    // HM_CYCLE mode toggles `on` between true and false at the gun
    // layer (see `update()`); each transition lands here and produces
    // the same INT16_MIN / non-sentinel pair.  The role doesn't need
    // to know it's being cycled.
    const int16_t target = effective ? int16_t(1) : INT16_MIN;
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
//
// Phase 4 polish 2026-05-26: fan modes are {continuous, pulse}.
//   FN_CONTINUOUS — fan runs at 100% while `_smokeArmed && _firing`.
//   FN_PULSE      — sinusoidal envelope: 50% base while firing+armed,
//                   ramps to 100% and back over `fanPulseDurationMs`
//                   on each shot event (doShot() resets the start
//                   clock).  When idle (no shots within the last
//                   envelope duration) → holds 50% base.  When NOT
//                   firing → fan off.
//
//   pct(t) = 50 + 50 * sin(π * t / fanPulseDurationMs), t ∈ [0, dur]
//   →  t=0           pct = 50  (base / start of envelope)
//   →  t=dur/2       pct = 100 (peak)
//   →  t=dur         pct = 50  (back to base; envelope done)
//
// Sustained fire that resets the clock faster than the envelope
// completes (e.g. 600 RPM = 100 ms period with default 100 ms
// duration) collapses into a perpetually-rising → peak → slightly-
// falling → reset waveform.  The motor's inertia smooths abrupt
// resets visually.
//
// _fanBurstEndMs handles the manual operator burst (puppet mode)
// independently of the mode scheduler — bypasses the smoke-armed +
// firing gate so the operator can verify the fan in isolation.

inline void GunUnit::scheduleFanPuff(uint32_t nowMs) {
    // Manual one-shot burst (GUN_MANUAL_SET FAN_BURST path).  Drives
    // the fan at 100% for `fanPulseDurationMs` and auto-cuts via
    // `_fanBurstEndMs`.  Bypasses the smoke-armed + firing gate.
    if (_spec.smoke.fanPort.portKind == 0) return;
    const uint16_t ms = _spec.smoke.fanPulseDurationMs ? _spec.smoke.fanPulseDurationMs : 100;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandFanPct(100);
    if (_commit && _sendCtx) _commit(_sendCtx);
    _fanLastPct = 100;
    _fanLastUpdateMs = nowMs;
    _fanBurstEndMs = nowMs + ms;
}

// Helper: send commandFanPct only when (a) the value changed
// (rounded to integer %), or (b) we're past the rate-limit window.
// Sinusoidal updates land at ~50 Hz; constant base / off skip the
// wire entirely once the value is set.
inline void GunUnit::sendFanPctIfChanged(uint8_t pct, uint32_t nowMs) {
    if (pct == _fanLastPct) return;
    if (nowMs - _fanLastUpdateMs < kFanUpdatePeriodMs && _fanLastPct != 255) return;
    if (_begin && _sendCtx) _begin(_sendCtx);
    commandFanPct(pct);
    if (_commit && _sendCtx) _commit(_sendCtx);
    _fanLastPct = pct;
    _fanLastUpdateMs = nowMs;
}

inline void GunUnit::tickFan(uint32_t nowMs) {
    if (_spec.smoke.fanPort.portKind == 0) return;

    // (1) Pending operator-driven manual burst (GUN_MANUAL_SET
    //     FAN_BURST flag).  Bypasses the smoke-armed + firing gate
    //     so the operator can test the fan in puppet mode.  Burst
    //     auto-cuts via _fanBurstEndMs below.
    if (_pendingFanBurst) {
        _pendingFanBurst = false;
        scheduleFanPuff(nowMs);
        return;
    }
    if (_fanBurstEndMs && (int32_t)(nowMs - _fanBurstEndMs) >= 0) {
        _fanBurstEndMs = 0;
        if (_begin && _sendCtx) _begin(_sendCtx);
        commandFanPct(0);
        if (_commit && _sendCtx) _commit(_sendCtx);
        _fanLastPct = 0;
        _fanLastUpdateMs = nowMs;
    }
    if (_fanBurstEndMs) return;     // burst still running — skip the mode scheduler

    // (2) Smoke-fan invariant: ON only when `_smokeArmed && _firing`.
    //     When this AND is false, every mode collapses to "fan off"
    //     and any in-flight envelope clock is cleared.
    const bool fanShouldRun = _smokeArmed && _firing;

    // (3) Continuous mode — edge-driven set/clear.
    if (_spec.smoke.fanMode == SmokeConfig::FN_CONTINUOUS) {
        if (fanShouldRun != _fanContinuous) {
            _fanContinuous = fanShouldRun;
            if (_begin && _sendCtx) _begin(_sendCtx);
            commandFanPct(fanShouldRun ? 100 : 0);
            if (_commit && _sendCtx) _commit(_sendCtx);
            _fanLastPct = fanShouldRun ? 100 : 0;
            _fanLastUpdateMs = nowMs;
        }
        return;
    }

    // (4) Pulse mode — sinusoidal envelope at 50% base, peaking to
    //     100% on each shot.  Off entirely when not firing+armed.
    if (_spec.smoke.fanMode == SmokeConfig::FN_PULSE) {
        if (!fanShouldRun) {
            // Idle: fan off, clear envelope clock.
            _fanPulseStartMs = 0;
            if (_fanLastPct != 0) {
                if (_begin && _sendCtx) _begin(_sendCtx);
                commandFanPct(0);
                if (_commit && _sendCtx) _commit(_sendCtx);
                _fanLastPct = 0;
                _fanLastUpdateMs = nowMs;
            }
            return;
        }

        const uint16_t dur = _spec.smoke.fanPulseDurationMs
                                ? _spec.smoke.fanPulseDurationMs : 100;
        uint8_t pct;
        if (_fanPulseStartMs == 0
                || (int32_t)(nowMs - _fanPulseStartMs) >= (int32_t)dur) {
            // No envelope in flight (just-fired before doShot kicks
            // the clock) OR previous envelope completed — sit at the
            // 50% base level.
            _fanPulseStartMs = 0;
            pct = 50;
        } else {
            // Sinusoidal envelope: pct = 50 + 50 * sin(π * t / dur).
            const uint32_t elapsed = nowMs - _fanPulseStartMs;
            const float t = float(elapsed) / float(dur);     // 0..1
            const float val = 50.0f + 50.0f * sinf(float(M_PI) * t);
            const int    clamped = (val < 0.0f) ? 0 : (val > 100.0f) ? 100 : int(val + 0.5f);
            pct = uint8_t(clamped);
        }
        sendFanPctIfChanged(pct, nowMs);
        return;
    }
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
