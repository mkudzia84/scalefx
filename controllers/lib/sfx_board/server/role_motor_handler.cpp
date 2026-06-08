/*
 * DcMotorRoleHandler implementation — bodies moved verbatim from the former
 * RoleServicePolicy DC-motor handlers; the stall callback goes through the
 * bound RoleEventEmitter.
 */

#include "role_motor_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#include <variant>

namespace sfx_core {

bool DcMotorRoleHandler::attach(PwmBinding& b, uint8_t portIdx,
                                const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<DcMotorRole>(b.port, b.iSense, b.vSense);
    // Wire scaling context from the port binding (Phase 2 of GunFX
    // rollout, instructions/22).
    role.setPortRailMv(b.voltageMv);
    // Optional config (append-only — Rule 11 still governs fielded
    // firmware, even though the GunFX rollout itself is greenfield):
    //   [stallThreshold_mA:u16LE][stallWindow_ms:u16LE]
    //   [elementMv:u16LE][scaling:u8]
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    if (cfgLen >= 7) {
        ElementConfig ec;
        ec.elementMv = SfxWire::getU16LE(&cfg[4]);
        ec.mode      = static_cast<ElementScalingMode>(cfg[6]);
        role.setElement(ec);
    }
    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        _emit->emitMotorStallEvent(portIdx, peak, dur);
    });
    return true;
}

void DcMotorRoleHandler::handleSetDuty(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setDuty(SfxWire::getU16LE(&p[1]));
    _ctx->sendAck();
}

void DcMotorRoleHandler::handleBrake(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->brake();
    _ctx->sendAck();
}

void DcMotorRoleHandler::handleGetStatusReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[8];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->duty());
    SfxWire::putI16LE(&out[3], r->voltage_mV());
    SfxWire::putI16LE(&out[5], r->current_mA());
    out[7] = r->stalled() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::MOTOR_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// Live element-config retune (Phase 2.9.x — Rule 42).
void DcMotorRoleHandler::handleSetElement(const uint8_t* p, size_t len) {
    if (len < 4) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    ElementConfig ec;
    ec.elementMv = SfxWire::getU16LE(&p[1]);
    ec.mode      = static_cast<ElementScalingMode>(p[3]);
    r->setElement(ec);
    _ctx->sendAck();
}

void DcMotorRoleHandler::handleGetElementReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->pwmAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t out[6];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->element().elementMv);
    out[3] = static_cast<uint8_t>(r->element().mode);
    SfxWire::putU16LE(&out[4], r->portRailMv());
    _ctx->sendRawPacket(RolePacket::MOTOR_ELEMENT_RESP, _ctx->currentTag(), out, sizeof out);
}

// Intent-layer DC motor drive (Phase 2.9.x — Rule 42).  "Drive at N %
// of the element's rated voltage"; the role applies scaleDuty() and
// writes the port-native duty.  Replaces the gun_unit `pct*40` stopgap
// for the smoke fan.
void DcMotorRoleHandler::handleSetPct(const uint8_t* p, size_t len) {
    if (len < 2) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->pwmAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<DcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    uint8_t pct = p[1];
    if (pct > 100) pct = 100;
    r->setPct(pct);
    _ctx->sendAck();
}

}  // namespace sfx_core
