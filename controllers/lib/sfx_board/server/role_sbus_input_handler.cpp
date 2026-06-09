/*
 * SbusInputHandler implementation — bodies moved verbatim from the former
 * InputRoleHandler SBUS methods.
 */

#include "role_sbus_input_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#include <variant>

namespace sfx_core {

bool SbusInputHandler::attach(InputBinding& b, uint8_t portIdx,
                              const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<SbusInputRole>();
    if (!role.bind(b.port)) { b.role.emplace<std::monostate>(); return false; }
    // Optional config: [broadcastHz:u8]
    // Default 50 Hz so the InputDispatcher (and thus effects) get RC values
    // from boot even with no host connected — field operation has no Studio.
    // The WIRE broadcast is gated on a listening host (hostVerboseActive), so
    // a non-zero default doesn't stream into a dead port; only the LOCAL
    // dispatch runs.  A host can still override the rate via Set*BroadcastHz.
    // Wire broadcast stays OFF at attach (no host yet); the LOCAL effect feed
    // runs at the role's fixed 50 Hz default so the model flies standalone.  A
    // host subscribes to the wire via Set*BroadcastHz when it opens the live-
    // channel view (the config byte no longer auto-enables the wire — that
    // flooded a connected-but-not-viewing host).
    (void)cfg; (void)cfgLen;
    role.onBroadcast([this, portIdx](uint8_t /*ch*/, bool /*valid*/,
                                     bool /*failsafe*/, bool /*frameLost*/) {
        // The broadcast packet rebuilds the full channel payload —
        // walk the role each tick via the registry.
        auto* binding = _reg->inputAt(portIdx);
        if (!binding) return;
        if (auto* r = std::get_if<SbusInputRole>(&binding->role)) {
            _emit->emitSbusFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

void SbusInputHandler::handleGetFrameReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<SbusInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const uint8_t count = r->channelCount();
    uint8_t flags = 0;
    if (r->valid())     flags |= 0x01;
    if (r->failsafe())  flags |= 0x02;
    if (r->frameLost()) flags |= 0x04;
    if (r->ch17())      flags |= 0x08;
    if (r->ch18())      flags |= 0x10;

    uint8_t out[3 + 16*2];
    out[0] = idx;
    out[1] = count;
    out[2] = flags;
    size_t off = 3;
    for (uint8_t i = 0; i < count && off + 2 <= sizeof out; i++) {
        SfxWire::putU16LE(&out[off], r->channel_us((uint8_t)(i + 1)));
        off += 2;
    }
    _ctx->sendRawPacket(RolePacket::SBUS_FRAME_RESP, _ctx->currentTag(), out, off);
}

void SbusInputHandler::handleSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    const uint8_t hz  = p[1];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<SbusInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setBroadcastHz(hz);
    _ctx->sendAck();
}

}  // namespace sfx_core
