/*
 * TopologyServicePolicyT — system-wide port + role addressing.
 *
 *   The single wire surface Studio / CLI talks to for the "unified
 *   topology" view (hub + every connected expander, addressed by
 *   GUID).  Routes incoming GUID-prefixed packets to either:
 *
 *     - the LOCAL hub's `PortRegistry` + `RoleServicePolicy` (when the
 *       guid is "" or matches the hub's deviceName suffix), or
 *     - a specific expander via its slot's outbound queue / a
 *       synchronous CDC forward (config-time commands).
 *
 *   Also re-emits every TAG_ASYNC packet that arrives from expanders
 *   (role broadcasts, role-attached events, etc.) as a
 *   `TOPOLOGY_ROLE_EVENT` so Studio sees a single tagged stream.
 *
 *   This policy depends on `ExpanderServicePolicyT<>` for USB plumbing
 *   and GUID tracking, and on the local `PortServicePolicy` /
 *   `RoleServicePolicy` for hub-side dispatch.  All three are bound
 *   via setters before `board.begin()` runs.
 *
 *   Packet ownership: 0x88..0x8E.
 *   Capability bits: none (already advertised by ExpanderService).
 */

#ifndef HUBFX_TOPOLOGY_SERVICE_H
#define HUBFX_TOPOLOGY_SERVICE_H

#include <concepts>
#include <cstdint>
#include <cstddef>
#include <cstring>

#include <serial/core/core.h>          // CommandHandleResult, CorePacket, SerialError
#include <serial/wire.h>               // SfxWire helpers + TAG_ASYNC
#include <serial/diag_log.h>           // SFX_LOG_*
#include <serial/ports.h>              // PortPacket / PortKind / PortError
#include <serial/roles.h>              // RolePacket / RoleKind / RoleError
#include <server/board_server.h>       // sfx_core::BoardServerBase
#include <server/port_registry.h>      // PortRegistryBase + binding structs
#include <server/role_registry.h>      // roleKindOf / forEachAttachedRole — shared role-kind map
#include <server/role_service.h>       // sfx_core::RoleServicePolicy

#include "topology_protocol.h"
#include "../effects/effect_id.h"
#include "../expanders/expander_service.h"   // ExpanderService concept

namespace hubfx::topology {

using hubfx::effects::EffectId;
using hubfx::effects::PortRef;

// One row in the claim registry.  Held in a small fixed-size array on
// `TopologyServicePolicyT` so the entire ownership map fits in BSS.
struct ClaimEntry {
    PortRef  port;
    uint8_t  roleKind = 0;          ///< role advertised at claim time (diag only)
    EffectId owner    = EffectId::None;
    bool     occupied = false;
};

// ────────────────────────────────────────────────────────────────────
//  TopologyService concept
// ────────────────────────────────────────────────────────────────────
//
// Compile-time contract every type passed as `TTopology` to consumers
// (LandingLight, LightFx, GearControl, EngineFx, GunFx, InputDispatcher,
// AlertService) must satisfy.  `TopologyServicePolicyT<TExpander>` is
// the canonical model.  Concept-checked template signatures point
// straight at missing methods instead of nested instantiation chains.
//
// Surface is split into three logical groups:
//   - port / role plumbing       (sendRoleCommand, attachRole, detachRole)
//   - claim registry             (claim, release, ownerOf)
//   - batching + event fan-out   (beginBatch / commitBatch / onRoleEvent)
//
template <typename T>
concept TopologyService = requires(T& t, const PortRef& addr,
                                   uint8_t innerType, const uint8_t* p,
                                   size_t len, EffectId who, uint8_t roleKind,
                                   typename T::RoleEventCallback cb,
                                   void* ctx) {
    // Role command dispatch + role lifecycle.
    { t.sendRoleCommand(addr, innerType, p, len) } -> std::convertible_to<bool>;
    { t.attachRole(addr, roleKind, p, uint8_t{0}) } -> std::convertible_to<bool>;
    { t.detachRole(addr) }                           -> std::convertible_to<bool>;

    // Claim registry.
    { t.claim(addr, who, roleKind) }                 -> std::convertible_to<bool>;
    { t.release(addr) };
    { t.ownerOf(addr) }                              -> std::convertible_to<EffectId>;

    // Batching primitives — effects use these to atomic-burst their
    // multi-command transitions through the wire.
    { t.beginBatch() };
    { t.commitBatch() }                              -> std::convertible_to<bool>;
    { t.discardBatch() };
    { t.inBatch() }                                  -> std::convertible_to<bool>;

    // Master-internal role-event fan-out.
    { t.onRoleEvent(cb, ctx) }                       -> std::convertible_to<bool>;
};

template <hubfx::expanders::ExpanderService TExpander>
class TopologyServicePolicyT {
public:
    /// Advertises `CoreCapability::TOPOLOGY` — master-only.  The
    /// `EXPANDER_BUS` bit already advertises raw expander enumeration;
    /// `TOPOLOGY` says the master additionally speaks the GUID-addressed
    /// port/role surface (0x88..0x8E) on top of it.
    static constexpr uint32_t kCapabilityBits = CoreCapability::TOPOLOGY;

    /// Max number of (board, port) targets a board's effects can
    /// claim simultaneously.  Sized for the HubFX rev: 8 PWM + 11
    /// servo + 1 input local + ~12 expander ports = ≈32, leave
    /// headroom.
    static constexpr uint8_t kMaxClaims = 48;

    /// Batching budget — see the `beginBatch` / `commitBatch` block
    /// below.  A program switch's worst case is ~16 channels × 3
    /// commands (LED_QUEUE_LOAD / SET_BRIGHTNESS / START) + landing
    /// lights ≈ 60 entries.  Leave headroom.
    static constexpr uint8_t  kMaxBatchEntries = 80;
    /// Pooled payload buffer for queued batch entries.  Biggest single
    /// payload is LED_QUEUE_LOAD with the full event queue
    /// (~ 2 + 16 events × 10 bytes = 162 B); 16 channels of that fits
    /// comfortably under 3 KB.
    static constexpr size_t   kBatchBufferSize = 3072;

    TopologyServicePolicyT() = default;

    // ── SystemServicePolicy surface ─────────────────────────────────────

    /// Resolves all three dependencies (port registry, role service,
    /// expander service) from the BoardServerBase context — no
    /// pre-begin() user wiring required.  Returns false if any of
    /// them is missing from the policy pack.
    bool begin(sfx_core::BoardServerBase* ctx) {
        _ctx = ctx;
        if (!_ctx) return false;

        _reg     = ctx->portRegistry();
        _roleSvc = ctx->template findPolicy<sfx_core::RoleServicePolicy>();
        _exp     = ctx->template findPolicy<TExpander>();

        if (!_reg || !_roleSvc || !_exp) return false;

        // Cache the hub's own GUID for tagging local-async fan-outs.
        const char* name = _ctx->deviceName();
        if (name) {
            const char* dash = std::strrchr(name, '-');
            if (dash && dash[1]) {
                size_t n = std::strlen(dash + 1);
                if (n > 4) n = 4;
                std::memcpy(_hubGuid, dash + 1, n);
                _hubGuid[n] = 0;
            }
        }

        // Re-emit every expander-side TAG_ASYNC packet upward, wrapped
        // in a `TOPOLOGY_ROLE_EVENT` carrying the source GUID so Studio
        // can correlate to the right board.
        _exp->onExpanderAsync(
            [this](uint8_t slotIdx, uint8_t type,
                   const uint8_t* p, size_t len) {
                this->onExpanderAsync(slotIdx, type, p, len);
            });

        // Subscribe to hub-local role-async emits so effects can
        // listen to the unified stream regardless of where the event
        // originated.
        _roleSvc->setLocalAsyncListener(&onLocalRoleAsyncTrampoline, this);
        return true;
    }

    bool ownsType(uint8_t type) const {
        return type == TopologyPacket::TOPOLOGY_PORT_LIST_REQ
            || type == TopologyPacket::TOPOLOGY_ROLE_LIST_REQ
            || type == TopologyPacket::TOPOLOGY_ROLE_ATTACH
            || type == TopologyPacket::TOPOLOGY_ROLE_DETACH
            || type == TopologyPacket::TOPOLOGY_ROLE_FORWARD
            || type == TopologyPacket::TOPOLOGY_ROLE_QUERY;
        // _RESP / _EVENT / _RESPONSE are outbound only — never received.
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    void update() {}   // event-driven; no periodic work

    const char* getErrorMessage(uint8_t code) const {
        return TopologyError::getMessage(code);
    }

    // ════════════════════════════════════════════════════════════════
    //  Effect-facing API — not on the wire
    // ════════════════════════════════════════════════════════════════
    //
    // These methods are how `effects/*` code interacts with the system
    // topology.  None of them go through serial dispatch — they're
    // direct C++ calls on the master.

    /// Send a per-role command (LED_SET_BRIGHTNESS, SERVO_SET_TARGET,
    /// LED_QUEUE_LOAD, …) to the role attached at `addr`.  Hub-local
    /// targets dispatch through the local `RoleServicePolicy`; remote
    /// targets forward over CDC.  Returns true on ACK from the target.
    ///
    /// The role at `addr` must already be attached — this call does
    /// NOT auto-attach.  Use `attachRole(...)` for that.
    bool sendRoleCommand(const PortRef& addr, uint8_t innerType,
                         const uint8_t* payload, size_t len);

    /// Attach a role to a port.  Convenience wrapper that builds the
    /// ROLE_ATTACH payload and routes through the same local/remote
    /// dispatcher used by the wire-side TOPOLOGY_ROLE_ATTACH handler.
    bool attachRole(const PortRef& addr, uint8_t roleKind,
                    const uint8_t* cfg = nullptr, uint8_t cfgLen = 0);

    /// Detach the role at `addr`.  Auto-releases any claim held on it.
    bool detachRole(const PortRef& addr);

    /// Detach EVERY currently-attached hub-local role (walks the port
    /// registry, detaches each occupied servo/pwm/hbridge/input port).  Used
    /// by the config-reload re-apply: clear the live port/role layout so the
    /// fresh /hubfx.yaml `ports:` block can be re-attached, making a Studio
    /// Apply change roles WITHOUT a reboot.  Returns the count detached.
    uint8_t detachAllHubRoles();

    /// Push a board's FULL role set to a remote expander in ONE packet
    /// (ROLE_BULK_ATTACH) — the declarative bringup path used by
    /// ExpanderService::onReady.  `block` is `[count][entries…]` built by
    /// `buildRoleBlockForGuid`.  On ACK, the hub's cached role roster for the
    /// board is updated from the block (so topo-roles / Studio reflect the
    /// applied config — the old per-port forward never did this).  A count==0
    /// block is a no-op success (unconfigured board).  Returns true on ACK.
    bool applyRoleConfig(const char* guid, const uint8_t* block, size_t len);

    /// Enumerate every (board, port) currently bound to `roleKind`.
    /// Writes up to `maxOut` entries into `out`; returns the actual
    /// count.  Walks the hub-local registry + every live expander's
    /// cached role roster.
    size_t portsByRole(uint8_t roleKind, PortRef* out, size_t maxOut) const;

    // ── Claim registry ─────────────────────────────────────────────
    //
    // Master-side ownership map keyed by (board, port).  Effects call
    // `claim()` before they start driving a port; second claimant for
    // the same port gets rejected (unless it's the same owner, in
    // which case `claim()` is idempotent).
    //
    // Auto-release: `detachRole()` and `ROLE_DETACHED` async events
    // drop the matching claim.  Manual `release()` is also available.

    /// Try to claim `addr` for `who`.  Returns true if the claim
    /// succeeded (or was already held by the same owner).  False on
    /// conflict — the existing owner keeps the port.
    bool claim(const PortRef& addr, EffectId who, uint8_t roleKind = 0);

    /// Release whatever claim exists on `addr`.  No-op if unclaimed.
    void release(const PortRef& addr);

    /// Return the owner of `addr`, or `EffectId::None` if unclaimed.
    EffectId ownerOf(const PortRef& addr) const;

    /// Direct access to the claim table for diagnostics / Studio.
    const ClaimEntry* claims()      const { return _claims; }
    uint8_t           numClaims()   const { return _numClaims; }

    // ── Outgoing-command batching ────────────────────────────────────
    //
    // Effects that need to apply many role commands as one logical
    // unit (e.g. a 16-channel program switch) wrap the operation in
    // `beginBatch()` / `commitBatch()`.  Every `sendRoleCommand` call
    // in between is queued into a small pooled buffer instead of
    // dispatching immediately.  `commitBatch()` then dispatches the
    // whole queue back-to-back so the serial / CDC bus sees one
    // contiguous burst rather than 16 round-trips of one packet each.
    //
    // Nested batches are supported via a depth counter — inner
    // `beginBatch`/`commitBatch` calls become no-ops, the outermost
    // pair is the one that actually opens / dispatches.  Effects
    // call `inBatch()` if they need to know whether they own the
    // batch lifecycle.
    //
    // `discardBatch()` drops the queue without dispatch — error-path use.

    void beginBatch()    { ++_batchDepth; }
    bool inBatch() const { return _batchDepth > 0; }
    bool commitBatch();        ///< returns true if every queued command succeeded
    void discardBatch();

    /// Master-internal subscription: fan-out of every role async event
    /// (SERVO_TARGET_REACHED, LED_QUEUE_DONE, ROLE_ATTACHED, etc.) from
    /// BOTH hub-local roles AND every connected expander, normalized
    /// with the source GUID prefix.  Supports up to `kMaxRoleEventSubs`
    /// concurrent subscribers — every effect listening for async
    /// events registers once here.
    using RoleEventCallback = void (*)(void* ctx, const char* guid,
                                       uint8_t innerType,
                                       const uint8_t* p, size_t len);

    static constexpr uint8_t kMaxRoleEventSubs = 4;

    /// Append a subscriber.  Returns false if the table is full.
    bool onRoleEvent(RoleEventCallback fn, void* ctx) {
        if (_numRoleEventSubs >= kMaxRoleEventSubs || !fn) return false;
        _roleEventSubs[_numRoleEventSubs].fn  = fn;
        _roleEventSubs[_numRoleEventSubs].ctx = ctx;
        ++_numRoleEventSubs;
        return true;
    }

private:
    // Wire handlers ----------------------------------------------------
    void handlePortListReq(const uint8_t* p, size_t len);
    void handleRoleListReq(const uint8_t* p, size_t len);
    void handleRoleAttach (const uint8_t* p, size_t len);
    void handleRoleDetach (const uint8_t* p, size_t len);
    void handleRoleForward(const uint8_t* p, size_t len);   // Rule 11 ext 2026-05-24
    void handleRoleQuery  (const uint8_t* p, size_t len);   // generic request-response forward

    /// Dispatch a role QUERY (request→typed-RESP) to a GUID — local (capture
    /// the RESP) or remote (forwardQuery the expander).  Role-agnostic.  Fills
    /// respType + respBuf[0..respLen) on success.  Returns false on any failure.
    bool queryRole(const char* guid, uint8_t reqType, const uint8_t* req, size_t reqLen,
                   uint8_t& respType, uint8_t* respBuf, size_t respMax, size_t& respLen);
    /// Remote half of queryRole — forwards to the expander + captures its RESP.
    bool forwardQuery(uint8_t slotIdx, uint8_t reqType, const uint8_t* req, size_t reqLen,
                      uint8_t& respType, uint8_t* respBuf, size_t respMax, size_t& respLen);

    // Async event re-emit ----------------------------------------------
    void onExpanderAsync(uint8_t slotIdx, uint8_t type,
                         const uint8_t* p, size_t len);

    // GUID routing helpers ---------------------------------------------
    /// Decode the `[guidLen:u8][guid:str]` prefix shared by every
    /// topology request.  Returns false on truncated input.  Writes
    /// up to 4 hex chars into `outGuid` (NUL-terminated); empty
    /// string means "target = local hub".  `outOff` is set to the
    /// first byte AFTER the guid block.
    static bool readGuidPrefix(const uint8_t* p, size_t len,
                               char outGuid[5], size_t& outOff);

    /// Returns true when `guid` is empty OR matches the hub's own
    /// deviceName suffix.
    bool isLocalTarget(const char* guid) const;

    /// Lookup helpers wrapping ExpanderService.
    int  slotIdxByGuid(const char* guid) const;   ///< -1 if not live

    // Hub-side port/role enumeration ----------------------------------
    void appendHubPortBlock (uint8_t* buf, size_t& off, size_t cap);
    void appendHubRoleBlock (uint8_t* buf, size_t& off, size_t cap);
    void appendExpanderPortBlock(uint8_t* buf, size_t& off, size_t cap, uint8_t slotIdx);
    void appendExpanderRoleBlock(uint8_t* buf, size_t& off, size_t cap, uint8_t slotIdx);

    // Forward a request to an expander synchronously (config-time path —
    // blocks main loop until ACK / NACK / timeout).  Returns the
    // expander's reply tag for inspection, false on send failure.
    CommandResult forwardToExpander(uint8_t slotIdx, uint8_t innerType,
                                    const uint8_t* payload, size_t len);

    // Internal helper: drop the claim at the i-th slot (swap-with-end).
    void releaseAt(uint8_t i);

    sfx_core::BoardServerBase*    _ctx     = nullptr;
    sfx_core::PortRegistryBase*   _reg     = nullptr;
    sfx_core::RoleServicePolicy*  _roleSvc = nullptr;
    TExpander*                    _exp     = nullptr;

    // Claim table — small fixed array keyed by (port, kind, idx).
    ClaimEntry _claims[kMaxClaims] = {};
    uint8_t    _numClaims = 0;

    // Master-internal role-event subscriber list (see `onRoleEvent`).
    struct RoleEventSub {
        RoleEventCallback fn  = nullptr;
        void*             ctx = nullptr;
    };
    RoleEventSub _roleEventSubs[kMaxRoleEventSubs] = {};
    uint8_t      _numRoleEventSubs                  = 0;

    // ── Batch state (see `beginBatch` / `commitBatch`) ──────────────
    struct BatchEntry {
        PortRef  addr;
        uint8_t  innerType = 0;
        uint16_t offset    = 0;     ///< into `_batchBuffer`
        uint16_t len       = 0;
    };
    BatchEntry _batch[kMaxBatchEntries] = {};
    uint8_t    _batchCount        = 0;
    uint8_t    _batchDepth        = 0;
    uint8_t    _batchBuffer[kBatchBufferSize] = {};
    uint16_t   _batchBufferUsed   = 0;

    // Hub's own GUID, cached at begin() for the local-async path.
    char _hubGuid[5] = {0};

    // Static trampoline for `RoleServicePolicy::setLocalAsyncListener`.
    static void onLocalRoleAsyncTrampoline(void* ctx, uint8_t innerType,
                                           const uint8_t* p, size_t len);
};

}  // namespace hubfx::topology

#include "topology_service.ipp"

#endif  // HUBFX_TOPOLOGY_SERVICE_H
