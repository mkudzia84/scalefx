/*
 * gunfx_service.ipp — GunFX service template-method bodies.
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

    for (uint8_t i = 0; i < _numDefs; ++i) {
        _units[i].configure(_defs[i],
                            &GunFxServicePolicyT::sendRoleCmdTrampoline,
                            static_cast<void*>(_topo),
                            &GunFxServicePolicyT::beginBatchTrampoline,
                            &GunFxServicePolicyT::commitBatchTrampoline,
                            &GunFxServicePolicyT::shotEventTrampoline,
                            static_cast<void*>(this));

        // Per-unit trigger input — register only when the unit's def
        // names an RC port AND the dispatcher is present.
        if (_dispatcher && _defs[i].trigger.portKind != 0) {
            _trigCtx[i] = { this, i };
            input::TriggerMapping m;
            m.kind         = input::TriggerKind::Boolean;
            m.thresholdUs  = _defs[i].triggerThresholdUs;
            m.hysteresisUs = 50;
            m.failsafe     = input::FailsafeBehaviour::ForceLow;
            _triggers[i].configure(m,
                                   &GunFxServicePolicyT::triggerChangeTrampoline,
                                   static_cast<void*>(&_trigCtx[i]));
            _dispatcher->subscribe(&_triggers[i], _defs[i].trigger, /*channel=*/0);
        }
    }
    claimPorts();
    SFX_LOG_INFO("[gun-svc] ready (%u guns)", (unsigned)_numDefs);
    return true;
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::claimPorts() {
    using namespace sfx_core;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const GunDef& d = _defs[i];
        if (d.muzzleFlash.portKind != 0) {
            _topo->claim(d.muzzleFlash, EffectId::GunFx, RoleKind::LedAnimator);
        }
        if (d.recoilServo.portKind != 0) {
            _topo->claim(d.recoilServo, EffectId::GunFx, RoleKind::ServoActuator);
        }
        if (d.smokeHeater.portKind != 0) {
            _topo->claim(d.smokeHeater, EffectId::GunFx, RoleKind::Heater);
        }
        // Trigger input is shared — no exclusive claim.
    }
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::update() {
    const uint32_t now = sfx_core::EffectClock::instance().nowMs();
    for (uint8_t i = 0; i < _numDefs; ++i) {
        _units[i].update(now);
    }
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
GunUnit* GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::findById(uint8_t id) {
    for (uint8_t i = 0; i < _numDefs; ++i) {
        if (_units[i].id() == id) return &_units[i];
    }
    return nullptr;
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
CommandHandleResult GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case GunPacket::GUN_FIRE_ONCE:    handleFireOnce(payload, len);    return CommandHandleResult::Handled;
        case GunPacket::GUN_START_FIRING: handleStartFiring(payload, len); return CommandHandleResult::Handled;
        case GunPacket::GUN_STOP_FIRING:  handleStopFiring(payload, len);  return CommandHandleResult::Handled;
        case GunPacket::GUN_SMOKE_ARM:    handleSmokeArm(payload, len);    return CommandHandleResult::Handled;
        case GunPacket::GUN_STATUS_REQ:   handleStatusReq();               return CommandHandleResult::Handled;
        default:                          return CommandHandleResult::NotMyCommand;
    }
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::handleFireOnce(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    GunUnit* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GunError::UNKNOWN_ID); return; }
    g->fireOnce();
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
    g->startFiring(rpm);
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
    buf[0] = _numDefs;
    size_t off = 1;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        buf[off++] = _units[i].id();
        buf[off++] = _units[i].firing()     ? 1 : 0;
        buf[off++] = _units[i].smokeArmed() ? 1 : 0;
    }
    _ctx->sendRawPacket(GunPacket::GUN_STATUS_RESP,
                        _ctx->currentTag(), buf, off);
}

// ─── Shot fan-out ───────────────────────────────────────────────────

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::onShotFired(
        uint8_t id, const char* soundPath,
        uint8_t audioChannel, uint8_t outputMask) {
    if (!_ctx) return;
    const uint8_t payload[1] = { id };
    _ctx->sendRawPacket(GunPacket::GUN_SHOT_EVENT,
                        SfxWire::TAG_ASYNC, payload, sizeof(payload));

#if defined(SFX_HAS_AUDIO)
    if (soundPath && soundPath[0]) {
        AudioPlaybackOptions opts;
        opts.volume         = 1.0f;
        opts.outputChannels = outputMask;
        TMixer::instance().playAsync(audioChannel, soundPath, opts);
    }
#else
    (void)soundPath; (void)audioChannel; (void)outputMask;
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
        void* ctx, uint8_t id, const char* soundPath,
        uint8_t audioChannel, uint8_t outputMask) {
    auto* self = static_cast<GunFxServicePolicyT*>(ctx);
    if (self) self->onShotFired(id, soundPath, audioChannel, outputMask);
}

template <MixerLike TMixer, hubfx::topology::TopologyService TTopology, hubfx::effects::input::InputDispatcher TInputDispatcher>
void GunFxServicePolicyT<TMixer, TTopology, TInputDispatcher>::triggerChangeTrampoline(
        void* ctx, const input::TriggerValue& v) {
    auto* tc = static_cast<TriggerCtx*>(ctx);
    if (!tc || !tc->svc) return;
    if (v.kind != input::TriggerKind::Boolean) return;
    GunUnit& u = tc->svc->_units[tc->unitIdx];
    // The TriggerInput already debounced + applied hysteresis; we
    // just forward the boolean edge to start / stop firing.
    if (v.b) u.startFiring(0);    // 0 → use defaultIntervalMs
    else     u.stopFiring();
}

}  // namespace hubfx::effects::gunfx

#endif  // HUBFX_GUNFX_SERVICE_IPP
