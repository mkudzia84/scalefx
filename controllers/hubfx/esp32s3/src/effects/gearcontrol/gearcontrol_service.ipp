/*
 * gearcontrol_service.ipp — GearControl service template-method bodies.
 */

#ifndef HUBFX_GEARCONTROL_SERVICE_IPP
#define HUBFX_GEARCONTROL_SERVICE_IPP

#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <serial/wire.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw SFX_MILLIS()

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

    // Bind gear FSMs + claim motor ports for whatever defs exist now
    // (none at boot — the YAML config chain calls configure() later,
    // which re-runs applyDefs() with the real defs).
    applyDefs();
    _topo->onRoleEvent(&GearControlServicePolicyT::roleEventTrampoline,
                       static_cast<void*>(this));
    SFX_LOG_INFO("[gear-svc] ready (%u gears)", (unsigned)_numDefs);
    return true;
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::applyDefs() {
    if (!_topo) return;   // begin() hasn't bound topology yet
    _seqActive = 0xFF;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        _gears[i].configure(_defs[i],
                            &GearControlServicePolicyT::sendRoleCmdTrampoline,
                            static_cast<void*>(_topo),
                            &GearControlServicePolicyT::beginBatchTrampoline,
                            &GearControlServicePolicyT::commitBatchTrampoline,
                            &GearControlServicePolicyT::phaseEventTrampoline,
                            static_cast<void*>(this));
        applySyncFlags(_gears[i]);
    }
    claimPorts();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::claimPorts() {
    using namespace sfx_core;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const GearDef& d = _defs[i];
        if (!_topo->claim(d.motor, EffectId::GearCtrl, RoleKind::BiDcMotor)) {
            SFX_LOG_WARN("[gear-svc] gear %u: motor claim failed", d.id);
        }
        // Status LEDs are NOT claimed/driven here — the GearControl
        // expander lights them locally from its H-bridge state.
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::update() {
    const uint32_t now = sfx_core::EffectClock::instance().nowMs();
    for (uint8_t i = 0; i < _numDefs; ++i) {
        _gears[i].update(now);
    }
    releaseBarriersIfReady();
}

// ─── Multi-gear coordinator ─────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::applySyncFlags(Gear& g) const {
    switch (_coordMode) {
        case CoordMode::DoorSync:  g.setSyncBarriers(/*doors=*/true,  /*motor=*/false); break;
        case CoordMode::FullSync:  g.setSyncBarriers(/*doors=*/true,  /*motor=*/true);  break;
        case CoordMode::Independent:
        case CoordMode::Sequenced:
        default:                   g.setSyncBarriers(/*doors=*/false, /*motor=*/false); break;
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::releaseBarriersIfReady() {
    // DoorSync / FullSync: advance every mid-cycle gear past a barrier once
    // ALL mid-cycle gears sit at that same barrier (port of the archive's
    // releaseSyncBarriersIfReady).  Independent/Sequenced have no barriers.
    if (_coordMode == CoordMode::DoorSync || _coordMode == CoordMode::FullSync) {
        bool anyCycling   = false;
        bool allAtDoors   = true;
        bool allAtMotor   = true;
        for (uint8_t i = 0; i < _numDefs; ++i) {
            if (!_gears[i].isCycling()) continue;
            anyCycling = true;
            if (!_gears[i].isWaitingDoorsOpenBarrier()) allAtDoors = false;
            if (!_gears[i].isWaitingMotorDoneBarrier()) allAtMotor = false;
        }
        if (anyCycling && allAtDoors) {
            if (_topo) _topo->beginBatch();
            for (uint8_t i = 0; i < _numDefs; ++i)
                if (_gears[i].isCycling()) _gears[i].advanceBarrier();
            if (_topo) _topo->commitBatch();
        }
        if (anyCycling && allAtMotor) {
            if (_topo) _topo->beginBatch();
            for (uint8_t i = 0; i < _numDefs; ++i)
                if (_gears[i].isCycling()) _gears[i].advanceBarrier();
            if (_topo) _topo->commitBatch();
        }
        return;
    }

    // Sequenced: when the active gear settles, kick the next one.
    if (_coordMode == CoordMode::Sequenced && _seqActive != 0xFF) {
        if (_seqActive < _numDefs && !_gears[_seqActive].isCycling()) {
            sequencedKick();
        }
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::sequencedKick() {
    // Advance to the next gear after the current one finished its full cycle.
    uint8_t next = (_seqActive == 0xFF) ? 0 : (uint8_t)(_seqActive + 1);
    if (next >= _numDefs) { _seqActive = 0xFF; return; }   // chain done
    _seqActive = next;
    applySyncFlags(_gears[next]);   // (no barriers in Sequenced, but keep it explicit)
    if (_topo) _topo->beginBatch();
    if (_seqDeploying) _gears[next].deploy();
    else               _gears[next].retract();
    if (_topo) _topo->commitBatch();
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
        case GearPacket::GEAR_RESET:      handleReset(payload, len);     return CommandHandleResult::Handled;
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
    // Per-gear deploy is independent of cross-channel sync (bench testing) —
    // run without barriers regardless of the global coord mode.
    g->setSyncBarriers(false, false);
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
    g->setSyncBarriers(false, false);   // per-gear command is independent
    g->retract();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleReset(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    g->clearError();          // no-op if not in ERROR
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

    // STOP is universal — halt every gear + reset the sequenced chain.
    if (action == GearAllAction::Stop) {
        if (_topo) _topo->beginBatch();
        for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].stop();
        if (_topo) _topo->commitBatch();
        _seqActive = 0xFF;
        _ctx->sendAck();
        return;
    }

    const bool deploying = (action == GearAllAction::Deploy);
    if (action != GearAllAction::Deploy && action != GearAllAction::Retract) {
        _ctx->sendNack(SerialError::INVALID_PARAM);
        return;
    }

    // Sequenced: kick only gear[0]; releaseBarriersIfReady() chains the rest.
    if (_coordMode == CoordMode::Sequenced) {
        _seqDeploying = deploying;
        _seqActive    = 0xFF;        // sequencedKick() advances to 0
        sequencedKick();
        _ctx->sendAck();
        return;
    }

    // Independent / DoorSync / FullSync: start every gear in one wire burst.
    // Sync flags inserted per coord mode so the coordinator can hold the
    // barriers; Independent leaves them off (today's behaviour).
    if (_topo) _topo->beginBatch();
    for (uint8_t i = 0; i < _numDefs; ++i) {
        applySyncFlags(_gears[i]);
        if (deploying) _gears[i].deploy();
        else           _gears[i].retract();
    }
    if (_topo) _topo->commitBatch();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStatusReq() {
    // Rule 11 append: per-entry [id][phase][subPhase] — old clients read 2.
    uint8_t buf[1 + kMaxGears * 3];
    buf[0] = _numDefs;
    size_t off = 1;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        buf[off++] = _gears[i].id();
        buf[off++] = _gears[i].phase();
        buf[off++] = _gears[i].subPhase();
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
    const bool guidEmpty = !guid || guid[0] == 0;
    auto guidMatches = [&](const PortRef& ref) -> bool {
        return (ref.guid[0] == 0 && guidEmpty) ||
               (ref.guid[0] != 0 && guid && std::strcmp(ref.guid, guid) == 0);
    };

    // ── Motor leg completion ──────────────────────────────────────────
    // Gear motion uses the BiDcMotor endstop seek, so the motor-leg async is
    // BIMOTOR_ENDSTOP_RESULT ([portIdx][outcome][travel:u16][peak:u16]).
    if (innerType == RolePacket::BIMOTOR_ENDSTOP_RESULT) {
        if (len < 2) return;
        const uint8_t portIdx = p[0];
        const uint8_t outcome = p[1];
        for (uint8_t i = 0; i < _numDefs; ++i) {
            if (_defs[i].motor.portIdx == portIdx && guidMatches(_defs[i].motor)) {
                _gears[i].onEndstopResult(outcome);
            }
        }
        return;
    }

    // ── Door leg completion ───────────────────────────────────────────
    // Door servos report SERVO_MOTION_DONE ([portIdx]) on the rising edge of
    // their motion profile reaching target (monitored, decision #1).  Route
    // to the gear whose door PortRef matches (board GUID + portIdx).
    if (innerType == RolePacket::SERVO_MOTION_DONE) {
        if (len < 1) return;
        const uint8_t portIdx = p[0];
        for (uint8_t i = 0; i < _numDefs; ++i) {
            for (uint8_t d = 0; d < _defs[i].numDoors; ++d) {
                const PortRef& s = _defs[i].doors[d].servo;
                if (s.portIdx == portIdx && guidMatches(s)) {
                    _gears[i].onServoMotionDone(portIdx);
                }
            }
        }
        return;
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
        uint8_t id, uint8_t phase, uint8_t subPhase) {
    if (!_ctx) return;
    // Rule 11 append: [id][phase][subPhase] — old clients read the first 2.
    const uint8_t payload[3] = { id, phase, subPhase };
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
        void* ctx, uint8_t id, uint8_t newPhase, uint8_t newSubPhase) {
    auto* self = static_cast<GearControlServicePolicyT*>(ctx);
    if (self) self->emitPhaseEvent(id, newPhase, newSubPhase);
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
