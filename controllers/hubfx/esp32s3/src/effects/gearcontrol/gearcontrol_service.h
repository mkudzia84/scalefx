/*
 * gearcontrol_service.h — `GearControlServicePolicyT<TTopology, TLandingService>`.
 *
 *   Master-side gear effect.  Slotted into the HubFX BoardOf<...>
 *   pack after the topology service and (if used) the landing-light
 *   service.  Owns the gear registry, claims motor + LED ports for
 *   `EffectId::GearCtrl`, dispatches the GEAR_* wire surface, and
 *   subscribes to topology role events so MOTOR_STALL_EVENT can
 *   advance each gear's state machine.
 *
 *   Cross-effect integration: every successful DEPLOYED ↔ RETRACTED
 *   transition forwards `setState()` to `LandingLightService` for
 *   landing lights whose owner is `EffectId::GearCtrl` — the gear-
 *   bound landing-light story.
 */

#ifndef HUBFX_GEARCONTROL_SERVICE_H
#define HUBFX_GEARCONTROL_SERVICE_H

#include <cstdint>
#include <cstring>

#include <serial/core/core.h>
#include <serial/diag_log.h>
#include <serial/roles.h>
#include <server/board_server.h>

#include "../effect_id.h"
#include "../../topology/topology_service.h"            // TopologyService concept
#include "../landing_lights/landing_light_service.h"    // LandingLightService concept
#include "gear.h"
#include "gearcontrol_protocol.h"

namespace hubfx::effects::gearctrl {

inline constexpr uint8_t kMaxGears = 6;

template <hubfx::topology::TopologyService TTopology, hubfx::effects::landing::LandingLightService TLandingService>
class GearControlServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits = CoreCapability::GEARCTRL;

    GearControlServicePolicyT() = default;

    void configure(const GearDef* defs, uint8_t count, uint8_t coordMode = CoordMode::Independent) {
        _numDefs = 0;
        for (uint8_t i = 0; i < count && i < kMaxGears; ++i) {
            _defs[_numDefs++] = defs[i];
        }
        _coordMode = coordMode;
        applyDefs();   // (re)bind gear FSMs + claim motor ports if topology is up
    }

    /// Runtime-enable flag — see LandingLightService for rationale.
    void setEnabled(bool v) { _enabled = v; }
    bool enabled() const    { return _enabled; }

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == GearPacket::GEAR_DEPLOY
            || type == GearPacket::GEAR_RETRACT
            || type == GearPacket::GEAR_STOP
            || type == GearPacket::GEAR_ALL
            || type == GearPacket::GEAR_STATUS_REQ
            || type == GearPacket::GEAR_LIST_REQ
            || type == GearPacket::GEAR_RESET;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    /// Tick every gear (drives the timeout-to-ERROR path).
    void update();

    const char* getErrorMessage(uint8_t code) const {
        return GearError::getMessage(code);
    }

    // ── Direct API (for board sketches or other effects) ─────────────

    Gear*   findById(uint8_t id);
    uint8_t count() const { return _numDefs; }

private:
    // (Re)bind every gear FSM to its def + claim its motor port.  No-op
    // until `_topo` is bound.  Called from BOTH begin() and configure()
    // so it runs once defs + topology both exist, regardless of order;
    // idempotent on config reload.
    void applyDefs();
    void claimPorts();

    void handleDeploy     (const uint8_t* p, size_t len);
    void handleRetract    (const uint8_t* p, size_t len);
    void handleStop       (const uint8_t* p, size_t len);
    void handleAll        (const uint8_t* p, size_t len);
    void handleStatusReq  ();
    void handleListReq    ();
    void handleReset      (const uint8_t* p, size_t len);

    void onRoleEvent(const char* guid, uint8_t innerType,
                     const uint8_t* p, size_t len);

    /// Multi-gear coordinator (instructions/29 decision #2): release the
    /// sync barriers when all mid-cycle gears sit at the same barrier, and
    /// drive the Sequenced one-gear-at-a-time chain.  Called from update().
    void releaseBarriersIfReady();

    /// Per-cycle sync flags for a gear, derived from `_coordMode`.
    void applySyncFlags(Gear& g) const;

    /// Sequenced mode: index of the gear currently running its full cycle
    /// (0xFF = none).  Advanced as each gear settles.
    void sequencedKick();

    static bool sendRoleCmdTrampoline(void* ctx, const PortRef& addr,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);
    static void beginBatchTrampoline (void* ctx);
    static void commitBatchTrampoline(void* ctx);
    static void phaseEventTrampoline (void* ctx, uint8_t id,
                                      uint8_t newPhase, uint8_t newSubPhase);
    static void roleEventTrampoline  (void* ctx, const char* guid,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);

    // Fan-out helper: on a Deployed/Retracted transition, push the
    // matching state to every gear-bound landing light.
    void forwardToLandings(uint8_t newPhase);

    void emitPhaseEvent(uint8_t id, uint8_t phase, uint8_t subPhase);

    sfx_core::BoardServerBase* _ctx     = nullptr;
    TTopology*                 _topo    = nullptr;
    TLandingService*           _landing = nullptr;

    GearDef _defs[kMaxGears]      = {};
    Gear    _gears[kMaxGears]     = {};
    uint8_t _numDefs              = 0;
    bool    _enabled              = true;     // runtime enable flag (config-driven)
    uint8_t _coordMode            = CoordMode::Independent;
    uint8_t _seqActive            = 0xFF;     // Sequenced: gear index mid-cycle
    bool    _seqDeploying         = false;    // Sequenced: direction of the chain
};

}  // namespace hubfx::effects::gearctrl

#include "gearcontrol_service.ipp"

#endif  // HUBFX_GEARCONTROL_SERVICE_H
