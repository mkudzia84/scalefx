/*
 * gearcontrol_service.ipp — GearControl service template-method bodies.
 */

#ifndef HUBFX_GEARCONTROL_SERVICE_IPP
#define HUBFX_GEARCONTROL_SERVICE_IPP

#include <platform/sfx_platform.h>   // SFX_MILLIS()
#include <serial/wire.h>
#include <server/effect_clock.h>   // Rule 40 — effects use EffectClock, not raw SFX_MILLIS()
#include "../input/input_dispatcher.h"   // item 6 — connection-loss subscription

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

    // Item 6: subscribe to the InputDispatcher's connection-loss signal so an
    // input link dropping can emergency-deploy the gear.  The dispatcher is
    // templated on the SAME TTopology, so we can resolve it from the pack with
    // no extra template param.  Optional — if it isn't in the pack, the opt-in
    // is simply inert.
    auto* disp = ctx->template findPolicy<
        hubfx::effects::input::InputDispatcherServicePolicyT<TTopology>>();
    if (disp) {
        disp->onConnectionLoss(&GearControlServicePolicyT::connLossTrampoline,
                               static_cast<void*>(this));
    } else {
        SFX_LOG_WARN("[gear-svc] InputDispatcher not found — connection-loss deploy unavailable");
    }

    SFX_LOG_INFO("[gear-svc] ready (%u gears)", (unsigned)_numDefs);
    return true;
}

// ─── Connection-loss → emergency deploy (item 6) ────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::connLossTrampoline(
        void* ctx, const char* guid, uint8_t pk, uint8_t pi, uint8_t state) {
    auto* self = static_cast<GearControlServicePolicyT*>(ctx);
    if (self) self->onConnectionLoss(guid, pk, pi, state);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::onConnectionLoss(
        const char* guid, uint8_t /*portKind*/, uint8_t portIdx, uint8_t state) {
    if (state == 0) { _connLossLatched = false; return; }   // recovered — re-arm
    if (!_enabled || !_deployOnConnLoss) return;
    if (state != 2) return;                                  // act only on confirmed DOWN
    if (_connLossLatched) return;                            // one deploy per loss
    _connLossLatched = true;
    SFX_LOG_ERROR("[gear-svc] input link %s port=%u DOWN → EMERGENCY DEPLOY",
                  (guid && guid[0]) ? guid : "hub", portIdx);
    commandAll(/*deploy=*/1);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::applyDefs() {
    if (!_topo) return;   // begin() hasn't bound topology yet
    _seqActive  = 0xFF;
    _syncActive = false;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        // configure() resets each strut to UNKNOWN (initial-start: physical
        // position uncertain until the first command homes it).
        _gears[i].configure(_defs[i],
                            &GearControlServicePolicyT::sendRoleCmdTrampoline,
                            static_cast<void*>(_topo),
                            &GearControlServicePolicyT::beginBatchTrampoline,
                            &GearControlServicePolicyT::commitBatchTrampoline,
                            &GearControlServicePolicyT::phaseEventTrampoline,
                            static_cast<void*>(this));
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
    driveCoordinator();
    updateTransitSound();
}

// ─── Transit sounds ─────────────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::updateTransitSound() {
    if (!_audio) return;   // sketch never wired the mixer (no SFX_HAS_AUDIO)

    // Direction = the phase of any mid-transit gear (a mixed-direction set
    // keeps the loop of whichever direction we saw first — sane for the
    // realistic single-direction fleet command).
    bool anyMoving = false, deploying = false;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const uint8_t ph = _gears[i].phase();
        if (ph == GearPhase::Deploying)  { anyMoving = true; deploying = true;  break; }
        if (ph == GearPhase::Retracting) { anyMoving = true; deploying = false; break; }
    }
    // A Sequenced chain counts as "still moving" between gears — the
    // inter-gear handoff gap must not stop/restart the loop.
    const bool chainPending = (_coordMode == CoordMode::Sequenced && _seqActive != 0xFF);

    if (anyMoving && !_soundActive) {
        const char* path = deploying ? _deploySound : _retractSound;
        if (path[0]) {
            _audio(_audioCtx, path, _audioChannel, _soundMask, /*loop=*/true);
            _soundActive    = true;
            _soundDeploying = deploying;
        }
    } else if (_soundActive && anyMoving && deploying != _soundDeploying) {
        // Direction flipped mid-set (operator reversed the command): switch
        // the loop to the matching sample (PLAY replaces on the channel).
        const char* path = deploying ? _deploySound : _retractSound;
        if (path[0]) {
            _audio(_audioCtx, path, _audioChannel, _soundMask, /*loop=*/true);
            _soundDeploying = deploying;
        } else {
            _audio(_audioCtx, nullptr, _audioChannel, 0, false);
            _soundActive = false;
        }
    } else if (_soundActive && !anyMoving && !chainPending) {
        _audio(_audioCtx, nullptr, _audioChannel, 0, false);   // settled — stop
        _soundActive = false;
    }
}

// ─── Multi-gear coordinator ─────────────────────────────────────────

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::driveCoordinator() {
    // Sequenced: advance the chain when the active strut reaches a terminal
    // state — atTarget OR a faulted/held strut — so one stuck strut can't stall
    // the whole gear; the rest keeps deploying past the skipped leg.
    if (_coordMode == CoordMode::Sequenced) {
        if (_seqActive != 0xFF && _seqActive < _numDefs) {
            const uint8_t ph = _gears[_seqActive].phase();
            if (_gears[_seqActive].atTarget() ||
                ph == GearPhase::Error || ph == GearPhase::Held) {
                sequencedKick();
            }
        }
        return;
    }
    driveSyncStep();   // DoorSync / FullSync — no-op when !_syncActive
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::driveSyncStep() {
    if (!_syncActive) return;

    // A barrier walk: keep every strut in lockstep by stepping them all to the
    // next leg only once ALL have reached the current boundary (legComplete).
    bool allSettled  = true;
    bool allComplete = true;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        if (_gears[i].atTarget()) continue;
        allSettled = false;
        if (!_gears[i].legComplete()) allComplete = false;
    }
    if (allSettled)  { _syncActive = false; return; }   // whole set reached the target
    if (!allComplete) return;                            // wait at the barrier

    if (_topo) _topo->beginBatch();
    for (uint8_t i = 0; i < _numDefs; ++i) {
        if (_gears[i].atTarget()) continue;
        // DoorSync syncs only the doors-open boundary: once the doors are open
        // together, release each strut to finish its strut+close on its own.
        if (_coordMode == CoordMode::DoorSync &&
            _gears[i].subPhase() == GearSubPhase::doors_open) {
            _gears[i].setTarget(_syncTarget);
        } else {
            _gears[i].stepToward(_syncTarget);   // FullSync: lockstep every leg
        }
    }
    if (_topo) _topo->commitBatch();
    if (_coordMode == CoordMode::DoorSync) _syncActive = false;   // released to free-run
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::sequencedKick() {
    // Advance to the next strut after the current one reached its target.
    uint8_t next = (_seqActive == 0xFF) ? 0 : (uint8_t)(_seqActive + 1);
    if (next >= _numDefs) { _seqActive = 0xFF; return; }   // chain done
    _seqActive = next;
    if (_topo) _topo->beginBatch();
    _gears[next].setTarget(_seqTarget);
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
        case GearPacket::GEAR_ESTOP:      handleEstop(payload, len);     return CommandHandleResult::Handled;
        case GearPacket::GEAR_STEP:       handleStep(payload, len);      return CommandHandleResult::Handled;
        case GearPacket::GEAR_DOOR:       handleDoor(payload, len);      return CommandHandleResult::Handled;
        case GearPacket::GEAR_STRUT:      handleStrut(payload, len);     return CommandHandleResult::Handled;
        case GearPacket::GEAR_DOOR_ALL:   handleDoorAll(payload, len);   return CommandHandleResult::Handled;
        case GearPacket::GEAR_STRUT_ALL:  handleStrutAll(payload, len);  return CommandHandleResult::Handled;
        default:                          return CommandHandleResult::NotMyCommand;
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleDeploy(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    // Item 4: a deploy/retract from the effect auto-clears a prior fault and
    // retries (no manual GEAR_RESET needed) — clearError() is a no-op when not
    // in Error.  Per-strut bench command: full-cycle toward Down, independent
    // of the global coord mode (the coordinator's barrier walk is fleet-only).
    g->clearError();
    g->setTarget(Gear::Target::Down);
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleRetract(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    g->clearError();                    // item 4: auto-clear a prior fault + retry
    g->setTarget(Gear::Target::Up);     // per-strut full-cycle toward Up
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
    // STOP halts the strut in place (brake + freeze) — the per-strut emergency
    // hold.  Resumes on the next deploy/retract.
    g->emergencyHold();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleEstop(
        const uint8_t* p, size_t len) {
    // `[]` (whole set) or `[id]` (one strut) → emergency hold (brake + freeze).
    if (len >= 1) {
        Gear* g = findById(p[0]);
        if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
        g->emergencyHold();
    } else {
        emergencyHoldAll();
    }
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStep(
        const uint8_t* p, size_t len) {
    // `[id][target]` — advance ONE leg toward target (0=up, 1=down) then park.
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    const Gear::Target t = (p[1] == GearStepTarget::Down) ? Gear::Target::Down
                                                          : Gear::Target::Up;
    g->clearError();              // item 4: a manual step also retries a fault
    g->stepToward(t);             // single-step: cross one boundary, then park
    _ctx->sendAck();
}

// ── Manual / maintenance: door-only + strut-only, per-leg + fleet ────────
//   The Gear FSM is the authoritative interlock; here we just map its
//   ManualResult onto an ACK or a NACK with the matching GearError.

static inline uint8_t manualNackCode(Gear::ManualResult r) {
    switch (r) {
        case Gear::ManualResult::DoorsClosed: return GearError::DOORS_CLOSED;
        case Gear::ManualResult::StrutNotUp:  return GearError::STRUT_NOT_UP;
        default:                              return GearError::GEAR_BUSY;
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleDoor(
        const uint8_t* p, size_t len) {
    // `[id][open]` — open(1)/close(0) ONE strut's doors (firmware interlock).
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    const Gear::ManualResult r = g->manualDoors(p[1] == GearDoorAction::Open);
    if (r == Gear::ManualResult::Ok) _ctx->sendAck();
    else                             _ctx->sendNack(manualNackCode(r));
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStrut(
        const uint8_t* p, size_t len) {
    // `[id][down]` — deploy(1)/retract(0) ONE strut's motor (firmware interlock).
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    Gear* g = findById(p[0]);
    if (!g) { _ctx->sendNack(GearError::UNKNOWN_ID); return; }
    const Gear::ManualResult r = g->manualStrut(p[1] == GearStrutAction::Deploy);
    if (r == Gear::ManualResult::Ok) _ctx->sendAck();
    else                             _ctx->sendNack(manualNackCode(r));
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleDoorAll(
        const uint8_t* p, size_t len) {
    // `[open]` — every strut's doors, ALL-OR-NOTHING: dry-run the interlock on
    // all legs first; refuse the whole command if any leg would be unsafe.
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const bool open = (p[0] == GearDoorAction::Open);
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const Gear::ManualResult c = _gears[i].checkManualDoors(open);
        if (c != Gear::ManualResult::Ok) { _ctx->sendNack(manualNackCode(c)); return; }
    }
    if (_topo) _topo->beginBatch();
    for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].manualDoors(open);
    if (_topo) _topo->commitBatch();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStrutAll(
        const uint8_t* p, size_t len) {
    // `[down]` — every strut's motor, ALL-OR-NOTHING (see handleDoorAll).
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const bool down = (p[0] == GearStrutAction::Deploy);
    for (uint8_t i = 0; i < _numDefs; ++i) {
        const Gear::ManualResult c = _gears[i].checkManualStrut(down);
        if (c != Gear::ManualResult::Ok) { _ctx->sendNack(manualNackCode(c)); return; }
    }
    if (_topo) _topo->beginBatch();
    for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].manualStrut(down);
    if (_topo) _topo->commitBatch();
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::emergencyHoldAll() {
    if (_topo) _topo->beginBatch();
    for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].emergencyHold();
    if (_topo) _topo->commitBatch();
    _seqActive  = 0xFF;
    _syncActive = false;
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::commandAll(
        uint8_t action) {
    if (action == GearAllAction::Stop) { emergencyHoldAll(); return; }
    commandAllTarget(action == GearAllAction::Deploy ? Gear::Target::Down
                                                     : Gear::Target::Up);
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::commandAllTarget(
        Gear::Target t) {
    _seqActive  = 0xFF;
    _syncActive = false;

    // Item 4: a fleet gear-up/down auto-clears any strut sitting in Error and
    // retries it.  Done as a pre-pass (each clearError() runs its own wire
    // batch) BEFORE the command batch below so the batches don't nest.
    for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].clearError();

    switch (_coordMode) {
        case CoordMode::Sequenced:
            // One strut at a time — start strut[0]; driveCoordinator() chains.
            _seqTarget = t;
            _seqActive = 0xFF;          // sequencedKick() advances to 0
            sequencedKick();
            return;

        case CoordMode::DoorSync:
        case CoordMode::FullSync:
            // Barrier walk: step every strut to its first boundary (doors open),
            // then driveSyncStep() advances them in lockstep.
            _syncTarget = t;
            _syncActive = true;
            if (_topo) _topo->beginBatch();
            for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].stepToward(t);
            if (_topo) _topo->commitBatch();
            return;

        case CoordMode::Independent:
        default:
            // Each strut runs its own full cycle (and pre-empts independently).
            if (_topo) _topo->beginBatch();
            for (uint8_t i = 0; i < _numDefs; ++i) _gears[i].setTarget(t);
            if (_topo) _topo->commitBatch();
            return;
    }
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleAll(
        const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t action = p[0];
    if (action != GearAllAction::Stop && action != GearAllAction::Deploy &&
        action != GearAllAction::Retract) {
        _ctx->sendNack(SerialError::INVALID_PARAM);
        return;
    }
    commandAll(action);
    _ctx->sendAck();
}

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
void GearControlServicePolicyT<TTopology, TLandingService>::handleStatusReq() {
    // Rule 11 append: per-entry [id][phase][subPhase][errReason][doorsOpen]
    // [strutState] — old clients read the first 2; the host derives the stride
    // from count.  doorsOpen + strutState drive the manual-control interlock gate.
    uint8_t buf[1 + kMaxGears * 6];
    buf[0] = _numDefs;
    size_t off = 1;
    for (uint8_t i = 0; i < _numDefs; ++i) {
        buf[off++] = _gears[i].id();
        buf[off++] = _gears[i].phase();
        buf[off++] = _gears[i].subPhase();
        buf[off++] = _gears[i].errorReason();
        buf[off++] = _gears[i].doorsOpen() ? 1 : 0;
        buf[off++] = _gears[i].strutState();
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
        uint8_t id, uint8_t phase, uint8_t subPhase, uint8_t errReason) {
    if (!_ctx) return;
    // Rule 11 append: [id][phase][subPhase][errReason][doorsOpen][strutState] —
    // old clients read 2.  doorsOpen/strutState (looked up by id since the
    // phase-callback doesn't carry them) drive the manual-control interlock gate.
    uint8_t doorsOpen = 0, strutState = GearStrutState::Unknown;
    if (Gear* g = findById(id)) { doorsOpen = g->doorsOpen() ? 1 : 0; strutState = g->strutState(); }
    const uint8_t payload[6] = { id, phase, subPhase, errReason, doorsOpen, strutState };
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
        void* ctx, uint8_t id, uint8_t newPhase, uint8_t newSubPhase, uint8_t errReason) {
    auto* self = static_cast<GearControlServicePolicyT*>(ctx);
    if (self) self->emitPhaseEvent(id, newPhase, newSubPhase, errReason);
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
