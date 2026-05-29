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
 * Packet slice: 0xE8..0xEA (from the master dispatch map's free range
 * 0xE8..0xED — InputDispatcher sits ahead of Audio/Board in the
 * BoardOf<> pack, so it claims these before anyone else).
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

}  // namespace hubfx::effects::input

#endif  // HUBFX_INPUT_PROTOCOL_H
