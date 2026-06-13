/*
 * input_protocol.h — HubFX input-routing control packets.
 *
 * `InputDispatcherServicePolicy` fans decoded RC channels (PPM / SBUS /
 * Jeti EX) out to the effect TriggerInputs that subscribed to them.
 * These packets toggle that fan-out globally: when routing is DISABLED
 * the dispatcher stops feeding effects, so RC sticks no longer drive
 * engine / gun / etc. — the operator drives them from Studio instead
 * (per-effect operational + puppet buttons).  Effects HOLD their last
 * commanded state while routing is off; the wire broadcast still flows
 * so Studio's live channel bars keep moving (you can see RC, it just
 * doesn't act).
 *
 * Protocol-agnostic: the gate sits at the single convergence point in
 * the dispatcher (after the per-protocol extractor), so one flag covers
 * PPM, SBUS and Jeti EX identically.
 *
 * Packet slice: 0xE8..0xED (from the master dispatch map's free range —
 * InputDispatcher sits ahead of Audio/Board in the BoardOf<> pack, so it
 * claims these before anyone else).  0xE8..0xEA = routing gate;
 * 0xEB..0xED = telemetry-collection read (item 4).
 *
 * Rule 11 applies — append optional fields, never reorder.
 */

#ifndef HUBFX_INPUT_PROTOCOL_H
#define HUBFX_INPUT_PROTOCOL_H

#include <cstdint>

namespace hubfx::effects::input {

namespace InputRoutingPacket {

    constexpr uint8_t INPUT_ROUTING_SET_ENABLED = 0xE8;
        ///< [enabled:u8] (0 = RC routing off / manual, 1 = on) → ACK
    constexpr uint8_t INPUT_ROUTING_GET_REQ     = 0xE9;
        ///< [] → INPUT_ROUTING_RESP
    constexpr uint8_t INPUT_ROUTING_RESP        = 0xEA;
        ///< [enabled:u8]

}  // namespace InputRoutingPacket

// ── Telemetry collection (item 4) ────────────────────────────────────
//
// Read the master's live telemetry collection (JetiTelemetryHub) — the
// hub-local sensors + every actively-polled input device (e.g. an ESC on
// IN_2).  Surfaced in Studio's Input/Output Ports tab and via the CLI.
namespace TelemetryPacket {

    constexpr uint8_t TELEMETRY_GET_REQ = 0xEB;
        ///< [] → TELEMETRY_RESP (a snapshot of the whole collection)
    constexpr uint8_t TELEMETRY_RESP    = 0xEC;
        ///< Snapshot, little-endian:
        ///<   [pubIntervalMs:u16][respHz_x10:u16][activeSensors:u8][deviceCount:u8]
        ///<   per device:
        ///<     [usn:u16][lsn:u16][flags:u8(b0 local,b1 active)][sensorCount:u8]
        ///<     [nameLen:u8][name…]
        ///<     per sensor:
        ///<       [id:u8][type:u8][decimals:u8][flags:u8(b0 active)][value:i32LE]
        ///<       [labelLen:u8][label…][unitLen:u8][unit…]
        ///< Truncated to fit MAX_PAYLOAD; deviceCount reflects what was emitted.
    constexpr uint8_t TELEMETRY_UPDATE  = 0xED;
        ///< async TAG_ASYNC live push (same body as RESP), gated on host-verbose
        ///< (Rule 53 lossy live-view).  Reserved — poll via GET_REQ for v1.

}  // namespace TelemetryPacket

// ── Generic input connection-loss (item 5) ───────────────────────────
//
// The dispatcher watches every input SOURCE it has seen frames from (PPM /
// SBUS / Jeti EX, local or remote) and detects a link going silent — one
// protocol-agnostic mechanism off the frame-broadcast timestamps.  On a loss
// it holds effect outputs (the routing layer already holds last value),
// records a brownout, and — if silence exceeds the configurable link-loss
// interval — emits CONNECTION_EVENT (async) for the GUI + an in-firmware
// callback that effects (GearControl) can subscribe to for an emergency
// reaction.  0xAD..0xAF from the master dispatch map's free range.
namespace ConnectionPacket {

    constexpr uint8_t CONNECTION_EVENT   = 0xAD;
        ///< async TAG_ASYNC: [portKind:u8][portIdx:u8][guidLen:u8][guid…]
        ///<                   [state:u8(0 up,1 lost,2 down)][brownouts:u16LE]
    constexpr uint8_t CONNECTION_GET_REQ = 0xAE;
        ///< [] → CONNECTION_RESP
    constexpr uint8_t CONNECTION_RESP    = 0xAF;
        ///< [linkLossMs:u16LE][count:u8] then per link:
        ///<   [portKind:u8][portIdx:u8][guidLen:u8][guid…][state:u8]
        ///<   [brownouts:u16LE][silenceMs:u16LE]
    constexpr uint8_t CONNECTION_SET_CFG = 0xA8;
        ///< [linkLossMs:u16LE] → ACK  (global; 0 = disable loss signalling)

}  // namespace ConnectionPacket

}  // namespace hubfx::effects::input

#endif  // HUBFX_INPUT_PROTOCOL_H
