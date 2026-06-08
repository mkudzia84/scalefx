/*
 * BiMotorRoleHandler implementation — bodies moved verbatim from the former
 * RoleServicePolicy bi-motor handlers; stall + endstop callbacks go through
 * the bound RoleEventEmitter.
 */

#include "role_bimotor_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>          // RolePacket
#include <serial/wire.h>           // SfxWire
#include <serial/core/core.h>      // SerialError / PortError / RoleError

#include <variant>

namespace sfx_core {

bool BiMotorRoleHandler::attach(HBridgeBinding& b, uint8_t portIdx,
                                const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<BiDcMotorRole>(b.port, b.iSense, b.vSense);

    // Attach config layout — Rule 11 append-only.  Older masters send
    // only the first 4 bytes (Fixed-mode threshold + window); a Strategy A
    // master appends the LiveRatio config + initial position.
    //
    //   [0..3]  Fixed-mode:  threshold_mA:u16LE | window_ms:u16LE
    //   [4]     guardMode:u8  (0=Fixed, 1=LiveRatio)  -- Rule 11 ext
    //   [5..6]  a:u16LE       -- LiveRatio: ratio_x100 (e.g. 250 = 2.5×)
    //   [7..8]  b:u16LE       -- LiveRatio: runSample_ms
    //   [9..10] c:u16LE       -- LiveRatio: inrushBlank_ms
    //   [11..12] d:u16LE      -- LiveRatio: maxTravel_ms (failsafe, 0 = none)
    //   [13]    initialPosition:u8  (0=Unknown, 1=A, 2=B) -- Strategy A restore
    if (cfgLen >= 4) {
        const uint16_t th = SfxWire::getU16LE(&cfg[0]);
        const uint16_t wn = SfxWire::getU16LE(&cfg[2]);
        role.setStallGuard(th, wn);
    }
    if (cfgLen >= 13) {
        const uint8_t  mode = cfg[4];
        const uint16_t a    = SfxWire::getU16LE(&cfg[5]);
        const uint16_t bv   = SfxWire::getU16LE(&cfg[7]);
        const uint16_t cv   = SfxWire::getU16LE(&cfg[9]);
        const uint16_t dv   = SfxWire::getU16LE(&cfg[11]);
        if (mode == 1) role.setStallGuardRatio(a, bv, cv, dv);
        // mode == 0 keeps Fixed-mode config from the first 4 bytes.
    }
    if (cfgLen >= 14) {
        role.setPosition(static_cast<BiDcMotorRole::Position>(cfg[13]));
    }

    role.onStall([this, portIdx](uint16_t peak, uint16_t dur) {
        _emit->emitBiMotorStallEvent(portIdx, peak, dur);
    });
    role.onEndstopResult([this, portIdx](uint8_t outcome, uint16_t travel, uint16_t peak,
                                         BiDcMotorRole::Position pos) {
        _emit->emitBiMotorEndstopResult(portIdx, outcome, travel, peak,
                                        static_cast<uint8_t>(pos));
    });
    return true;
}

void BiMotorRoleHandler::handleSetSigned(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->setSigned((int16_t)SfxWire::getI16LE(&p[1]));
    _ctx->sendAck();
}

void BiMotorRoleHandler::handleBrake(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->brake();
    _ctx->sendAck();
}

void BiMotorRoleHandler::handleCoast(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    r->coast();
    _ctx->sendAck();
}

void BiMotorRoleHandler::handleGetStatus(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->hbridgeAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    // Rule 11 extension: append [position][guardMode] tail.  Older
    // masters parse only the first 8 bytes; newer masters see the
    // Strategy A position + active stall-detection mode.
    uint8_t out[10];
    out[0] = idx;
    SfxWire::putI16LE(&out[1], r->signedDuty());
    SfxWire::putI16LE(&out[3], r->voltage_mV());
    SfxWire::putI16LE(&out[5], r->current_mA());
    out[7] = r->stalled() ? 1 : 0;
    out[8] = static_cast<uint8_t>(r->position());
    out[9] = static_cast<uint8_t>(r->guardMode());
    _ctx->sendRawPacket(RolePacket::BIMOTOR_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

void BiMotorRoleHandler::handleSeekEndstop(const uint8_t* p, size_t len) {
    if (len < 5) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    const int16_t  duty    = (int16_t)SfxWire::getI16LE(&p[1]);
    const uint16_t timeout = SfxWire::getU16LE(&p[3]);   // 0 = no timeout
    r->seekEndstop(duty, timeout);
    _ctx->sendAck();
}

// Strategy A move-to-end: like SEEK_ENDSTOP but records `endLabel` as
// the destination so the role's position state advances on Reached.
// `signed_duty == 0` is the position-restore special case (no motion).
void BiMotorRoleHandler::handleMoveToEnd(const uint8_t* p, size_t len) {
    if (len < 6) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    const auto    end     = static_cast<BiDcMotorRole::Position>(p[1]);
    const int16_t duty    = (int16_t)SfxWire::getI16LE(&p[2]);
    const uint16_t timeout = SfxWire::getU16LE(&p[4]);
    r->moveToEnd(end, duty, timeout);
    _ctx->sendAck();
}

// Live retune of stall-guard mode + parameters.  Used by the Studio
// calibration dialog when the operator switches between Fixed and
// LiveRatio without wanting to re-attach the role (which would erase
// the cached position + any in-progress seek state).
void BiMotorRoleHandler::handleSetGuard(const uint8_t* p, size_t len) {
    if (len < 12) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    auto* b = _reg->hbridgeAt(p[0]);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<BiDcMotorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }
    const uint8_t  mode      = p[1];
    const uint16_t window_ms = SfxWire::getU16LE(&p[2]);
    const uint16_t a         = SfxWire::getU16LE(&p[4]);
    const uint16_t bv        = SfxWire::getU16LE(&p[6]);
    const uint16_t cv        = SfxWire::getU16LE(&p[8]);
    const uint16_t dv        = SfxWire::getU16LE(&p[10]);
    if (mode == 0) {
        r->setStallGuard(a, window_ms != 0 ? window_ms : r->windowMs());
    } else if (mode == 1) {
        r->setStallGuardRatio(a, bv, cv, dv);
        if (window_ms != 0) r->setStallWindowMs(window_ms);
    } else {
        _ctx->sendNack(RoleError::ROLE_CONFIG_INVALID);
        return;
    }
    // Rule 11 append: optional absolute over-current ceiling at [12:14]
    // (LiveRatio backstop; 0 = none).  Old clients omit it.
    if (len >= 14) r->setAbsoluteCeiling(SfxWire::getU16LE(&p[12]));
    _ctx->sendAck();
}

}  // namespace sfx_core
