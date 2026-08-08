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

    /// Opt-in (item 6): when an input link drops (the InputDispatcher's
    /// CONNECTION DOWN signal), emergency-deploy the whole undercarriage.
    /// Distinct from the per-channel RC-loss failsafe — this fires on the
    /// whole input LINK dying (e.g. the Jeti UART), not just the gear
    /// up/down channel.  Set from /gearcontrol.yaml on every reload.
    void setDeployOnConnectionLoss(bool v) { _deployOnConnLoss = v; }
    bool deployOnConnectionLoss() const    { return _deployOnConnLoss; }

    // ── Transit sounds (optional) ────────────────────────────────────
    /// Audio command hook — same shape as GunFx's AudioCmdFn so the sketch
    /// wires one trampoline into the board mixer.  `soundPath == nullptr`
    /// stops the channel.  Injected (not a TMixer template param) so the
    /// service keeps its two-parameter signature.
    using AudioCmdFn = void (*)(void* ctx, const char* soundPath,
                                uint8_t audioChannel, uint8_t outputMask,
                                bool loop);

    /// Wire the mixer trampoline + the board's Gear audio channel
    /// (HubFxLayout::Gear — passed in so this header stays board-agnostic).
    void bindAudio(AudioCmdFn fn, void* ctx, uint8_t audioChannel) {
        _audio        = fn;
        _audioCtx     = ctx;
        _audioChannel = audioChannel;
    }

    /// Per-direction transit-loop paths (empty/null = silent) + stereo mask.
    /// Called from the /gearcontrol.yaml apply on every CONFIG_RELOAD.
    void setSounds(const char* deploySound, const char* retractSound,
                   uint8_t outputMask) {
        std::memset(_deploySound,  0, sizeof(_deploySound));
        std::memset(_retractSound, 0, sizeof(_retractSound));
        if (deploySound  && deploySound[0])
            std::strncpy(_deploySound,  deploySound,  sizeof(_deploySound)  - 1);
        if (retractSound && retractSound[0])
            std::strncpy(_retractSound, retractSound, sizeof(_retractSound) - 1);
        _soundMask = (outputMask >= 1 && outputMask <= 3) ? outputMask : 0x03;
    }

    /// Fleet command — the same path GEAR_ALL takes on the wire (action:
    /// 0 = stop/hold, 1 = deploy, 2 = retract).  Stop → emergency-hold every
    /// strut; deploy/retract → `commandAllTarget(Down/Up)`.
    void commandAll(uint8_t action);

    /// Fleet target — drive every strut to `t` honouring the coord mode
    /// (Independent = full-cycle each; DoorSync/FullSync = barrier-stepped;
    /// Sequenced = one at a time).  The RC-channel + GEAR_ALL entry point;
    /// a mid-transit re-target reverses each strut cleanly.
    void commandAllTarget(Gear::Target t);

    /// Emergency hold every strut in place (brake + freeze).  GEAR_ESTOP `[]`.
    void emergencyHoldAll();

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == GearPacket::GEAR_DEPLOY
            || type == GearPacket::GEAR_RETRACT
            || type == GearPacket::GEAR_STOP
            || type == GearPacket::GEAR_ALL
            || type == GearPacket::GEAR_STATUS_REQ
            || type == GearPacket::GEAR_LIST_REQ
            || type == GearPacket::GEAR_RESET
            || type == GearPacket::GEAR_ESTOP
            || type == GearPacket::GEAR_STEP
            || type == GearPacket::GEAR_DOOR
            || type == GearPacket::GEAR_STRUT
            || type == GearPacket::GEAR_DOOR_ALL
            || type == GearPacket::GEAR_STRUT_ALL;
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
    void handleEstop      (const uint8_t* p, size_t len);
    void handleStep       (const uint8_t* p, size_t len);
    void handleDoor       (const uint8_t* p, size_t len);   ///< manual: one strut's doors
    void handleStrut      (const uint8_t* p, size_t len);   ///< manual: one strut's motor
    void handleDoorAll    (const uint8_t* p, size_t len);   ///< manual: every strut's doors (all-or-nothing)
    void handleStrutAll   (const uint8_t* p, size_t len);   ///< manual: every strut's motor (all-or-nothing)

    /// Role-event INGRESS — enqueue-only, callable from ANY dispatch context.
    /// The 2026-08-08 coredump family: reacting to BIMOTOR_ENDSTOP_RESULT
    /// inline (enterError → commitBatch → forwardToExpander) pumps the wire
    /// for the ACK, which dispatches the NEXT queued event NESTED inside the
    /// first handler — with three simultaneous faults the recursion corrupts
    /// the shared batch state / stack.  Events are now queued here and the
    /// REACTIONS run from update() on the loop task.
    void onRoleEvent(const char* guid, uint8_t innerType,
                     const uint8_t* p, size_t len);

    /// The former onRoleEvent body — match the event to a gear and run its
    /// FSM reaction (which may send wire commands).  update()-context only.
    void dispatchRoleEvent(const char* guid, uint8_t innerType,
                           uint8_t portIdx, uint8_t outcome);

    /// Drain the role-event ring — first thing in update().
    void drainRoleEvents();

    // Connection-loss subscriber (item 6) — registered on the InputDispatcher
    // in begin().  `state` 2 = DOWN (the actionable loss); fires an emergency
    // deploy when `_deployOnConnLoss` is set.
    void onConnectionLoss(const char* guid, uint8_t portKind,
                          uint8_t portIdx, uint8_t state);
    static void connLossTrampoline(void* ctx, const char* guid,
                                   uint8_t portKind, uint8_t portIdx,
                                   uint8_t state);

    /// Multi-strut coordinator — called from update().  Sequenced: advance the
    /// chain when the active strut settles.  DoorSync/FullSync: step every
    /// strut to the next leg once ALL sit at the current barrier (legComplete).
    void driveCoordinator();
    void driveSyncStep();

    /// Sequenced mode: start the next strut's full cycle (toward `_seqTarget`).
    void sequencedKick();

    static bool sendRoleCmdTrampoline(void* ctx, const PortRef& addr,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);
    static void beginBatchTrampoline (void* ctx);
    static void commitBatchTrampoline(void* ctx);
    static void phaseEventTrampoline (void* ctx, uint8_t id, uint8_t newPhase,
                                      uint8_t newSubPhase, uint8_t errReason);
    static void roleEventTrampoline  (void* ctx, const char* guid,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);

    // Fan-out helper: on a Deployed/Retracted transition, push the
    // matching state to every gear-bound landing light.
    void forwardToLandings(uint8_t newPhase);

    void emitPhaseEvent(uint8_t id, uint8_t phase, uint8_t subPhase, uint8_t errReason);

    /// Transit-sound edge detector (called each update()): start the
    /// direction-matched loop when ANY gear begins moving, stop it when the
    /// whole set settles (and no Sequenced chain is still pending — the
    /// inter-gear handoff gap must not stutter the loop).
    void updateTransitSound();

    sfx_core::BoardServerBase* _ctx     = nullptr;
    TTopology*                 _topo    = nullptr;
    TLandingService*           _landing = nullptr;

    GearDef _defs[kMaxGears]      = {};
    Gear    _gears[kMaxGears]     = {};
    uint8_t _numDefs              = 0;
    bool    _enabled              = true;     // runtime enable flag (config-driven)
    uint8_t _coordMode            = CoordMode::Independent;
    uint8_t _seqActive            = 0xFF;     // Sequenced: strut index mid-cycle
    Gear::Target _seqTarget       = Gear::Target::Up;   // Sequenced: chain direction
    bool    _syncActive           = false;    // DoorSync/FullSync: a barrier walk is in flight
    Gear::Target _syncTarget      = Gear::Target::Up;   // DoorSync/FullSync: barrier-walk target
    bool    _deployOnConnLoss     = false;    // item 6: emergency deploy on input link loss

    // Deferred role-event ring (see onRoleEvent).  Producers can be the USB
    // CDC driver task AND a loop-task sync-forward pump, so head/tail moves
    // are guarded by a spinlock (ESP-IDF portMUX — this is hub-only code).
    struct QueuedRoleEvent {
        uint8_t innerType = 0;
        uint8_t portIdx   = 0;
        uint8_t outcome   = 0;
        char    guid[5]   = {};
    };
    static constexpr uint8_t kRoleEventQ = 16;
    QueuedRoleEvent _roleEvQ[kRoleEventQ] = {};
    uint8_t  _roleEvHead = 0;
    uint8_t  _roleEvTail = 0;
    uint32_t _roleEvDropped = 0;
    portMUX_TYPE _roleEvMux = portMUX_INITIALIZER_UNLOCKED;
    bool    _connLossLatched      = false;    // one deploy per loss (cleared on recovery)

    // Transit sounds (optional — silent when no paths configured).
    AudioCmdFn _audio        = nullptr;       // sketch-wired mixer trampoline
    void*      _audioCtx     = nullptr;
    uint8_t    _audioChannel = 0;             // HubFxLayout::Gear (sketch-supplied)
    char       _deploySound[64]  = {};
    char       _retractSound[64] = {};
    uint8_t    _soundMask        = 0x03;
    bool       _soundActive      = false;     // a transit loop is playing
    bool       _soundDeploying   = false;     // direction of the active loop
};

}  // namespace hubfx::effects::gearctrl

#include "gearcontrol_service.ipp"

#endif  // HUBFX_GEARCONTROL_SERVICE_H
