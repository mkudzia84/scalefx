/*
 * topology_protocol.h — HubFX system-wide port + role addressing.
 *
 * `TopologyServicePolicy` exposes a single coherent view of every port
 * and role across the entire ScaleFX system (hub + every connected
 * expander).  Studio / CLI talks ONLY to this surface — the underlying
 * GUID-prefixed packets here route to either the local hub's
 * `PortServicePolicy` / `RoleServicePolicy` (GUID = "" or hub's GUID)
 * or to a specific expander (GUID matches its IDENTIFY suffix).
 *
 * Master holds the GUID concept top-to-bottom; expanders never learn
 * about GUIDs.  The hub's `RoleServicePolicy` keeps owning the
 * GUID-less per-board protocol it already speaks.
 *
 * Packet slice: 0x88..0x8E (0x80..0x87 reserved for ExpanderService
 * lifecycle + system info + collision events).
 *
 * Rule 11 applies — every payload that grows in the future appends
 * optional fields at the end.
 */

#ifndef HUBFX_TOPOLOGY_PROTOCOL_H
#define HUBFX_TOPOLOGY_PROTOCOL_H

#include <cstdint>

namespace hubfx::topology {

// ============================================================================
// Wire packet types
// ============================================================================
//
// Every request carries a `[guidLen:u8][guid:str]` filter prefix.
//   guidLen == 0   →  operation targets the local hub
//   guid == hubGuid → same as above (canonical form)
//   guid == known expander's GUID → forwarded over CDC
//   unknown guid   →  NACK TOPOLOGY_UNKNOWN_GUID
//
namespace TopologyPacket {

    constexpr uint8_t TOPOLOGY_PORT_LIST_REQ   = 0x88;
        ///< [guidLen:u8][guid:str?]
        ///<   empty guid → return ALL boards
        ///<   non-empty  → return only that board
        ///< Response: TOPOLOGY_PORT_LIST_RESP

    constexpr uint8_t TOPOLOGY_PORT_LIST_RESP  = 0x89;
        ///< [boardCount:u8]
        ///< per-board:
        ///<   [guidLen:u8][guid:str]      ("" = hub)
        ///<   [nameLen:u8][name:str]      (deviceName, with embedded GUID)
        ///<   [numServos:u8] × [idx:u8][capFlags:u8]
        ///<   [numPwms:u8]   × [idx:u8][senseFlags:u8]
        ///<   [numHBridges:u8] × [idx:u8][senseFlags:u8]
        ///<   [numInputs:u8] × [idx:u8][capFlags:u8]

    constexpr uint8_t TOPOLOGY_ROLE_LIST_REQ   = 0x8A;
        ///< [guidLen:u8][guid:str?]
        ///< Response: TOPOLOGY_ROLE_LIST_RESP

    constexpr uint8_t TOPOLOGY_ROLE_LIST_RESP  = 0x8B;
        ///< [boardCount:u8]
        ///< per-board:
        ///<   [guidLen:u8][guid:str]
        ///<   [roleCount:u8]
        ///<   per-role: [portKind:u8][portIdx:u8][roleKind:u8][flags:u8]

    constexpr uint8_t TOPOLOGY_ROLE_ATTACH     = 0x8C;
        ///< [guidLen:u8][guid:str][portKind:u8][portIdx:u8]
        ///< [roleKind:u8][cfgLen:u8][cfg:N]
        ///< Response: ACK or NACK with TOPOLOGY_* error code.

    constexpr uint8_t TOPOLOGY_ROLE_DETACH     = 0x8D;
        ///< [guidLen:u8][guid:str][portKind:u8][portIdx:u8]
        ///< Response: ACK or NACK.

    constexpr uint8_t TOPOLOGY_ROLE_EVENT      = 0x8E;
        ///< async TAG_ASYNC: [guidLen:u8][guid:str][innerType:u8][innerPayload:N]
        ///< Re-emit of an expander-side role async packet
        ///< (SERVO_TARGET_REACHED, RCIN_VALUE_BROADCAST, LED_QUEUE_DONE, ...).
        ///< Hub-local role asyncs are also wrapped so Studio sees a
        ///< single tagged stream regardless of where the event came from.

    /// `[guidLen:u8][guid:str][innerType:u8][innerLen:u16LE][inner:N]`
    /// → ACK / NACK (TOPOLOGY_* error code).
    ///
    /// Generic role-command pass-through.  Studio (or the CLI) builds
    /// a normal role packet — `SERVO_SET_TARGET`, `SERVO_SET_PROFILE`,
    /// `LED_QUEUE_LOAD`, anything in the role layer — wraps it inside
    /// this envelope with a target GUID, and the hub routes:
    ///   - `guid` empty → local `RoleServicePolicy::handle(innerType, …)`
    ///                    in capture mode (returns ACK/NACK directly)
    ///   - `guid` set  → `ExpanderService::queueCommand(slot, innerType, …)`
    ///                    with a CDC round-trip; the hub waits for the
    ///                    expander's ACK and mirrors it back to Studio
    ///
    /// Pair with `TOPOLOGY_ROLE_EVENT` (0x8E) for the reverse direction.
    /// Studio's calibration dialog uses this for cross-board servo
    /// SET_TARGET + SET_PROFILE without needing a dedicated wire packet
    /// per role command.  Added 2026-05-24.
    constexpr uint8_t TOPOLOGY_ROLE_FORWARD    = 0x8F;
}

// ============================================================================
// Topology-layer error codes (0xC0..0xC6 within the SerialError space)
// ----------------------------------------------------------------------------
// MOVED 2026-06-06 from 0x90..0x96, which COLLIDED with AlertError (0x90..0x9F)
// — the Go errorNames map is global + last-init-wins, so a topology NACK showed
// up as "ALERT_DISABLED" etc. Topology is infrastructure with no gap below 0xC0,
// so it carves the bottom of the Reserved block (error codes are a namespace
// SEPARATE from packet types; 0xC0 here is unrelated to packet 0xC0). Guarded by
// tests/host/go_unit/error_collisions_test.
// ============================================================================

namespace TopologyError {
    constexpr uint8_t UNKNOWN_GUID        = 0xC0;  ///< GUID never seen this session
    constexpr uint8_t PENDING_GUID        = 0xC1;  ///< GUID known but currently disconnected
    constexpr uint8_t PORT_NOT_FOUND      = 0xC2;  ///< (kind, idx) out of range on target board
    constexpr uint8_t ROLE_NOT_SUPPORTED  = 0xC3;  ///< target board rejected the role kind
    constexpr uint8_t FORWARD_FAILED      = 0xC4;  ///< CDC send / response timed out
    constexpr uint8_t GUID_COLLISION      = 0xC5;  ///< target is in a GUID collision (Rule 32)
    constexpr uint8_t HANDSHAKE_PENDING   = 0xC6;  ///< expander still finishing post-IDENTIFY roster harvest

    inline const char* getMessage(uint8_t code) {
        switch (code) {
            case UNKNOWN_GUID:       return "Unknown board GUID";
            case PENDING_GUID:       return "Board GUID known but not currently connected";
            case PORT_NOT_FOUND:     return "Port not found on target board";
            case ROLE_NOT_SUPPORTED: return "Role kind not supported on this port";
            case FORWARD_FAILED:     return "CDC forward to expander failed";
            case GUID_COLLISION:     return "Target board is in a GUID collision";
            case HANDSHAKE_PENDING:  return "Expander roster harvest still in progress";
            default:                 return nullptr;
        }
    }
}

}  // namespace hubfx::topology

#endif  // HUBFX_TOPOLOGY_PROTOCOL_H
