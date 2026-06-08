/*
 * ServoRoleHandler implementation — bodies moved verbatim from the former
 * RoleServicePolicy servo handlers; emit*() calls now go through the bound
 * RoleEventEmitter (_emit->...).
 */

#include "role_servo_handler.h"
#include "role_event_emitter.h"
#include "board_server.h"
#include <serial/roles.h>                 // RolePacket
#include <serial/wire.h>                  // SfxWire
#include <serial/core/core.h>             // SerialError / PortError / RoleError
#include <motion/servo_profile_wire.h>    // the one servo-profile wire codec
#include <platform/sfx_platform.h>        // SFX_MILLIS()

#include <variant>

namespace sfx_core {

bool ServoRoleHandler::attach(ServoBinding& b, uint8_t portIdx,
                              const uint8_t* cfg, size_t cfgLen) {
    auto& role = b.role.emplace<ServoActuatorRole>(b.port);
    // Rule 42 storage + Rule 44 editing-surface: the per-port profile travels
    // with the role-attach payload from /hubfx.yaml's ports[] block, decoded by
    // the ONE ServoProfileWire codec shared with the pack site + SERVO_SET_PROFILE
    // (Rule 11 append-only). Seed from the role's port-default profile, overlay
    // the wire fields, then push limits + REV + profile in lock-step.
    ServoMotionProfile prof = role.profile();          // initFromPort defaults
    ServoProfileWire::unpack(cfg, cfgLen, prof);
    role.setLimits(prof.minUs, prof.maxUs);
    role.setReversed(prof.inverted);
    role.setProfile(prof);
    role.onTargetReached([this, portIdx](uint16_t pos) { _emit->emitServoTargetReached(portIdx, pos); });
    // SERVO_MOTION_DONE — monitored completion for gear door sequencing
    // (instructions/29 decision #1).  Same rising edge as TARGET_REACHED;
    // a separate, lighter [portIdx] async the hub gear service routes to
    // the right Gear's DoorSequencer.
    role.onMotionDone([this, portIdx]() { _emit->emitServoMotionDone(portIdx); });
    SFX_LOG_INFO("[servo] attach idx=%u  min=%u max=%u rev=%u  (cfgLen=%u)",
                 (unsigned)portIdx, (unsigned)role.profile().minUs,
                 (unsigned)role.profile().maxUs, (unsigned)prof.inverted, (unsigned)cfgLen);
    return true;
}

void ServoRoleHandler::handleSetTarget(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx    = p[0];
    const uint16_t target = SfxWire::getU16LE(&p[1]);
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (auto* r = std::get_if<ServoActuatorRole>(&b->role)) {
        r->setTarget(target);
        _ctx->sendAck();
    } else {
        _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH);
    }
}

void ServoRoleHandler::handleSetPosNorm(const uint8_t* p, size_t len) {
    if (len < 3) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx = p[0];
    const uint16_t pos = SfxWire::getU16LE(&p[1]);
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (auto* r = std::get_if<ServoActuatorRole>(&b->role)) {
        r->setNormalizedTarget(pos);   // 0..kPosNormFull → live calibrated [min,max]
        // Log only the endpoint commands (landing deploy/retract) — gunfx
        // streams intermediate positions and would flood the diag.
        if (pos == 0 || pos == RolePacket::kPosNormFull) {
            SFX_LOG_INFO("[servo] pos_norm idx=%u  pos=%u  → target=%u  (min=%u max=%u)",
                         (unsigned)idx, (unsigned)pos, (unsigned)r->target(),
                         (unsigned)r->profile().minUs, (unsigned)r->profile().maxUs);
        }
        _ctx->sendAck();
    } else {
        _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH);
    }
}

void ServoRoleHandler::handleRecoil(const uint8_t* p, size_t len) {
    // Add a transient recoil offset on top of the aim for `durationMs`, then
    // auto-remove ("de-jerk"). The motion profile keeps tracking its target
    // underneath, so the kick works whether the servo is moving or stationary.
    // GunFx calls this once per shot with a random ± offset.
    // Payload: [portIdx:u8][offsetUs:i16LE][durationMs:u16LE]
    if (len < 5) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t  idx        = p[0];
    const int16_t  offsetUs   = SfxWire::getI16LE(&p[1]);
    const uint16_t durationMs = SfxWire::getU16LE(&p[3]);
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    if (auto* r = std::get_if<ServoActuatorRole>(&b->role)) {
        r->applyRecoil(offsetUs, durationMs);
        _ctx->sendAck();
    } else {
        _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH);
    }
}

void ServoRoleHandler::handleGetStatusReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<ServoActuatorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    uint8_t out[8];
    out[0] = idx;
    SfxWire::putU16LE(&out[1], r->position());
    SfxWire::putU16LE(&out[3], r->target());
    SfxWire::putI16LE(&out[5], r->velocity_us_per_s());
    out[7] = r->atTarget() ? 1 : 0;
    _ctx->sendRawPacket(RolePacket::SERVO_STATUS_RESP, _ctx->currentTag(), out, sizeof out);
}

// Live motion-profile retune (Phase 2.9.x — Rule 42).  Same wire shape
// as the role-attach payload tail; the role's `setLimits` + `setReversed`
// + `setProfile` together replace any in-flight slew with the new shape.
// In-flight `target_us` is preserved (clamped into the new range).
void ServoRoleHandler::handleSetProfile(const uint8_t* p, size_t len) {
    if (len < 14) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<ServoActuatorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    // Same ServoProfileWire codec as the role-attach path (profile body starts
    // at p[1], after the portIdx). Seed from the live profile, overlay the wire
    // fields, push limits + REV + profile in lock-step.
    ServoMotionProfile prof = r->profile();
    ServoProfileWire::unpack(&p[1], len - 1, prof);
    r->setLimits(prof.minUs, prof.maxUs);
    r->setReversed(prof.inverted);
    r->setProfile(prof);
    SFX_LOG_INFO("[servo] setprofile idx=%u  min=%u max=%u rev=%u  (live calibrate)",
                 (unsigned)idx, (unsigned)r->profile().minUs,
                 (unsigned)r->profile().maxUs, (unsigned)prof.inverted);
    _ctx->sendAck();
}

void ServoRoleHandler::handleGetProfileReq(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    const uint8_t idx = p[0];
    auto* b = _reg->servoAt(idx);
    if (!b || !b->occupied()) { _ctx->sendNack(PortError::PORT_NOT_FOUND); return; }
    auto* r = std::get_if<ServoActuatorRole>(&b->role);
    if (!r) { _ctx->sendNack(RoleError::ROLE_KIND_MISMATCH); return; }

    const ServoMotionProfile& prof = r->profile();
    uint8_t out[14];
    out[0] = idx;
    SfxWire::putU16LE(&out[1],  prof.minUs);
    SfxWire::putU16LE(&out[3],  prof.maxUs);
    SfxWire::putU16LE(&out[5],  prof.maxSpeedUsPerSec);
    out[7] = prof.inverted ? 1 : 0;
    SfxWire::putU16LE(&out[8],  prof.centerUs);
    SfxWire::putU16LE(&out[10], prof.maxAccelUsPerSec2);
    SfxWire::putU16LE(&out[12], prof.maxJerkUsPerSec3);
    _ctx->sendRawPacket(RolePacket::SERVO_PROFILE_RESP, _ctx->currentTag(), out, sizeof out);
}

void ServoRoleHandler::handleSetBroadcastHz(const uint8_t* p, size_t len) {
    if (len < 1) { _ctx->sendNack(SerialError::MISSING_PARAMETER); return; }
    uint8_t hz = p[0];
    if (hz > kBroadcastMaxHz) hz = kBroadcastMaxHz;
    _broadcastHz     = hz;
    _broadcastNextMs = SFX_MILLIS();   // emit promptly on (re)subscribe
    _ctx->sendAck();
}

void ServoRoleHandler::maybeBroadcast(uint32_t now) {
    // Generic servo telemetry — one batched snapshot of every active servo at
    // the host-requested rate, gated on a listening host.  Upload-safe: the
    // whole update() is skipped while the loop is upload-exclusive.
    if (_broadcastHz > 0 && (int32_t)(now - _broadcastNextMs) >= 0) {
        _broadcastNextMs = now + (uint32_t)(1000u / _broadcastHz);
        if (_ctx && _ctx->hostVerboseActive()) emitBroadcast(now);
    }
}

void ServoRoleHandler::emitBroadcast(uint32_t now) {
    (void)now;
    if (!_reg) return;
    // [count:u8]{ [portIdx:u8][pos:u16][target:u16][vel:i16] } × count.
    // Buffer sized for the worst case (numServoPorts ≤ the board's servo count;
    // HubFX has ≤ 10 → 1 + 10·7 = 71 B, well under the payload cap).
    uint8_t buf[1 + 32 * 7];
    size_t  off = 1;
    uint8_t count = 0;
    const uint8_t n = _reg->numServoPorts();
    for (uint8_t i = 0; i < n && count < 32; i++) {
        auto* b = _reg->servoAt(i);
        if (!b || !b->occupied()) continue;
        auto* r = std::get_if<ServoActuatorRole>(&b->role);
        if (!r) continue;
        if (off + 7 > sizeof buf) break;
        buf[off++] = i;
        SfxWire::putU16LE(&buf[off], r->position());          off += 2;
        SfxWire::putU16LE(&buf[off], r->target());            off += 2;
        SfxWire::putI16LE(&buf[off], r->velocity_us_per_s()); off += 2;
        count++;
    }
    if (count == 0) return;            // nothing to report — stay off the wire
    buf[0] = count;
    if (_ctx) _ctx->sendRawPacket(RolePacket::SERVO_MOTION_UPDATE,
                                  SfxWire::TAG_ASYNC, buf, off);
    _emit->fireLocalAsync(RolePacket::SERVO_MOTION_UPDATE, buf, off);
}

}  // namespace sfx_core
