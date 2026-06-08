/*
 * HeaterRoleHandler implementation — bodies moved verbatim from the former
 * RoleServicePolicy heater handlers.
 */

#include "role_heater_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#include <variant>

namespace sfx_core {

bool HeaterRoleHandler::attach(PwmBinding& b, uint8_t /*portIdx*/,
                               const uint8_t* cfg, size_t cfgLen) {
    // tSense is optional: present → closed-loop bang-bang;
    // absent → open-loop drive at `drivePct` whenever the target is set.
    auto& role = b.role.emplace<HeaterRole>();
    if (!role.bind(b.port, b.tSense)) { b.role.emplace<std::monostate>(); return false; }
    // Wire scaling context from the port binding (Phase 2 of GunFX
    // rollout, instructions/22 — voltage scaling lives on the role,
    // sourced from the per-port rail declared in Phase 0).
    role.setPortRailMv(b.voltageMv);
    // Optional config:
    //   [target_cx10:i16LE][hysteresis_cx10:i16LE][drivePct:u8]
    //   [elementMv:u16LE][scaling:u8]
    if (cfgLen >= 2) role.setTarget((int16_t)SfxWire::getI16LE(&cfg[0]));
    if (cfgLen >= 4) role.setHysteresis((int16_t)SfxWire::getI16LE(&cfg[2]));
    if (cfgLen >= 5) role.setDrivePct(cfg[4]);
    if (cfgLen >= 8) {
        ElementConfig ec;
        ec.elementMv = SfxWire::getU16LE(&cfg[5]);
        ec.mode      = static_cast<ElementScalingMode>(cfg[7]);
        role.setElement(ec);
    }
    return true;
}

void HeaterRoleHandler::handleSetTarget(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setTarget((int16_t)SfxWire::getI16LE(&p[1]));
    _ctx->sendAck();
}

void HeaterRoleHandler::handleGetStatus(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[8];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], r->target());
    SfxWire::putI16LE(&out[3], r->actual_cx10());
    SfxWire::putU16LE(&out[5], r->commandedDuty());
    out[7] = r->heating() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::HEATER_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// Live element-config retune (Phase 2.9.x — Rule 42). Element voltage,
// scaling mode, drive percent and hysteresis are all live-tunable. The
// heater's bang-bang `target_cx10` stays separate (HEATER_SET_TARGET).
void HeaterRoleHandler::handleSetElement(const uint8_t* p, size_t len) {
    if (len < 7) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    ElementConfig ec;
    ec.elementMv = SfxWire::getU16LE(&p[1]);
    ec.mode      = static_cast<ElementScalingMode>(p[3]);
    r->setElement(ec);
    r->setDrivePct(p[4]);
    r->setHysteresis(SfxWire::getI16LE(&p[5]));
    _ctx->sendAck();
}

void HeaterRoleHandler::handleGetElementReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<HeaterRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[9];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->element().elementMv);
    out[3] = static_cast<uint8_t>(r->element().mode);
    out[4] = r->drivePct();
    SfxWire::putI16LE(&out[5], r->hysteresis_cx10());
    SfxWire::putU16LE(&out[7], r->portRailMv());
    _ctx->sendRawPacket(RolePacket::HEATER_ELEMENT_RESP, _ctx->currentTag(), out, sizeof out);
}

}  // namespace sfx_core
