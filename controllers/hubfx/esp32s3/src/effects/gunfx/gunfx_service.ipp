/*
 * gunfx_service.ipp — GunFX service template-method bodies (Phase 2 of
 *                     GunFX rollout, instructions/22).
 */

#ifndef HUBFX_GUNFX_SERVICE_IPP
#define HUBFX_GUNFX_SERVICE_IPP

#include <Arduino.h>
#include <serial/wire.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw millis()

#if defined(SFX_HAS_AUDIO)
#include <audio/audio_mixer.h>
#endif

namespace hubfx::effects::gunfx {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;
    _topo       = ctx->template findPolicy<TTopology>();
    _dispatcher = ctx->template findPolicy<TInputDispatcher>();
    if (!_topo) {
        SFX_LOG_ERROR("[gun-svc] TopologyService not found");
        return false;
    }

    reapplySpecs();
    _lastUpdateMs = sfx_core::EffectClock::instance().nowMs();
    SFX_LOG_INFO("[gun-svc] ready (%u guns)", (unsigned)_numSpecs);
    return true;
}

// Re-bind GunUnits + dispatcher subscriptions to the current `_specs[]`.
// Called from begin() (initial bring-up) and from configure() (so a
// /gunfx.yaml reload picks up new ports without a reboot — Phase 2.9.x
// stopgap §8 #4).
template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::reapplySpecs() {
    unsubscribePerGunInputs();
    for (uint8_t i = 0; i < _numSpecs; ++i) {
        _units[i].configure(_specs[i],
                            &GunFxServicePolicyT::sendRoleCmdTrampoline,
                            static_cast<void*>(_topo),
                            &GunFxServicePolicyT::beginBatchTrampoline,
                            &GunFxServicePolicyT::commitBatchTrampoline,
                            &GunFxServicePolicyT::shotEventTrampoline,
                            static_cast<void*>(this),
                            &GunFxServicePolicyT::audioCmdTrampoline,
                            static_cast<void*>(this));
    }
    subscribePerGunInputs();
    claimPorts();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::unsubscribePerGunInputs() {
    if (!_dispatcher) return;
    for (uint8_t i = 0; i < kMaxGuns; ++i) {
        for (uint8_t k = 0; k < (uint8_t)TrigKind::_Count; ++k) {
            _dispatcher->unsubscribe(&_triggers[i][k]);
        }
    }
}

// ─── Port claiming (all 10 slots, output-exclusive) ─────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::claimPorts() {
    using namespace sfx_core;
    for (uint8_t i = 0; i < _numSpecs; ++i) {
        const GunSpec& s = _specs[i];
        if (s.muzzleFlashPort.portKind != 0)
            _topo->claim(s.muzzleFlashPort, EffectId::GunFx, RoleKind::LedAnimator);
        // Recoil no longer owns a port — it kicks the yaw/pitch axis
        // claimed below (Phase 4 polish, recoil is a turret behaviour).
        if (s.smoke.heaterPort.portKind != 0)
            _topo->claim(s.smoke.heaterPort, EffectId::GunFx, RoleKind::Heater);
        if (s.smoke.fanPort.portKind != 0)
            _topo->claim(s.smoke.fanPort, EffectId::GunFx, RoleKind::DcMotor);
        if (s.yaw.enabled && s.yaw.servoPort.portKind != 0)
            _topo->claim(s.yaw.servoPort, EffectId::GunFx, RoleKind::ServoActuator);
        if (s.pitch.enabled && s.pitch.servoPort.portKind != 0)
            _topo->claim(s.pitch.servoPort, EffectId::GunFx, RoleKind::ServoActuator);
        // Input ports (trigger / ROF selector / yaw input / pitch input)
        // are shareable — no exclusive claim.
    }
}

// ─── Input subscriptions (per-gun trigger + ROF + yaw + pitch) ──────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::subscribePerGunInputs() {
    if (!_dispatcher) return;
    for (uint8_t i = 0; i < _numSpecs; ++i) {
        const GunSpec& s = _specs[i];

        // 1. Fire trigger — Boolean (debounced edges).
        if (s.triggerPort.portKind != 0) {
            _inputCtx[i][(uint8_t)TrigKind::Trigger] = { this, i, TrigKind::Trigger };
            input::TriggerMapping m;
            m.kind         = input::TriggerKind::Boolean;
            m.thresholdUs  = s.triggerThresholdUs;
            m.hysteresisUs = s.triggerHysteresisUs;
            m.failsafe     = input::FailsafeBehaviour::ForceLow;
            _triggers[i][(uint8_t)TrigKind::Trigger].configure(m,
                &GunFxServicePolicyT::inputChangeTrampoline,
                static_cast<void*>(&_inputCtx[i][(uint8_t)TrigKind::Trigger]));
            _dispatcher->subscribe(&_triggers[i][(uint8_t)TrigKind::Trigger],
                                   s.triggerPort, s.triggerChannel);
        }

        // 2. ROF selector — Raw (we need the µs to do band arbitration).
        if (s.rofSelectorPort.portKind != 0) {
            _inputCtx[i][(uint8_t)TrigKind::Rof] = { this, i, TrigKind::Rof };
            input::TriggerMapping m;
            m.kind     = input::TriggerKind::Raw;
            m.failsafe = input::FailsafeBehaviour::Hold;
            _triggers[i][(uint8_t)TrigKind::Rof].configure(m,
                &GunFxServicePolicyT::inputChangeTrampoline,
                static_cast<void*>(&_inputCtx[i][(uint8_t)TrigKind::Rof]));
            _dispatcher->subscribe(&_triggers[i][(uint8_t)TrigKind::Rof],
                                   s.rofSelectorPort, s.rofSelectorChannel);
        }

        // 3 + 4. Yaw / pitch input channels — Raw.
        if (s.yaw.enabled && s.yaw.inputPort.portKind != 0) {
            _inputCtx[i][(uint8_t)TrigKind::Yaw] = { this, i, TrigKind::Yaw };
            input::TriggerMapping m;
            m.kind     = input::TriggerKind::Raw;
            m.failsafe = input::FailsafeBehaviour::Hold;
            _triggers[i][(uint8_t)TrigKind::Yaw].configure(m,
                &GunFxServicePolicyT::inputChangeTrampoline,
                static_cast<void*>(&_inputCtx[i][(uint8_t)TrigKind::Yaw]));
            _dispatcher->subscribe(&_triggers[i][(uint8_t)TrigKind::Yaw],
                                   s.yaw.inputPort, s.yaw.inputChannel);
        }
        if (s.pitch.enabled && s.pitch.inputPort.portKind != 0) {
            _inputCtx[i][(uint8_t)TrigKind::Pitch] = { this, i, TrigKind::Pitch };
            input::TriggerMapping m;
            m.kind     = input::TriggerKind::Raw;
            m.failsafe = input::FailsafeBehaviour::Hold;
            _triggers[i][(uint8_t)TrigKind::Pitch].configure(m,
                &GunFxServicePolicyT::inputChangeTrampoline,
                static_cast<void*>(&_inputCtx[i][(uint8_t)TrigKind::Pitch]));
            _dispatcher->subscribe(&_triggers[i][(uint8_t)TrigKind::Pitch],
                                   s.pitch.inputPort, s.pitch.inputChannel);
        }
    }
}

// ─── Tick ───────────────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::update() {
    const uint32_t now = sfx_core::EffectClock::instance().nowMs();
    const uint32_t dtMs = (now > _lastUpdateMs) ? (now - _lastUpdateMs) : 0;
    _lastUpdateMs = now;

    for (uint8_t i = 0; i < _numSpecs; ++i) {
        _units[i].update(now, dtMs);
        if (_verbose[i] && (now - _lastVerboseMs[i]) >= kVerboseStatusIntervalMs) {
            emitVerboseStatus(i, now);
            _lastVerboseMs[i] = now;
        }
    }
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
GunUnit* GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::findById(uint8_t id) {
    const int8_t idx = indexById(id);
    return (idx >= 0) ? &_units[idx] : nullptr;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
int8_t GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::indexById(uint8_t id) const {
    for (uint8_t i = 0; i < _numSpecs; ++i) {
        if (_specs[i].id == id) return (int8_t)i;
    }
    return -1;
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
CommandHandleResult GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case GunPacket::GUN_FIRE_ONCE:           handleFireOnce(payload, len);          return CommandHandleResult::Handled;
        case GunPacket::GUN_START_FIRING:        handleStartFiring(payload, len);       return CommandHandleResult::Handled;
        case GunPacket::GUN_STOP_FIRING:         handleStopFiring(payload, len);        return CommandHandleResult::Handled;
        case GunPacket::GUN_SMOKE_ARM:           handleSmokeArm(payload, len);          return CommandHandleResult::Handled;
        case GunPacket::GUN_STATUS_REQ:          handleStatusReq();                     return CommandHandleResult::Handled;
        case GunPacket::GUN_MANUAL_SET:          handleManualSet(payload, len);         return CommandHandleResult::Handled;
        case GunPacket::GUN_MANUAL_RELEASE:      handleManualRelease(payload, len);     return CommandHandleResult::Handled;
        case GunPacket::GUN_VERBOSE_STATUS_REQ:  handleVerboseStatusReq(payload, len);  return CommandHandleResult::Handled;
        default:                                 return CommandHandleResult::NotMyCommand;
    }
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleFireOnce(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    // Rule 11 append (2026-05-24): optional explicit rofIndex.  When
    // omitted (1-byte payload), the gun uses its currently-armed ROF;
    // when 0xFF, same semantic.  Out-of-range NACKs.
    uint8_t rofIdx = 0xFF;
    if (len >= 2) {
        rofIdx = p[1];
        if (rofIdx != 0xFF && rofIdx >= g->spec().numRofItems) {
            _ctx->sendNack(GunError::ROF_OUT_OF_RANGE);
            return;
        }
    }
    g->fireOnce(rofIdx);
    _ctx->sendAck();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleStartFiring(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    uint16_t rpm = 0;
    if (len >= 3) {
        rpm = static_cast<uint16_t>(p[1]) | (static_cast<uint16_t>(p[2]) << 8);
    }
    // Rule 11 append (2026-05-24): optional explicit rofIndex after
    // the rpm word.  4-byte payload form is the GUI Auto-Fire button
    // sending `[id][rpm:u16][rofIndex]`.
    uint8_t rofIdx = 0xFF;
    if (len >= 4) {
        rofIdx = p[3];
        if (rofIdx != 0xFF && rofIdx >= g->spec().numRofItems) {
            _ctx->sendNack(GunError::ROF_OUT_OF_RANGE);
            return;
        }
    }
    g->startFiring(rpm, rofIdx);
    _ctx->sendAck();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleStopFiring(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    g->stopFiring();
    _ctx->sendAck();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleSmokeArm(
        const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    g->armSmoke(p[1] != 0);
    _ctx->sendAck();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleStatusReq() {
    uint8_t buf[1 + kMaxGuns * 3];
    buf[0] = _numSpecs;
    size_t off = 1;
    for (uint8_t i = 0; i < _numSpecs; ++i) {
        buf[off++] = _units[i].id();
        buf[off++] = _units[i].firing()     ? 1 : 0;
        buf[off++] = _units[i].smokeArmed() ? 1 : 0;
    }
    _ctx->sendRawPacket(GunPacket::GUN_STATUS_RESP,
                        _ctx->currentTag(), buf, off);
}

// ─── Manual override ────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleManualSet(
        const uint8_t* p, size_t len) {
    // Payload: [id][flags][yawUs:u16][pitchUs:u16][rofIdx][fireHold][smokeArm][smokeFanBurst] = 10 B
    if (len < 10) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }

    const uint8_t  flags     = p[1];
    const uint16_t yawUs     = SfxWire::getU16LE(&p[2]);
    const uint16_t pitchUs   = SfxWire::getU16LE(&p[4]);
    const uint8_t  rofIdx    = p[6];
    const bool     fireHold  = p[7] != 0;
    const bool     smokeArm  = p[8] != 0;
    const bool     fanBurst  = p[9] != 0;
    const uint32_t now       = sfx_core::EffectClock::instance().nowMs();
    g->applyManualSet(flags, yawUs, pitchUs, rofIdx, fireHold,
                      smokeArm, fanBurst, now);
    _ctx->sendAck();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleManualRelease(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    g->releaseManual();
    _ctx->sendAck();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleVerboseStatusReq(
        const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const int8_t idx = indexById(p[0]);
    if (idx < 0) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    _verbose[idx] = (p[1] != 0);
    _lastVerboseMs[idx] = 0;     // force a fresh broadcast next tick
    _ctx->sendAck();
}

// ─── Verbose status producer ────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::emitVerboseStatus(
        uint8_t unitIdx, uint32_t /*nowMs*/) {
    if (!_ctx) return;
    const GunUnit& u = _units[unitIdx];

    // 26-byte fixed layout — matches DecodeVerboseStatus in
    // app/go/protocol/gunfx/gunfx.go (see gunfx_protocol.h for the
    // field map). The trailing 0 byte is reserved padding (keeps the
    // size at 26 B; future fields land here without a wire bump).
    uint8_t buf[26] = {};
    size_t  off = 0;
    buf[off++] = u.id();
    buf[off++] = u.manual().active ? GunMode::MANUAL : GunMode::RC;
    buf[off++] = u.firing()      ? 1 : 0;
    buf[off++] = u.smokeArmed()  ? 1 : 0;
    // Smoke fan running — real state read from GunUnit::fanRunning()
    // (Phase 2.9.x — replaces the (smokeArmed && firing) stopgap).
    buf[off++] = u.fanRunning() ? 1 : 0;
    buf[off++] = 0;                                            // heater duty %
    SfxWire::putI16LE(&buf[off], (int16_t)0x7FFF); off += 2;    // heater temp = no sensor
    SfxWire::putU16LE(&buf[off], u.yawCurrentUs());   off += 2;
    SfxWire::putU16LE(&buf[off], u.yawTargetUs());    off += 2;
    SfxWire::putU16LE(&buf[off], u.pitchCurrentUs()); off += 2;
    SfxWire::putU16LE(&buf[off], u.pitchTargetUs());  off += 2;
    buf[off++] = u.rofIndex();
    SfxWire::putU16LE(&buf[off], u.rofSelectorUs());  off += 2;
    // Trigger raw µs (Phase 2.9.x): read the shadow value from the
    // Boolean TriggerInput's `lastPulseUs()` — captured by feed() even
    // when the typed value is Boolean.  No separate Raw subscription
    // needed (avoids doubling dispatcher fanout).
    {
        const auto& trig = const_cast<input::TriggerInput&>(
            _triggers[unitIdx][(uint8_t)TrigKind::Trigger]);
        SfxWire::putU16LE(&buf[off], trig.lastPulseUs()); off += 2;
    }
    const uint32_t shots = u.shotsThisSession();
    buf[off++] = (uint8_t)(shots);
    buf[off++] = (uint8_t)(shots >> 8);
    buf[off++] = (uint8_t)(shots >> 16);
    buf[off++] = (uint8_t)(shots >> 24);
    // off should be 25 here; buf[25] stays 0 (reserved padding).

    _ctx->sendRawPacket(GunPacket::GUN_VERBOSE_STATUS,
                        SfxWire::TAG_ASYNC, buf, sizeof(buf));
}

// ─── Shot fan-out ───────────────────────────────────────────────────

// Visual broadcast only.  Called on EVERY shot (one-shot, auto-fire
// tick, sustained-fire start) so Studio's verbose-status mirror sees
// individual shot pulses regardless of how the audio is being driven.
template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::onShotEvent(
        uint8_t id) {
    if (!_ctx) return;
    const uint8_t payload[1] = { id };
    _ctx->sendRawPacket(GunPacket::GUN_SHOT_EVENT,
                        SfxWire::TAG_ASYNC, payload, sizeof(payload));
}

// Audio command — drives the local mixer.  `soundPath == nullptr`
// stops the channel (used by `stopFiring()` to drop the sustained
// loop).  `loop=true` plays the WAV indefinitely (sustained fire);
// `loop=false` is one-shot (single fire, or the first tick of a
// burst before the loop takes over — though startFiring() now uses
// the loop=true path directly).
template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::onAudioCmd(
        const char* soundPath, uint8_t audioChannel,
        uint8_t outputMask, bool loop) {
#if defined(SFX_HAS_AUDIO)
    if (!soundPath) {
        // Stop the channel — releases the looping sample on stopFiring().
        SFX_LOG_INFO("[gun-audio] STOP ch=%u", (unsigned)audioChannel);
        TMixer::instance().stopAsync((int)audioChannel,
                                     AudioStopMode::Immediate);
        return;
    }
    if (!soundPath[0]) return;          // empty path = no-op
    AudioPlaybackOptions opts;
    opts.volume         = 1.0f;
    opts.outputChannels = outputMask;
    if (loop) {
        opts.loop      = true;
        opts.loopCount = LOOP_INFINITE;
    }
    // Preload-into-memory (2026-05-23): gun shot WAVs are small (typ.
    // 200 ms – 2 s, well under the 2 s @ 48 kHz preload cap) and they
    // loop hot during sustained fire.  Asking the mixer to cache the
    // whole sample in PSRAM means the loop wrap never touches SD —
    // critical because the engine-running loop is reading SD on the
    // SAME card and the per-wrap seek was making gun audio stutter
    // (operator-reported, 2026-05-23).  Mixer falls back to streaming
    // if the file exceeds the cap OR PSRAM headroom is tight.
    opts.preloadIntoMemory = true;
    const bool queued = TMixer::instance().playAsync(audioChannel, soundPath, opts);
    SFX_LOG_INFO("[gun-audio] PLAY ch=%u %s loop=%d mask=0x%02x → queued=%d",
                 (unsigned)audioChannel, soundPath, (int)loop,
                 (unsigned)outputMask, (int)queued);
#else
    (void)soundPath; (void)audioChannel; (void)outputMask; (void)loop;
#endif
}

// ─── Trampolines ────────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
bool GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::sendRoleCmdTrampoline(
        void* ctx, const PortRef& addr, uint8_t innerType,
        const uint8_t* p, size_t len) {
    return static_cast<TTopology*>(ctx)->sendRoleCommand(addr, innerType, p, len);
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::beginBatchTrampoline(void* ctx) {
    if (auto* t = static_cast<TTopology*>(ctx)) t->beginBatch();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::commitBatchTrampoline(void* ctx) {
    if (auto* t = static_cast<TTopology*>(ctx)) t->commitBatch();
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::shotEventTrampoline(
        void* ctx, uint8_t id) {
    auto* self = static_cast<GunFxServicePolicyT*>(ctx);
    if (self) self->onShotEvent(id);
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::audioCmdTrampoline(
        void* ctx, const char* soundPath, uint8_t audioChannel,
        uint8_t outputMask, bool loop) {
    auto* self = static_cast<GunFxServicePolicyT*>(ctx);
    if (self) self->onAudioCmd(soundPath, audioChannel, outputMask, loop);
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::inputChangeTrampoline(
        void* ctx, const input::TriggerValue& v) {
    auto* ic = static_cast<InputCtx*>(ctx);
    if (!ic || !ic->svc) return;
    GunUnit& u = ic->svc->_units[ic->unitIdx];
    switch (ic->kind) {
        case TrigKind::Trigger:
            // Boolean: edge-driven start/stop. Raw µs is also useful
            // for the verbose-status mirror — feed it via a second
            // subscription path? For now we only have the boolean here;
            // the raw value comes from the TriggerInput's `last()` value
            // if needed. Phase 4 polish: subscribe a second Raw input
            // for the trigger if Studio needs the live µs trace.
            if (v.kind == input::TriggerKind::Boolean) {
                u.onTriggerBoolean(v.b);
            }
            break;
        case TrigKind::Rof:
            if (v.kind == input::TriggerKind::Raw) {
                u.onRofSelectorUs(v.us, v.valid);
            }
            break;
        case TrigKind::Yaw:
            if (v.kind == input::TriggerKind::Raw) {
                u.onYawInputUs(v.us, v.valid);
            }
            break;
        case TrigKind::Pitch:
            if (v.kind == input::TriggerKind::Raw) {
                u.onPitchInputUs(v.us, v.valid);
            }
            break;
        default: break;
    }
}

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUNFX_SERVICE_IPP
