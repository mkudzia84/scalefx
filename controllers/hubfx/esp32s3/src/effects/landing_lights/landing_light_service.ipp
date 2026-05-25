/*
 * landing_light_service.ipp — template-method definitions.
 *
 *   Included at the bottom of landing_light_service.h.
 */

#ifndef HUBFX_LANDING_LIGHT_SERVICE_IPP
#define HUBFX_LANDING_LIGHT_SERVICE_IPP

#include <serial/wire.h>      // SfxWire::TAG_ASYNC

namespace hubfx::effects::landing {

// ─── Lifecycle ──────────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
bool LandingLightServicePolicyT<TTopology>::begin(sfx_core::BoardServerBase* ctx) {
    _ctx = ctx;
    if (!_ctx) return false;

    _topology = ctx->template findPolicy<TTopology>();
    if (!_topology) {
        SFX_LOG_ERROR("[ll-svc] TopologyService not found in policy pack");
        return false;
    }

    // Bind instances + claim ports for whatever defs exist now (none at
    // boot — the YAML config chain calls configure() later, which re-runs
    // applyDefs() with the real defs).
    applyDefs();

    // Subscribe to every role async event so we can advance the state
    // machine on SERVO_TARGET_REACHED.  Single-slot subscription on
    // the topology side — only one consumer today.
    _topology->onRoleEvent(&LandingLightServicePolicyT::roleEventTrampoline,
                           static_cast<void*>(this));
    return true;
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::applyDefs() {
    if (!_topology) return;   // begin() hasn't bound topology yet
    for (uint8_t i = 0; i < _numDefs; ++i) {
        _instances[i].configure(_defs[i],
                                &LandingLightServicePolicyT::sendRoleCmdTrampoline,
                                static_cast<void*>(_topology),
                                &LandingLightServicePolicyT::beginBatchTrampoline,
                                &LandingLightServicePolicyT::commitBatchTrampoline,
                                &LandingLightServicePolicyT::phaseEventTrampoline,
                                static_cast<void*>(this));
    }
    claimPorts();
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::claimPorts() {
    if (!_topology) return;
    using namespace sfx_core;

    for (uint8_t i = 0; i < _numDefs; ++i) {
        const LandingLightDef& d = _defs[i];
        // Claim every servo in the group (multi-servo support 2026-05-24).
        for (uint8_t j = 0; j < d.numServos; ++j) {
            const PortRef& s = d.servos[j];
            if (!_topology->claim(s, EffectId::LandingLight,
                                  RoleKind::ServoActuator)) {
                SFX_LOG_WARN("[ll-svc] ll=%u: servo[%u] claim failed (%s:%u)",
                             d.id, (unsigned)j,
                             s.guid[0] ? s.guid : "hub",
                             (unsigned)s.portIdx);
            }
        }
        // Claim each LED.
        for (uint8_t j = 0; j < d.numLeds; ++j) {
            if (!_topology->claim(d.leds[j], EffectId::LandingLight,
                                  RoleKind::LedAnimator)) {
                SFX_LOG_WARN("[ll-svc] ll=%u: led claim failed (%s:%u)",
                             d.id,
                             d.leds[j].guid[0] ? d.leds[j].guid : "hub",
                             (unsigned)d.leds[j].portIdx);
            }
        }
    }
}

// ─── Effects-facing API ─────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
bool LandingLightServicePolicyT<TTopology>::setState(uint8_t id,
                                                     uint8_t state,
                                                     EffectId caller) {
    LandingLight* ll = findById(id);
    if (!ll) {
        SFX_LOG_WARN("[ll-svc] setState: unknown id %u", id);
        return false;
    }
    if (ll->owner() != caller) {
        SFX_LOG_WARN("[ll-svc] setState %u: caller %s doesn't own this LL (owner=%s)",
                     id, effectIdName(caller), effectIdName(ll->owner()));
        return false;
    }
    ll->setState(state);
    return true;
}

template <hubfx::topology::TopologyService TTopology>
LandingLight* LandingLightServicePolicyT<TTopology>::findById(uint8_t id) {
    for (uint8_t i = 0; i < _numDefs; ++i) {
        if (_instances[i].id() == id) return &_instances[i];
    }
    return nullptr;
}

// ─── Wire dispatch ──────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
CommandHandleResult LandingLightServicePolicyT<TTopology>::handle(
        uint8_t type, const uint8_t* payload, size_t len) {
    switch (type) {
        case LandingLightPacket::LL_LIST_REQ:
            handleListReq();
            return CommandHandleResult::Handled;
        case LandingLightPacket::LL_STATUS_REQ:
            handleStatusReq();
            return CommandHandleResult::Handled;
        case LandingLightPacket::LL_SET_STATE:
            handleSetState(payload, len);
            return CommandHandleResult::Handled;
        default:
            return CommandHandleResult::NotMyCommand;
    }
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::handleListReq() {
    uint8_t buf[1 + kMaxLandingLights * (1 + 1 + 16 + 1 + 1)];
    size_t  off = 1;
    uint8_t cnt = 0;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const auto& d = _defs[i];
        if (off + 4 + 16 > sizeof(buf)) break;
        buf[off++] = d.id;
        uint8_t nlen = (uint8_t)std::strlen(d.name);
        if (nlen > 15) nlen = 15;
        buf[off++] = nlen;
        std::memcpy(&buf[off], d.name, nlen); off += nlen;
        buf[off++] = static_cast<uint8_t>(d.owner);
        buf[off++] = _instances[i].phase();
        ++cnt;
    }
    buf[0] = cnt;
    _ctx->sendRawPacket(LandingLightPacket::LL_LIST_RESP,
                        _ctx->currentTag(), buf, off);
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::handleStatusReq() {
    uint8_t buf[1 + kMaxLandingLights * 2];
    buf[0] = _numDefs;
    size_t off = 1;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        buf[off++] = _instances[i].id();
        buf[off++] = _instances[i].phase();
    }
    _ctx->sendRawPacket(LandingLightPacket::LL_STATUS_RESP,
                        _ctx->currentTag(), buf, off);
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::handleSetState(
        const uint8_t* p, size_t len) {
    if (len < 2) {
        _ctx->sendNack(SerialError::MISSING_PARAMETER);
        return;
    }
    const uint8_t id    = p[0];
    const uint8_t state = p[1];
    LandingLight* ll = findById(id);
    if (!ll) {
        _ctx->sendNack(LandingLightError::UNKNOWN_ID);
        return;
    }
    // Wire-side SET_STATE bypasses ownership — the operator on the
    // other end of the COBS link is authoritative.  Cross-effect
    // ownership only matters for in-process callers (programs).
    ll->setState(state);
    _ctx->sendAck();
}

// ─── Async event handling ───────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::onRoleEvent(
        const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    if (innerType != RolePacket::SERVO_TARGET_REACHED) return;
    if (len < 1) return;
    const uint8_t portIdx = p[0];
    const bool guidEmpty  = !guid || guid[0] == 0;

    // Multi-servo support (2026-05-24): walk every landing light's
    // `servos[]` block and forward the event with the SLOT INDEX so
    // the state machine can mark the right servo's arrival in its
    // bitset.  A single physical servo can only belong to ONE landing
    // light (the apply translator validates this), so we keep walking
    // after a match in the unlikely case the operator's config has
    // multiple bindings — only the right one will fire.
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const LandingLightDef& def = _defs[i];
        for (uint8_t j = 0; j < def.numServos; ++j) {
            const PortRef& s = def.servos[j];
            if (s.portIdx != portIdx) continue;
            const bool sameGuid =
                (s.guid[0] == 0 && guidEmpty) ||
                (s.guid[0] != 0 && guid && std::strcmp(s.guid, guid) == 0);
            if (sameGuid) {
                _instances[i].onServoTargetReached(j);
                break;          // next landing light
            }
        }
    }
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::roleEventTrampoline(
        void* ctx, const char* guid, uint8_t innerType,
        const uint8_t* p, size_t len) {
    auto* self = static_cast<LandingLightServicePolicyT*>(ctx);
    if (self) self->onRoleEvent(guid, innerType, p, len);
}

// ─── Trampolines + helpers ──────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology>
bool LandingLightServicePolicyT<TTopology>::sendRoleCmdTrampoline(
        void* ctx, const PortRef& addr, uint8_t innerType,
        const uint8_t* p, size_t len) {
    auto* topo = static_cast<TTopology*>(ctx);
    return topo->sendRoleCommand(addr, innerType, p, len);
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::beginBatchTrampoline(void* ctx) {
    auto* topo = static_cast<TTopology*>(ctx);
    if (topo) topo->beginBatch();
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::commitBatchTrampoline(void* ctx) {
    auto* topo = static_cast<TTopology*>(ctx);
    if (topo) topo->commitBatch();
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::phaseEventTrampoline(
        void* ctx, uint8_t id, uint8_t newPhase) {
    auto* self = static_cast<LandingLightServicePolicyT*>(ctx);
    self->emitPhaseEvent(id, newPhase);
}

template <hubfx::topology::TopologyService TTopology>
void LandingLightServicePolicyT<TTopology>::emitPhaseEvent(uint8_t id,
                                                           uint8_t phase) {
    if (!_ctx) return;
    const uint8_t payload[2] = { id, phase };
    _ctx->sendRawPacket(LandingLightPacket::LL_PHASE_EVENT,
                        SfxWire::TAG_ASYNC, payload, sizeof(payload));
}

}  // namespace hubfx::effects::landing

#endif  // HUBFX_LANDING_LIGHT_SERVICE_IPP
