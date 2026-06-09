/*
 * RcPwmInputHandler implementation — bodies moved verbatim from the former
 * InputRoleHandler RC-PWM methods.
 */

#include "role_rcpwm_input_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#include <variant>

namespace sfx_core {

bool RcPwmInputHandler::attach(InputBinding& b, uint8_t portIdx,
                               const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<RcPwmInputRole>();
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
    role.onBroadcast([this, portIdx](uint8_t /*count*/, bool /*valid*/) {
        // Rebuild the full PPM channel frame from the role each tick
        // (mirrors the SBUS / Jeti pattern).
        auto* binding = _reg->inputAt(portIdx);
        if (!binding) return;
        if (auto* r = std::get_if<RcPwmInputRole>(&binding->role)) {
            _emit->emitPpmFrameBroadcast(portIdx, *r);
        }
    });
    return true;
}

void RcPwmInputHandler::handleGetValueReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<RcPwmInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[4];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->latest_us());
    out[3] = r->valid() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::RCIN_VALUE_RESP, _ctx->currentTag(), out, sizeof out);
}

void RcPwmInputHandler::handleSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    const uint8_t hz  = p[1];
    auto* b = _reg->inputAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<RcPwmInputRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setBroadcastHz(hz);
    _ctx->sendAck();
}

}  // namespace sfx_core
