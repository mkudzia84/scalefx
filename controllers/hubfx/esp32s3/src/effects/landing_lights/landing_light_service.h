/*
 * landing_light_service.h — `LandingLightServicePolicyT<TTopology>`.
 *
 *   System-service policy slotted into the HubFX `BoardOf<...>` pack.
 *   Owns the table of `LandingLight` instances + their (servo, LED)
 *   port claims, handles the wire surface (LL_LIST / LL_SET_STATE /
 *   LL_STATUS / LL_PHASE_EVENT), and listens for topology role-event
 *   broadcasts to advance each state machine when its servo reaches
 *   its target.
 *
 *   Template-parameterised on the topology service so the service can
 *   call its non-virtual `sendRoleCommand` / `claim` API directly.
 *   The board sketch resolves the type at compile time via
 *   `findPolicy<>()` and a using-alias.
 *
 *   Configuration: for now, the master sketch hands an
 *   `std::initializer_list<LandingLightDef>` (or array) to `configure()`
 *   before `board.begin()`.  YAML loading lands later.
 */

#ifndef HUBFX_LANDING_LIGHT_SERVICE_H
#define HUBFX_LANDING_LIGHT_SERVICE_H

#include <concepts>
#include <cstdint>
#include <cstring>

#include <serial/core/core.h>          // CommandHandleResult, CorePacket
#include <serial/roles.h>              // RolePacket::SERVO_TARGET_REACHED
#include <serial/diag_log.h>
#include <server/board_server.h>

#include "../effect_id.h"
#include "../../topology/topology_service.h"   // TopologyService concept
#include "landing_light.h"
#include "landing_light_protocol.h"

namespace hubfx::effects::landing {

/// Max landing lights on a single board.  HubFX rev needs at most 4
/// (main left, main right, nose, tail) — leave headroom for kit boards.
inline constexpr uint8_t kMaxLandingLights = 8;

// ────────────────────────────────────────────────────────────────────
//  LandingLightService concept
// ────────────────────────────────────────────────────────────────────
//
// Contract every type passed as `TLandingService` to consumers
// (LightFx, GearControl) must satisfy.  Both effects forward landing-
// light state changes through this surface — they never touch the
// underlying servo / LED ports directly (those are claimed by
// `EffectId::LandingLight`).
//
template <typename T>
concept LandingLightService = requires(T& s, uint8_t id, uint8_t state,
                                       hubfx::effects::EffectId caller) {
    { s.setState(id, state, caller) } -> std::convertible_to<bool>;
    { s.count() }                     -> std::convertible_to<uint8_t>;
    { s.at(id) }                      -> std::same_as<LandingLight*>;
    { s.findById(id) }                -> std::same_as<LandingLight*>;
};

template <hubfx::topology::TopologyService TTopology>
class LandingLightServicePolicyT {
public:
    static constexpr uint32_t kCapabilityBits = CoreCapability::LANDING_LIGHTS;

    LandingLightServicePolicyT() = default;

    /// Install the landing-light definitions.  Call order vs
    /// `board.begin()` no longer matters: `applyDefs()` binds the per-LL
    /// instances + claims their ports as soon as BOTH the defs and the
    /// topology pointer exist (begin() binds topology + calls applyDefs;
    /// configure() sets defs + calls applyDefs).  In the YAML config-
    /// chain model this fires from the config callback AFTER begin().
    /// Copies are made — caller's array can go out of scope.
    void configure(const LandingLightDef* defs, uint8_t count) {
        _numDefs = 0;
        for (uint8_t i = 0; i < count && i < kMaxLandingLights; ++i) {
            _defs[_numDefs++] = defs[i];
        }
        applyDefs();
    }

    /// Runtime-enable flag — picked up by `BoardServer::
    /// computeEnabledCapabilities()` so the host can distinguish
    /// "compiled in" from "active right now".  Set by
    /// `HubFxConfigServicePolicy::applyConfig()` from
    /// `cfg.landing.enabled`.  Defaults to true so a board with no
    /// config file boots in the same state it always did.
    void setEnabled(bool v) { _enabled = v; }
    bool enabled() const    { return _enabled; }

    // ── SystemServicePolicy surface ──────────────────────────────────

    bool begin(sfx_core::BoardServerBase* ctx);

    bool ownsType(uint8_t type) const {
        return type == LandingLightPacket::LL_LIST_REQ
            || type == LandingLightPacket::LL_SET_STATE
            || type == LandingLightPacket::LL_STATUS_REQ;
    }

    CommandHandleResult handle(uint8_t type,
                               const uint8_t* payload, size_t len);

    void update() {}   // event-driven

    const char* getErrorMessage(uint8_t code) const {
        return LandingLightError::getMessage(code);
    }

    // ── Effects-facing API ───────────────────────────────────────────

    /// Programs in other effect families call this to flip a landing
    /// light.  Returns true on success.  Wrong owner → silently
    /// returns false (and logs a warning) — programs reference LLs by
    /// ID, so cross-owner refs are a config bug surfaced via diag.
    bool setState(uint8_t id, uint8_t state, EffectId caller);

    /// Find an instance by its ID, or nullptr.
    LandingLight* findById(uint8_t id);

    /// Direct iteration for diag / Studio inspection.
    uint8_t       count() const   { return _numDefs; }
    LandingLight* at(uint8_t i)   { return (i < _numDefs) ? &_instances[i] : nullptr; }

private:
    void handleListReq    ();
    void handleStatusReq  ();
    void handleSetState   (const uint8_t* p, size_t len);

    // Role-event router subscribed via topology's `onRoleEvent` —
    // receives every async role event from any board (hub-local or
    // expander) with the source GUID and the inner-packet payload.
    // Filters to SERVO_TARGET_REACHED packets matching one of our
    // landing lights' servo address.
    void onRoleEvent(const char* guid, uint8_t innerType,
                     const uint8_t* p, size_t len);
    static void roleEventTrampoline(void* ctx, const char* guid,
                                    uint8_t innerType,
                                    const uint8_t* p, size_t len);

    // Trampolines for the `LandingLight::SendRoleCmdFn` / PhaseEventFn
    // and the begin/commit-batch hooks (so the per-LL deploy / retract
    // path drops into a topology batch).
    static bool sendRoleCmdTrampoline(void* ctx, const PortRef& addr,
                                      uint8_t innerType,
                                      const uint8_t* p, size_t len);
    static void beginBatchTrampoline (void* ctx);
    static void commitBatchTrampoline(void* ctx);
    static void phaseEventTrampoline (void* ctx, uint8_t id, uint8_t newPhase);

    void emitPhaseEvent(uint8_t id, uint8_t phase);

    // (Re)bind every per-LL instance to its def + claim its (servo +
    // LED) ports.  No-op until `_topology` is bound.  Called from BOTH
    // begin() and configure() so it runs once defs + topology both exist,
    // regardless of order; idempotent on config reload.
    void applyDefs();

    // Claim every (servo + LED) port referenced by `_defs` for
    // `EffectId::LandingLight`.  Called from `applyDefs()`.
    void claimPorts();

    sfx_core::BoardServerBase* _ctx        = nullptr;
    TTopology*                _topology   = nullptr;

    LandingLightDef _defs[kMaxLandingLights] = {};
    LandingLight    _instances[kMaxLandingLights] = {};
    uint8_t         _numDefs   = 0;
    bool            _enabled   = true;     // runtime enable flag (config-driven)
};

}  // namespace hubfx::effects::landing

#include "landing_light_service.ipp"

#endif  // HUBFX_LANDING_LIGHT_SERVICE_H
