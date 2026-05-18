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

#include <Arduino.h>
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
#include <server/role_service.h>       // sfx_core::RoleServicePolicy

#include "topology_protocol.h"

namespace hubfx::topology {

template <typename TExpander>
class TopologyServicePolicyT {
public:
    /// No additional capability bit — `ExpanderServicePolicyT::SLAVE_BUS`
    /// already advertises that the master can enumerate expanders.
    static constexpr uint32_t kCapabilityBits = 0u;

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

        // Re-emit every expander-side TAG_ASYNC packet upward, wrapped
        // in a `TOPOLOGY_ROLE_EVENT` carrying the source GUID so Studio
        // can correlate to the right board.
        _exp->onExpanderAsync(
            [this](uint8_t slotIdx, uint8_t type,
                   const uint8_t* p, size_t len) {
                this->onExpanderAsync(slotIdx, type, p, len);
            });
        return true;
    }

    bool ownsType(uint8_t type) const {
        return type == TopologyPacket::TOPOLOGY_PORT_LIST_REQ
            || type == TopologyPacket::TOPOLOGY_ROLE_LIST_REQ
            || type == TopologyPacket::TOPOLOGY_ROLE_ATTACH
            || type == TopologyPacket::TOPOLOGY_ROLE_DETACH;
        // _RESP / _EVENT are outbound only — never received.
    }

    CommandHandleResult handle(uint8_t type, const uint8_t* payload, size_t len);

    void update() {}   // event-driven; no periodic work

    const char* getErrorMessage(uint8_t code) const {
        return TopologyError::getMessage(code);
    }

private:
    // Wire handlers ----------------------------------------------------
    void handlePortListReq(const uint8_t* p, size_t len);
    void handleRoleListReq(const uint8_t* p, size_t len);
    void handleRoleAttach (const uint8_t* p, size_t len);
    void handleRoleDetach (const uint8_t* p, size_t len);

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

    sfx_core::BoardServerBase*    _ctx     = nullptr;
    sfx_core::PortRegistryBase*   _reg     = nullptr;
    sfx_core::RoleServicePolicy*  _roleSvc = nullptr;
    TExpander*                    _exp     = nullptr;
};

}  // namespace hubfx::topology

#include "topology_service.ipp"

#endif  // HUBFX_TOPOLOGY_SERVICE_H
