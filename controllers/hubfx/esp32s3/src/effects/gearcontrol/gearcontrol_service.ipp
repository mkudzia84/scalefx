/*
 * gearcontrol_service.ipp — GearControl service template-method bodies.
 */

#ifndef HUBFX_GEARCONTROL_SERVICE_IPP
#define HUBFX_GEARCONTROL_SERVICE_IPP

#include <Arduino.h>      // millis()
#include <serial/wire.h>

namespace hubfx::effects::gearctrl {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
bool GearControlServicePolicyT<TTopology, TLandingService>::begin(
        sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;

    _topo    = ctx->template findPolicy<TTopology>();
    _landing = ctx->template findPolicy<TLandingService>();
    if (!_topo) {
        SFX_LOG_ERROR("[gear-svc] TopologyService not found");
        return false;
    }
    // LandingLightService is OPTIONAL — gear without landing lights
    // is legal; the gear-bound landing-light fan-out is a no-op when
    // `_landing == nullptr`.

    for (uint8_t i = 0; i < _numDefs; ++i) {
        _gears[i].configure(_defs[i],
                            &GearControlServicePolicyT::sendRoleCmdTrampoline,
                            static_cast<void*>(_topo),
                            &GearControlServicePolicyT::beginBatchTrampoline,
                            &GearControlServicePolicyT::commitBatchTrampoline,
                            &GearControlServicePolicyT::phaseEventTrampoline,
                            static_cast<void*>(this));
    }
    claimPorts();
    _topo->onRoleEvent(&GearControlServicePolicyT::roleEventTrampoline,
                       static_cast<void*>(this));
    SFX_LOG_INFO("[gear-svc] ready (%u gears)", (unsigned)_numDefs);
    return true;
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::claimPorts() {
    using namespace sfx_core;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const GearDef& d = _defs[i];
        if (!_topo->claim(d.motor, EffectId::GearCtrl, RoleKind::BiDcMotor)) {
            SFX_LOG_WARN("[gear-svc] gear %u: motor claim failed", d.id);
        }
        for (uint8_t j = 0; j < d.numLeds; ++j) {
            if (!_topo->claim(d.leds[j], EffectId::GearCtrl,
                              RoleKind::LedAnimator)) {
                SFX_LOG_WARN("[gear-svc] gear %u: led claim failed", d.id);
            }
        }
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::update() {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < _numDefs; ++i) {
        _gears[i].update(now);
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
Gear* GearControlServicePolicyT<TTopology, TLandingService>::findById(uint8_t id) {
    for (uint8_t i = 0; i < _numDefs; ++i) {
        if (_gears[i].id() == id) return &_gears[i];
    }
    return nullptr;
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
CommandHandleResult GearControlServicePolicyT<TTopology, TLandingService>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case GearPacket::GEAR_DEPLOY:     handleDeploy(payload, len);    return CommandHandleResult::Handled;
        case GearPacket::GEAR_RETRACT:    handleRetract(payload, len);   return CommandHandleResult::Handled;
        case GearPacket::GEAR_STOP:       handleStop(payload, len);      return CommandHandleResult::Handled;
        case GearPacket::GEAR_ALL:        handleAll(payload, len);       return CommandHandleResult::Handled;
        case GearPacket::GEAR_STATUS_REQ: handleStatusReq();             return CommandHandleResult::Handled;
        case GearPacket::GEAR_LIST_REQ:   handleListReq();               return CommandHandleResult::Handled;
        default:                          return CommandHandleResult::NotMyCommand;
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleDeploy(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    if (g->phase() == GearPhase::Error) {
        _ctx->sendNack(GearError::IN_ERROR_STATE);
        return;
    }
    g->deploy();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleRetract(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    if (g->phase() == GearPhase::Error) {
        _ctx->sendNack(GearError::IN_ERROR_STATE);
        return;
    }
    g->retract();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStop(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    g->stop();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleAll(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t action = p[0];
    // One outer batch: every per-gear command falls into the same
    // wire burst so all gears start moving simultaneously.
    if (_topo) _topo->beginBatch();
    for (uint8_t i = 0; i < _numDefs; ++i) {
        switch (action) {
            case GearAllAction::Deploy:  _gears[i].deploy();  break;
            case GearAllAction::Retract: _gears[i].retract(); break;
            case GearAllAction::Stop:    _gears[i].stop();    break;
            default: /* ignore */        break;
        }
    }
    if (_topo) _topo->commitBatch();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStatusReq() {
    uint8_t buf[1 + kMaxGears * 2];
    buf[0] = _numDefs;
    size_t off = 1;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        buf[off++] = _gears[i].id();
        buf[off++] = _gears[i].phase();
    }
    _ctx->sendRawPacket(GearPacket::GEAR_STATUS_RESP,
                        _ctx->currentTag(), buf, off);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleListReq() {
    uint8_t buf[1 + kMaxGears * (1 + 1 + 16)];
    size_t off = 1;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const auto& d = _defs[i];
        if (off + 2 + 16 > sizeof(buf)) break;
        buf[off++] = d.id;
        uint8_t nlen = (uint8_t)std::strlen(d.name);
        if (nlen > 15) nlen = 15;
        buf[off++] = nlen;
        std::memcpy(&buf[off], d.name, nlen);
        off += nlen;
    }
    buf[0] = _numDefs;
    _ctx->sendRawPacket(GearPacket::GEAR_LIST_RESP,
                        _ctx->currentTag(), buf, off);
}

// ─── Role-event ingress ─────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::onRoleEvent(
        const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    if (innerType != RolePacket::MOTOR_STALL_EVENT &&
        innerType != RolePacket::BIMOTOR_STALL_EVENT) return;
    if (len < 1) return;
    const uint8_t portIdx = p[0];
    const bool guidEmpty  = !guid || guid[0] == 0;

    for (uint8_t i = 0; i < _numDefs; ++i) {
        const PortRef& m = _defs[i].motor;
        if (m.portIdx != portIdx) continue;
        const bool sameGuid =
            (m.guid[0] == 0 && guidEmpty) ||
            (m.guid[0] != 0 && guid && std::strcmp(m.guid, guid) == 0);
        if (sameGuid) {
            _gears[i].onMotorStall();
        }
    }
}

// ─── Phase-change fan-out ───────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::forwardToLandings(
        uint8_t newPhase) {
    if (!_landing) return;
    const uint8_t state =
        (newPhase == GearPhase::Deployed) ? hubfx::effects::landing::LandingLightState::On
                                          : hubfx::effects::landing::LandingLightState::Off;
    const uint8_t llCount = _landing->count();
    for (uint8_t i = 0; i < llCount; ++i) {
        auto* ll = _landing->at(i);
        if (!ll) continue;
        if (ll->owner() != EffectId::GearCtrl) continue;
        _landing->setState(ll->id(), state, EffectId::GearCtrl);
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::emitPhaseEvent(
        uint8_t id, uint8_t phase) {
    if (!_ctx) return;
    const uint8_t payload[2] = { id, phase };
    _ctx->sendRawPacket(GearPacket::GEAR_PHASE_EVENT,
                        SfxWire::TAG_ASYNC, payload, sizeof(payload));
    if (phase == GearPhase::Deployed || phase == GearPhase::Retracted) {
        forwardToLandings(phase);
    }
}

// ─── Trampolines ────────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
bool GearControlServicePolicyT<TTopology, TLandingService>::sendRoleCmdTrampoline(
        void* ctx, const PortRef& addr, uint8_t innerType,
        const uint8_t* p, size_t len) {
    return static_cast<TTopology*>(ctx)->sendRoleCommand(addr, innerType, p, len);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::beginBatchTrampoline(void* ctx) {
    if (auto* t = static_cast<TTopology*>(ctx)) t->beginBatch();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::commitBatchTrampoline(void* ctx) {
    if (auto* t = static_cast<TTopology*>(ctx)) t->commitBatch();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::phaseEventTrampoline(
        void* ctx, uint8_t id, uint8_t newPhase) {
    auto* self = static_cast<GearControlServicePolicyT*>(ctx);
    if (self) self->emitPhaseEvent(id, newPhase);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::roleEventTrampoline(
        void* ctx, const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    auto* self = static_cast<GearControlServicePolicyT*>(ctx);
    if (self) self->onRoleEvent(guid, innerType, p, len);
}

}  // namespace hubfx::effects::gearctrl

#endif  // HUBFX_GEARCONTROL_SERVICE_IPP
